#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <numbers>
#include <string>
#include <thread>

#include <mr/kitti.hpp>

namespace mr {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t to_ns(Clock::time_point t) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// OXTS field indices (see KITTI devkit readme, dataformat.txt).
enum OxtsField : std::size_t {
  kLat = 0,
  kLon = 1,
  kAlt = 2,
  kYaw = 5,
  kVf = 8,
  kAf = 14,
  kAl = 15,
  kAu = 16,
  kWf = 20,
  kWl = 21,
  kWu = 22,
  kNumSats = 26,
  kFieldCount = 30,
};

template <typename T>
bool parse_number(std::string_view text, T& out) {
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc() && ptr == text.data() + text.size();
}

}  // namespace

Result<OxtsSample> parse_oxts_line(std::string_view line) {
  std::array<double, kFieldCount> v{};
  std::size_t n = 0;
  std::size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r')) {
      ++pos;
    }
    if (pos >= line.size()) {
      break;
    }
    const std::size_t end = std::min(line.find_first_of(" \t\r", pos), line.size());
    if (n >= v.size() || !parse_number(line.substr(pos, end - pos), v[n])) {
      return make_error(Errc::kInvalidArgument, std::format("bad OXTS field {}", n));
    }
    ++n;
    pos = end;
  }
  if (n != kFieldCount) {
    return make_error(Errc::kInvalidArgument,
                      std::format("OXTS line has {} fields, expected 30", n));
  }
  OxtsSample s;
  s.gps.latitude_deg = v[kLat];
  s.gps.longitude_deg = v[kLon];
  s.gps.altitude_m = v[kAlt];
  s.gps.speed_mps = static_cast<float>(v[kVf]);
  s.gps.heading_deg = static_cast<float>(v[kYaw] * 180.0 / std::numbers::pi);
  s.gps.fix_type = 3;
  s.gps.satellites = static_cast<std::uint8_t>(std::clamp(v[kNumSats], 0.0, 255.0));
  s.imu = ImuSample{v[kAf], v[kAl], v[kAu], v[kWf], v[kWl], v[kWu]};
  return s;
}

Result<std::int64_t> parse_kitti_timestamp(std::string_view text) {
  // "2011-09-26 13:02:25.964389445"
  if (text.size() < 19 || text[10] != ' ' || text[13] != ':' || text[16] != ':') {
    return make_error(Errc::kInvalidArgument, "bad KITTI timestamp '" + std::string(text) + "'");
  }
  int hh = 0;
  int mm = 0;
  int ss = 0;
  if (!parse_number(text.substr(11, 2), hh) || !parse_number(text.substr(14, 2), mm) ||
      !parse_number(text.substr(17, 2), ss)) {
    return make_error(Errc::kInvalidArgument, "bad KITTI timestamp '" + std::string(text) + "'");
  }
  std::int64_t frac_ns = 0;
  if (text.size() > 20 && text[19] == '.') {
    std::string_view frac = text.substr(20);
    while (!frac.empty() && (frac.back() == '\r' || frac.back() == ' ')) {
      frac.remove_suffix(1);
    }
    if (frac.empty() || frac.size() > 9 || !parse_number(frac, frac_ns)) {
      return make_error(Errc::kInvalidArgument, "bad KITTI timestamp fraction");
    }
    for (std::size_t i = frac.size(); i < 9; ++i) {
      frac_ns *= 10;
    }
  }
  return ((static_cast<std::int64_t>(hh) * 60 + mm) * 60 + ss) * 1'000'000'000LL + frac_ns;
}

Result<std::vector<OxtsSample>> load_oxts_dir(const std::filesystem::path& dir) {
  std::ifstream ts_file(dir / "timestamps.txt");
  if (!ts_file) {
    return make_error(Errc::kNotFound, "missing " + (dir / "timestamps.txt").string());
  }
  std::vector<OxtsSample> samples;
  std::string line;
  std::int64_t first = 0;
  for (std::size_t i = 0; std::getline(ts_file, line); ++i) {
    if (line.empty()) {
      continue;
    }
    auto ts = parse_kitti_timestamp(line);
    if (!ts) {
      return std::move(ts).error();
    }
    const auto data_path = dir / "data" / std::format("{:010}.txt", i);
    std::ifstream data(data_path);
    std::string data_line;
    if (!data || !std::getline(data, data_line)) {
      return make_error(Errc::kNotFound, "missing or empty " + data_path.string());
    }
    auto sample = parse_oxts_line(data_line);
    if (!sample) {
      return make_error(Errc::kInvalidArgument, data_path.string() + ": " + sample.error().message);
    }
    if (samples.empty()) {
      first = ts.value();
    }
    sample->timestamp_ns = ts.value() - first;
    samples.push_back(sample.value());
  }
  if (samples.empty()) {
    return make_error(Errc::kNotFound, "no OXTS samples in " + dir.string());
  }
  return samples;
}

KittiOxtsReplay::KittiOxtsReplay(KittiReplayConfig config, std::vector<OxtsSample> samples,
                                 RecordRing& imu_ring, RecordRing& gps_ring)
    : config_(config), samples_(std::move(samples)), imu_ring_(&imu_ring), gps_ring_(&gps_ring) {}

void KittiOxtsReplay::run(const std::stop_token& stop, Clock::time_point start) {
  if (samples_.empty()) {
    return;
  }
  const std::int64_t start_ns = to_ns(start);
  // Duration of one pass plus one nominal sample period, so looped passes do
  // not repeat a timestamp.
  const std::int64_t period =
      samples_.size() > 1 ? samples_[1].timestamp_ns - samples_[0].timestamp_ns : 100'000'000;
  const std::int64_t pass_ns = samples_.back().timestamp_ns + std::max<std::int64_t>(period, 1);
  const double scale = std::max(config_.rate_scale, 1e-3);
  std::uint64_t seq = 0;
  for (std::int64_t pass = 0; !stop.stop_requested(); ++pass) {
    for (const OxtsSample& s : samples_) {
      if (stop.stop_requested()) {
        return;
      }
      const std::int64_t virtual_ns = pass * pass_ns + s.timestamp_ns;
      if (!config_.unpaced) {
        const auto deadline = start + std::chrono::nanoseconds(static_cast<std::int64_t>(
                                          static_cast<double>(virtual_ns) / scale));
        while (!stop.stop_requested() && Clock::now() < deadline) {
          std::this_thread::sleep_until(
              std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
        }
      }
      const std::int64_t ts = start_ns + virtual_ns;
      const std::int64_t now = to_ns(Clock::now());
      const auto r1 = imu_ring_->push_with(
          {.timestamp_ns = ts, .seq = seq, .stream_id = config_.imu_stream}, now, ImuSample::kSize,
          [&s](MutableByteSpan out) { encode(s.imu, out); });
      const auto r2 = gps_ring_->push_with(
          {.timestamp_ns = ts, .seq = seq, .stream_id = config_.gps_stream}, now, GpsFix::kSize,
          [&s](MutableByteSpan out) { encode(s.gps, out); });
      if (r1 == PushResult::kStopped || r2 == PushResult::kStopped) {
        return;
      }
      ++seq;
      emitted_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!config_.loop) {
      return;
    }
  }
}

}  // namespace mr
