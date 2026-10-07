#pragma once

// Recorder: wires producers, rings, the collector thread and the storage
// writer together.
//
//   sensor thread ──SPSC ring──┐
//   sensor thread ──SPSC ring──┤   collector thread:
//   sensor thread ──SPSC ring──┼──> copy out -> quality checks -> privacy
//   sensor thread ──SPSC ring──┘    -> trigger / event window -> StorageWriter
//
// Threading: one thread per producer, one collector, plus the writer's
// compression pool and ordered writer thread (see writer.hpp).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <mr/privacy.hpp>
#include <mr/quality.hpp>
#include <mr/record_ring.hpp>
#include <mr/sensors.hpp>
#include <mr/trigger.hpp>
#include <mr/writer.hpp>

namespace mr {

enum class RecordMode : std::uint8_t { kContinuous, kTriggered };

struct StreamSetup {
  std::string name;
  SensorKind kind = SensorKind::kImu;
  double rate_hz = 100.0;
  OverflowPolicy policy = OverflowPolicy::kDropOldest;
  std::size_t ring_capacity = 1024;
  FaultConfig faults;
  std::uint16_t camera_width = 640;   // camera only
  std::uint16_t camera_height = 480;  // camera only
};

// The default AV-style sensor set: IMU 200 Hz, GPS 10 Hz, wheel/CAN 100 Hz,
// camera 640x480 at 30 Hz.
[[nodiscard]] std::vector<StreamSetup> default_streams();

struct RecorderConfig {
  std::filesystem::path output;
  std::vector<StreamSetup> streams = default_streams();
  WriterOptions writer;  // path and streams are filled in by the recorder
  RecordMode mode = RecordMode::kContinuous;
  TriggerConfig trigger;
  QualityConfig quality;
  PrivacyConfig privacy;
  Pacing pacing = Pacing::kRealTime;
  double rate_scale = 1.0;
  VehicleProfile vehicle;
  std::uint64_t seed = 42;
  // Optional KITTI raw OXTS directory (…/oxts with timestamps.txt and data/).
  // When set, IMU and GPS come from the dataset instead of the simulator.
  std::optional<std::filesystem::path> kitti_oxts_dir;
  bool kitti_loop = true;
};

struct StreamReport {
  std::string name;
  StreamId id = 0;
  SensorKind kind = SensorKind::kUnknown;
  OverflowPolicy policy = OverflowPolicy::kDropOldest;
  RingCounters ring;
  SensorCounters sensor;
};

struct RecorderReport {
  WriteSummary write;
  std::vector<StreamReport> streams;
  double elapsed_s = 0.0;
  std::uint64_t collected_records = 0;
  std::uint64_t quality_events = 0;
  std::uint64_t triggers = 0;
  std::uint64_t pre_trigger_overflow = 0;
};

class Recorder {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Recorder>> create(RecorderConfig config);

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
  Recorder(Recorder&&) = delete;
  Recorder& operator=(Recorder&&) = delete;
  // Destroying a running recorder stops all threads without closing the file
  // (it stays ".partial", as after a crash).
  ~Recorder();

  [[nodiscard]] Status start();
  // Stops producers, drains every ring, closes the log. Blocks until done.
  [[nodiscard]] Result<RecorderReport> stop();

  // Live counters for progress output (approximate while running).
  [[nodiscard]] std::uint64_t collected_records() const noexcept;

 private:
  struct Impl;
  struct PrivateTag {
    explicit PrivateTag() = default;
  };

 public:
  Recorder(PrivateTag tag, std::unique_ptr<Impl> impl);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace mr
