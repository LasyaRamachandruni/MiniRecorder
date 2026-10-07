#include <mr/sensors.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>

namespace mr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kGravity = 9.81;
constexpr double kMetersPerDegLat = 111'320.0;

std::int64_t to_ns(Clock::time_point t) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// Sleeps until `deadline` in slices so a stop request is honoured within
// ~20 ms even for slow sensors or long injected stalls.
void sleep_until_or_stop(Clock::time_point deadline, const std::stop_token& stop) {
  constexpr auto kSlice = std::chrono::milliseconds(20);
  while (!stop.stop_requested()) {
    const auto now = Clock::now();
    if (now >= deadline) {
      return;
    }
    std::this_thread::sleep_until(std::min(deadline, now + kSlice));
  }
}

class ImuGenerator final : public SignalGenerator {
 public:
  ImuGenerator(const VehicleProfile& p, std::uint64_t seed) : profile_(p), rng_(seed) {}
  [[nodiscard]] std::size_t payload_size() const noexcept override { return ImuSample::kSize; }
  void generate(double t_s, std::uint64_t /*seq*/, MutableByteSpan out) override {
    const VehicleState s = vehicle_state(profile_, t_s);
    const ImuSample sample{
        .accel_x = s.accel_mps2 + 0.05 * noise_(rng_),
        .accel_y = s.speed_mps * s.speed_mps / profile_.track_radius_m + 0.05 * noise_(rng_),
        .accel_z = kGravity + 0.02 * noise_(rng_),
        .gyro_x = 0.002 * noise_(rng_),
        .gyro_y = 0.002 * noise_(rng_),
        .gyro_z = s.yaw_rate_rps + 0.001 * noise_(rng_),
    };
    encode(sample, out);
  }

 private:
  VehicleProfile profile_;
  std::mt19937_64 rng_;
  std::normal_distribution<double> noise_{0.0, 1.0};
};

class GpsGenerator final : public SignalGenerator {
 public:
  GpsGenerator(const VehicleProfile& p, std::uint64_t seed) : profile_(p), rng_(seed) {}
  [[nodiscard]] std::size_t payload_size() const noexcept override { return GpsFix::kSize; }
  void generate(double t_s, std::uint64_t /*seq*/, MutableByteSpan out) override {
    const VehicleState s = vehicle_state(profile_, t_s);
    const GpsFix fix{
        .latitude_deg = s.latitude_deg + 1e-6 * noise_(rng_),
        .longitude_deg = s.longitude_deg + 1e-6 * noise_(rng_),
        .altitude_m = 25.0 + 0.3 * noise_(rng_),
        .speed_mps = static_cast<float>(s.speed_mps + 0.05 * noise_(rng_)),
        .heading_deg = static_cast<float>(s.heading_deg),
        .fix_type = 3,
        .satellites = static_cast<std::uint8_t>(9 + (rng_() % 3)),
    };
    encode(fix, out);
  }

 private:
  VehicleProfile profile_;
  std::mt19937_64 rng_;
  std::normal_distribution<double> noise_{0.0, 1.0};
};

class WheelGenerator final : public SignalGenerator {
 public:
  WheelGenerator(const VehicleProfile& p, std::uint64_t seed) : profile_(p), rng_(seed) {}
  [[nodiscard]] std::size_t payload_size() const noexcept override { return WheelSpeedFrame::kSize; }
  void generate(double t_s, std::uint64_t /*seq*/, MutableByteSpan out) override {
    const VehicleState s = vehicle_state(profile_, t_s);
    constexpr double kHalfTrack = 0.8;  // m
    const double inner = 1.0 - kHalfTrack / profile_.track_radius_m;
    const double outer = 1.0 + kHalfTrack / profile_.track_radius_m;
    const std::array<double, 4> factors{inner, outer, inner, outer};
    WheelSpeedFrame frame;
    for (std::size_t i = 0; i < 4; ++i) {
      const double centi_kph = s.speed_mps * factors[i] * 3.6 * 100.0 + 2.0 * noise_(rng_);
      frame.speed_centi_kph[i] = static_cast<std::uint16_t>(std::clamp(centi_kph, 0.0, 65535.0));
    }
    encode(frame, out);
  }

 private:
  VehicleProfile profile_;
  std::mt19937_64 rng_;
  std::normal_distribution<double> noise_{0.0, 1.0};
};

class CameraGenerator final : public SignalGenerator {
 public:
  CameraGenerator(std::uint16_t width, std::uint16_t height, std::uint64_t seed)
      : width_(width),
        height_(height),
        tex_w_(std::size_t{2} * width),
        tex_h_(std::size_t{2} * height),
        noise_state_(seed | 1U) {
    build_texture(seed);
  }

  [[nodiscard]] std::size_t payload_size() const noexcept override {
    return CameraFrameHeader::kSize + std::size_t{width_} * height_;
  }

