#pragma once

// Shared helpers for tests: temporary directories and small record files.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <mr/writer.hpp>

namespace mr::test {

class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = "mr_test_";
    if (info != nullptr) {
      name += info->test_suite_name();
      name += "_";
      name += info->name();
    }
    name += "_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    for (char& c : name) {
      if (c == '/') {
        c = '_';
      }
    }
    path_ = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

// Deterministic payload so tests can verify bytes after a round trip.
inline std::vector<std::byte> test_payload(StreamId stream, std::uint64_t seq, std::size_t size) {
  std::vector<std::byte> out(size);
  for (std::size_t i = 0; i < size; ++i) {
    out[i] = static_cast<std::byte>((stream * 31 + seq * 7 + i / 8) & 0xFFU);
  }
  return out;
}

inline std::vector<format::StreamInfo> test_streams(int count) {
  std::vector<format::StreamInfo> out;
  for (int i = 0; i < count; ++i) {
    out.push_back({.id = static_cast<StreamId>(i),
                   .kind = SensorKind::kImu,
                   .nominal_rate_hz = 100.0 * (i + 1),
                   .name = "s" + std::to_string(i)});
  }
  return out;
}

struct WrittenRecord {
  RecordMeta meta;
  std::size_t payload_size;
};

// Writes `per_stream` records for each stream with interleaved timestamps.
// Stream s has period (s + 1) ms; payload sizes vary.
inline std::vector<WrittenRecord> write_test_file(const std::filesystem::path& path, int streams,
                                                  int per_stream, std::size_t chunk_bytes = 4096,
                                                  int level = 3) {
  WriterOptions opts;
  opts.path = path;
  opts.streams = test_streams(streams);
  opts.zstd_level = level;
  opts.chunk_target_bytes = chunk_bytes;
  opts.fsync = {FsyncMode::kOnClose, std::chrono::milliseconds(0)};
  auto writer = StorageWriter::open(opts);
  EXPECT_TRUE(writer) << writer.error().describe();
  std::vector<WrittenRecord> written;
  for (int i = 0; i < per_stream; ++i) {
    for (int s = 0; s < streams; ++s) {
      const RecordMeta meta{
          .timestamp_ns = 1'000'000'000LL + static_cast<std::int64_t>(i) * (s + 1) * 1'000'000,
          .seq = static_cast<std::uint64_t>(i),
          .stream_id = static_cast<StreamId>(s),
          .flags = 0};
      const std::size_t size = 16 + static_cast<std::size_t>((i * 13 + s * 5) % 200);
      const auto payload = test_payload(meta.stream_id, meta.seq, size);
      EXPECT_TRUE((*writer)->append(meta, payload, 0));
      written.push_back({meta, size});
    }
  }
  auto summary = (*writer)->close();
  EXPECT_TRUE(summary) << summary.error().describe();
  return written;
}

}  // namespace mr::test
