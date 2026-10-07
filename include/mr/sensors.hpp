#pragma once

// Simulated vehicle sensors. All signals are deterministic functions of the
// sample time (plus seeded noise), so independent sensor threads describe a
// consistent vehicle without sharing state.
//
// The data is SYNTHETIC. The vehicle drives a circle at ~15 m/s and performs
// a hard brake (about -7 m/s^2 for 2 s) once per cycle, which gives the event
// trigger something real to detect.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <stop_token>
#include <string>
#include <vector>

#include <mr/record_ring.hpp>
#include <mr/sensor_types.hpp>

namespace mr {

struct VehicleProfile {
  double cycle_s = 30.0;        // driving pattern repeats with this period
  double brake_start_s = 20.0;  // hard brake begins at this offset within a cycle
  double brake_decel_mps2 = 7.0;
  double brake_duration_s = 2.0;
  double cruise_mps = 15.0;
  double recover_accel_mps2 = 2.0;
  double track_radius_m = 400.0;
  double origin_lat_deg = 37.3352;  // San Jose, CA
  double origin_lon_deg = -121.8811;
};

struct VehicleState {
  double speed_mps = 0;
  double accel_mps2 = 0;  // longitudinal
  double yaw_rate_rps = 0;
  double latitude_deg = 0;
  double longitude_deg = 0;
  double heading_deg = 0;
};

[[nodiscard]] VehicleState vehicle_state(const VehicleProfile& profile, double t_s) noexcept;

// Produces one payload for a sample at virtual time t (seconds since start).
class SignalGenerator {
 public:
  SignalGenerator() = default;
  SignalGenerator(const SignalGenerator&) = delete;
  SignalGenerator& operator=(const SignalGenerator&) = delete;
  SignalGenerator(SignalGenerator&&) = delete;
  SignalGenerator& operator=(SignalGenerator&&) = delete;
  virtual ~SignalGenerator() = default;

  [[nodiscard]] virtual std::size_t payload_size() const noexcept = 0;
  virtual void generate(double t_s, std::uint64_t seq, MutableByteSpan out) = 0;
};

[[nodiscard]] std::unique_ptr<SignalGenerator> make_imu_generator(const VehicleProfile& p,
                                                                  std::uint64_t seed);
[[nodiscard]] std::unique_ptr<SignalGenerator> make_gps_generator(const VehicleProfile& p,
                                                                  std::uint64_t seed);
[[nodiscard]] std::unique_ptr<SignalGenerator> make_wheel_generator(const VehicleProfile& p,
                                                                    std::uint64_t seed);
// Grayscale frames: a smooth multi-octave value-noise texture panned with the
// vehicle's motion plus small per-pixel sensor noise (0, +-1, +-2 levels). This is
// meant to compress roughly like a real raw image (neither random bytes nor
// a constant fill); it is still synthetic.
[[nodiscard]] std::unique_ptr<SignalGenerator> make_camera_generator(std::uint16_t width,
                                                                     std::uint16_t height,
                                                                     std::uint64_t seed);

struct FaultConfig {
  double gap_probability = 0.0;         // per sample: skip 1-3 samples (sequence gap)
  double regression_probability = 0.0;  // per sample: timestamp jumps back
  double stall_probability = 0.0;       // per sample: thread stalls before emitting
  std::chrono::milliseconds stall_duration{300};
  double regression_periods = 1.5;  // how far back a regressed timestamp jumps
};

enum class Pacing : std::uint8_t {
  kRealTime,  // sample k is emitted at start + k * period / rate_scale
  kUnpaced,   // as fast as possible (throughput and overload tests)
};

struct SensorConfig {
  StreamId stream = 0;
  double rate_hz = 1.0;
  double jitter_fraction = 0.02;  // timestamp jitter sigma as a fraction of the period
  FaultConfig faults;
  Pacing pacing = Pacing::kRealTime;
  double rate_scale = 1.0;
  std::uint64_t seed = 1;
};

struct SensorCounters {
  std::uint64_t emitted = 0;
  std::uint64_t gaps = 0;
  std::uint64_t regressions = 0;
  std::uint64_t stalls = 0;
};

// Runs one generator on its own thread and pushes into its ring.
//
// Timestamps are on a virtual sensor clock: sample k gets
// start + k * nominal_period + jitter, independent of rate_scale/pacing, so
// recorded data always looks like a vehicle running at nominal rates.
// enqueue_ns is the real steady-clock push time used for latency metrics.
class SensorProducer {
 public:
  SensorProducer(const SensorConfig& config, std::unique_ptr<SignalGenerator> generator,
                 RecordRing& ring);

  void run(const std::stop_token& stop, std::chrono::steady_clock::time_point start);
  [[nodiscard]] SensorCounters counters() const noexcept;
  [[nodiscard]] StreamId stream() const noexcept { return config_.stream; }

 private:
  SensorConfig config_;
  std::unique_ptr<SignalGenerator> generator_;
  RecordRing* ring_;
  std::mt19937_64 rng_;
  std::atomic<std::uint64_t> emitted_{0};
  std::atomic<std::uint64_t> gaps_{0};
  std::atomic<std::uint64_t> regressions_{0};
  std::atomic<std::uint64_t> stalls_{0};
};

}  // namespace mr
