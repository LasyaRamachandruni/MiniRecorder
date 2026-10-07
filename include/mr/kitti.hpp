#pragma once

// Optional replay source for real IMU/GPS data from the KITTI raw dataset
// (OXTS RT3003 GPS/IMU). Expected layout, as in the official archives:
//
//   <dir>/timestamps.txt        one "YYYY-MM-DD HH:MM:SS.fffffffff" per line
//   <dir>/data/0000000000.txt   30 space-separated values per file
//
// Download a "[synced+rectified data]" or "[unsynced+unrectified data]" drive
// from https://www.cvlibs.net/datasets/kitti/raw_data.php (registration
// required) and point --kitti at its "oxts" directory. Nothing in the build or
// test suite depends on the dataset being present.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string_view>
#include <vector>

#include <mr/record_ring.hpp>
#include <mr/result.hpp>
#include <mr/sensor_types.hpp>

namespace mr {

struct OxtsSample {
  std::int64_t timestamp_ns = 0;  // relative to the first sample
  GpsFix gps;
  ImuSample imu;
};

// Parses one OXTS data line (30 fields). Only the fields needed for the IMU
// and GPS payloads are kept.
[[nodiscard]] Result<OxtsSample> parse_oxts_line(std::string_view line);
// Parses "YYYY-MM-DD HH:MM:SS.fffffffff" to nanoseconds since midnight.
[[nodiscard]] Result<std::int64_t> parse_kitti_timestamp(std::string_view text);
// Loads a whole OXTS directory.
[[nodiscard]] Result<std::vector<OxtsSample>> load_oxts_dir(const std::filesystem::path& dir);

struct KittiReplayConfig {
  StreamId imu_stream = 0;
  StreamId gps_stream = 1;
  double rate_scale = 1.0;
  bool unpaced = false;
  bool loop = true;  // restart from the beginning, shifting timestamps forward
};

// One thread feeding two rings (IMU and GPS). Each ring still has exactly
// one producer, so the SPSC contract holds.
class KittiOxtsReplay {
 public:
  KittiOxtsReplay(KittiReplayConfig config, std::vector<OxtsSample> samples, RecordRing& imu_ring,
                  RecordRing& gps_ring);

  void run(const std::stop_token& stop, std::chrono::steady_clock::time_point start);
  [[nodiscard]] std::uint64_t emitted() const noexcept {
    return emitted_.load(std::memory_order_relaxed);
  }

 private:
  KittiReplayConfig config_;
  std::vector<OxtsSample> samples_;
  RecordRing* imu_ring_;
  RecordRing* gps_ring_;
  std::atomic<std::uint64_t> emitted_{0};
};

}  // namespace mr