  void generate(double /*t_s*/, std::uint64_t seq, MutableByteSpan out) override {
    encode(CameraFrameHeader{.width = width_, .height = height_, .format = 0, .frame_id = seq}, out);
    // Pan across the texture to emulate forward motion.
    const std::size_t ox = (seq * 5) % width_;
    const std::size_t oy = (seq * 2) % height_;
    std::byte* dst = out.data() + CameraFrameHeader::kSize;
    for (std::size_t y = 0; y < height_; ++y) {
      const std::uint8_t* src = texture_.data() + (y + oy) * tex_w_ + ox;
      std::size_t x = 0;
      while (x < width_) {
        std::uint64_t bits = next_noise();
        // 16 pixels of noise per 64-bit draw: mostly 0, sometimes +-1 or +-2
        // digital numbers, roughly the read noise of a well-lit sensor.
        for (int k = 0; k < 16 && x < width_; ++k, ++x) {
          const int noise = kNoiseTable[bits & 0xFU];
          bits >>= 4;
          dst[y * width_ + x] = static_cast<std::byte>(std::clamp(src[x] + noise, 0, 255));
        }
      }
    }
  }

 private:
  static constexpr std::array<int, 16> kNoiseTable{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, -1, -1, -1, 2, -2};

  // xorshift64*: fast, good enough for sensor noise.
  std::uint64_t next_noise() noexcept {
    noise_state_ ^= noise_state_ >> 12;
    noise_state_ ^= noise_state_ << 25;
    noise_state_ ^= noise_state_ >> 27;
    return noise_state_ * 0x2545F4914F6CDD1DULL;
  }

  void build_texture(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    texture_.resize(tex_w_ * tex_h_);
    // Two octaves of bilinear value noise plus a vertical gradient (bright
    // "sky" at the top, darker "road" at the bottom).
    struct Octave {
      std::size_t cell;
      double weight;
    };
    const std::array<Octave, 3> octaves{{{64, 0.6}, {16, 0.3}, {4, 0.1}}};
    std::vector<double> acc(tex_w_ * tex_h_, 0.0);
    for (const Octave& o : octaves) {
      const std::size_t gw = tex_w_ / o.cell + 2;
      const std::size_t gh = tex_h_ / o.cell + 2;
      std::vector<double> grid(gw * gh);
      for (double& g : grid) {
        g = static_cast<double>(rng() % 256);
      }
      for (std::size_t y = 0; y < tex_h_; ++y) {
        const std::size_t gy = y / o.cell;
        double fy = static_cast<double>(y % o.cell) / static_cast<double>(o.cell);
        fy = fy * fy * (3 - 2 * fy);
        for (std::size_t x = 0; x < tex_w_; ++x) {
          const std::size_t gx = x / o.cell;
          double fx = static_cast<double>(x % o.cell) / static_cast<double>(o.cell);
          fx = fx * fx * (3 - 2 * fx);
          const double a = grid[gy * gw + gx];
          const double b = grid[gy * gw + gx + 1];
          const double c = grid[(gy + 1) * gw + gx];
          const double d = grid[(gy + 1) * gw + gx + 1];
          const double top = a + (b - a) * fx;
          const double bottom = c + (d - c) * fx;
          acc[y * tex_w_ + x] += o.weight * (top + (bottom - top) * fy);
        }
      }
    }
    for (std::size_t y = 0; y < tex_h_; ++y) {
      const double gradient = 50.0 - 100.0 * static_cast<double>(y % height_) / static_cast<double>(height_);
      for (std::size_t x = 0; x < tex_w_; ++x) {
        const double v = acc[y * tex_w_ + x] * 0.7 + 40.0 + gradient;
        texture_[y * tex_w_ + x] = static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0));
      }
    }
  }

  std::uint16_t width_;
  std::uint16_t height_;
  std::size_t tex_w_;
  std::size_t tex_h_;
  std::vector<std::uint8_t> texture_;
  std::uint64_t noise_state_;
};

}  // namespace

VehicleState vehicle_state(const VehicleProfile& p, double t_s) noexcept {
  const double cycle = std::max(p.cycle_s, 1e-3);
  double tau = std::fmod(t_s, cycle);
  if (tau < 0) {
    tau += cycle;
  }
  const double brake_start = std::clamp(p.brake_start_s, 1e-3, cycle);
  const double v_low = std::max(0.0, p.cruise_mps - p.brake_decel_mps2 * p.brake_duration_s);
  const double recover_s = (p.cruise_mps - v_low) / std::max(p.recover_accel_mps2, 1e-6);

  VehicleState s;
  constexpr double kWobble = 0.8;  // m/s speed variation while cruising
  if (tau < brake_start) {
    const double w = 2.0 * std::numbers::pi / brake_start;
    s.speed_mps = p.cruise_mps + kWobble * std::sin(w * tau);
    s.accel_mps2 = kWobble * w * std::cos(w * tau);
  } else if (tau < brake_start + p.brake_duration_s) {
    s.speed_mps = std::max(0.0, p.cruise_mps - p.brake_decel_mps2 * (tau - brake_start));
    s.accel_mps2 = s.speed_mps > 0.0 ? -p.brake_decel_mps2 : 0.0;
  } else if (tau < brake_start + p.brake_duration_s + recover_s) {
    s.speed_mps = v_low + p.recover_accel_mps2 * (tau - brake_start - p.brake_duration_s);
    s.accel_mps2 = p.recover_accel_mps2;
  } else {
    s.speed_mps = p.cruise_mps;
    s.accel_mps2 = 0.0;
  }
  s.yaw_rate_rps = s.speed_mps / p.track_radius_m;

  // Position on a circle; uses the mean speed so it stays a closed-form
  // function of time (the simulator does not integrate speed).
  const double theta = p.cruise_mps * t_s / p.track_radius_m;
  const double lat0_rad = p.origin_lat_deg * std::numbers::pi / 180.0;
  s.latitude_deg = p.origin_lat_deg + p.track_radius_m * std::sin(theta) / kMetersPerDegLat;
  s.longitude_deg = p.origin_lon_deg +
                    p.track_radius_m * (1.0 - std::cos(theta)) / (kMetersPerDegLat * std::cos(lat0_rad));
  s.heading_deg = std::fmod(theta * 180.0 / std::numbers::pi, 360.0);
  return s;
}

std::unique_ptr<SignalGenerator> make_imu_generator(const VehicleProfile& p, std::uint64_t seed) {
  return std::make_unique<ImuGenerator>(p, seed);
}
std::unique_ptr<SignalGenerator> make_gps_generator(const VehicleProfile& p, std::uint64_t seed) {
  return std::make_unique<GpsGenerator>(p, seed);
}
std::unique_ptr<SignalGenerator> make_wheel_generator(const VehicleProfile& p, std::uint64_t seed) {
  return std::make_unique<WheelGenerator>(p, seed);
}
std::unique_ptr<SignalGenerator> make_camera_generator(std::uint16_t width, std::uint16_t height,
                                                       std::uint64_t seed) {
  return std::make_unique<CameraGenerator>(width, height, seed);
}

// ---------------------------------------------------------------- producer

SensorProducer::SensorProducer(const SensorConfig& config, std::unique_ptr<SignalGenerator> generator,
                               RecordRing& ring)
    : config_(config), generator_(std::move(generator)), ring_(&ring), rng_(config_.seed) {}

void SensorProducer::run(const std::stop_token& stop, Clock::time_point start) {
  const double rate = std::max(config_.rate_hz, 1e-3);
  const auto period_ns = static_cast<std::int64_t>(1e9 / rate);
  const double scale = std::max(config_.rate_scale, 1e-3);
  const std::int64_t start_ns = to_ns(start);
  const double max_jitter = static_cast<double>(period_ns) / 4.0;
  std::normal_distribution<double> jitter(0.0, config_.jitter_fraction * static_cast<double>(period_ns));
  std::uniform_real_distribution<double> uniform(0.0, 1.0);
  const FaultConfig& faults = config_.faults;
  const std::size_t size = generator_->payload_size();

  std::uint64_t seq = 0;
  std::uint64_t k = 0;  // sample slot index on the nominal timeline
  while (!stop.stop_requested()) {
    std::uint16_t flags = 0;
    if (faults.gap_probability > 0 && uniform(rng_) < faults.gap_probability) {
      const std::uint64_t skipped = 1 + rng_() % 3;
      seq += skipped;
      k += skipped;
      gaps_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (config_.pacing == Pacing::kRealTime) {
      const auto offset = std::chrono::nanoseconds(
          static_cast<std::int64_t>(static_cast<double>(k) * static_cast<double>(period_ns) / scale));
      sleep_until_or_stop(start + offset, stop);
    }
    if (faults.stall_probability > 0 && uniform(rng_) < faults.stall_probability) {
      sleep_until_or_stop(Clock::now() + faults.stall_duration, stop);
      stalls_.fetch_add(1, std::memory_order_relaxed);
      flags |= record_flags::kInjectedFault;
    }
    if (stop.stop_requested()) {
      break;
    }

    std::int64_t ts = start_ns + static_cast<std::int64_t>(k) * period_ns +
                      static_cast<std::int64_t>(std::clamp(jitter(rng_), -max_jitter, max_jitter));
    if (faults.regression_probability > 0 && uniform(rng_) < faults.regression_probability) {
      ts -= static_cast<std::int64_t>(faults.regression_periods * static_cast<double>(period_ns));
      regressions_.fetch_add(1, std::memory_order_relaxed);
      flags |= record_flags::kInjectedFault;
    }

    const RecordMeta meta{.timestamp_ns = ts, .seq = seq, .stream_id = config_.stream, .flags = flags};
    const double t_s = static_cast<double>(k) / rate;
    const PushResult r = ring_->push_with(meta, to_ns(Clock::now()), size, [&](MutableByteSpan out) {
      generator_->generate(t_s, seq, out);
    });
    if (r == PushResult::kStopped) {
      break;
    }
    emitted_.fetch_add(1, std::memory_order_relaxed);
    ++seq;
    ++k;
  }
}

SensorCounters SensorProducer::counters() const noexcept {
  return {emitted_.load(std::memory_order_relaxed), gaps_.load(std::memory_order_relaxed),
          regressions_.load(std::memory_order_relaxed), stalls_.load(std::memory_order_relaxed)};
}

}  // namespace mr
