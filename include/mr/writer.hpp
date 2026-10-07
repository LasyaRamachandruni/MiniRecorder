#pragma once

// StorageWriter: the write path of the storage engine.
//
//   append() ──> per-stream ChunkBuilder ──seal──> compression pool (N threads)
//                (collector thread)                      │ (any order)
//                                                        v
//                                     reorder buffer keyed by chunk_seq
//                                                        │ (strict order)
//                                                        v
//                                       single writer thread: write(2),
//                                       fdatasync per policy, chunk index
//
// Chunks are compressed in parallel but written strictly in chunk_seq order
// by one thread, so the file is always a valid prefix: if the process dies,
// every chunk before the first torn one is intact (see recovery.hpp).
// A counting semaphore bounds the number of chunks between seal and write;
// when the disk or the compressors fall behind, append() blocks and the
// back-pressure propagates to the per-stream rings and their overflow policy.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <mr/bytes.hpp>
#include <mr/format.hpp>
#include <mr/record.hpp>
#include <mr/result.hpp>

namespace mr {

enum class FsyncMode : std::uint8_t {
  kEveryChunk,  // fdatasync after every chunk write
  kInterval,    // fdatasync at most every `interval` while data is dirty
  kOnClose,     // only at close (fastest, loses up to the whole file on power loss)
};

struct FsyncPolicy {
  FsyncMode mode = FsyncMode::kInterval;
  std::chrono::milliseconds interval{100};
};

// Accepts "chunk", "close" or "interval:<ms>".
[[nodiscard]] Result<FsyncPolicy> parse_fsync_policy(std::string_view text);
[[nodiscard]] std::string describe(const FsyncPolicy& policy);

struct WriterOptions {
  std::filesystem::path path;  // final name; data goes to path + ".partial" until close
  std::vector<format::StreamInfo> streams;
  int zstd_level = 3;  // <= 0 stores chunks uncompressed
  std::size_t chunk_target_bytes = 1U << 20;
  std::chrono::milliseconds chunk_max_age{500};
  FsyncPolicy fsync;
  unsigned compression_threads = 2;
  std::size_t max_inflight_chunks = 8;
  bool track_latency = false;
  // Invoked on the writer thread after data becomes durable, with the number
  // of chunks durable so far. Must be cheap and must not call back into the
  // writer.
  std::function<void(std::uint64_t)> on_durable;
};

// Counters the recorder supplies at close; stored in the footer.
struct StreamQualityCounters {
  std::uint64_t dropped = 0;
  std::uint64_t seq_gaps = 0;
  std::uint64_t ts_regressions = 0;
  std::uint64_t rate_events = 0;
  std::uint64_t stale_events = 0;
};

struct WriterMetrics {
  std::uint64_t chunks_written = 0;
  std::uint64_t records_written = 0;
  std::uint64_t record_bytes = 0;  // uncompressed chunk bodies
  std::uint64_t file_bytes = 0;    // bytes written to the file (all structures)
  std::uint64_t fsync_count = 0;
  std::uint64_t compress_ns = 0;  // summed across compression threads
  std::uint64_t write_ns = 0;
  std::uint64_t fsync_ns = 0;
  std::uint64_t append_blocked_ns = 0;  // time append() waited for an in-flight slot
  // Per record, from producer enqueue: until write(2) returned, and until the
  // covering fdatasync returned. Only filled when track_latency is set.
  std::vector<std::int64_t> write_latency_ns;
  std::vector<std::int64_t> durable_latency_ns;
};

struct WriteSummary {
  std::filesystem::path path;
  format::Footer footer;
  WriterMetrics metrics;
};

// Not thread-safe: append/seal_aged/set_quality_counters/close must be
// externally serialized (the recorder calls them from the collector thread,
// and close() only after the collector has been joined).
class StorageWriter {
 public:
  [[nodiscard]] static Result<std::unique_ptr<StorageWriter>> open(WriterOptions options);

  StorageWriter(const StorageWriter&) = delete;
  StorageWriter& operator=(const StorageWriter&) = delete;
  StorageWriter(StorageWriter&&) = delete;
  StorageWriter& operator=(StorageWriter&&) = delete;
  // Without close() the threads are stopped and the file is left as
  // ".partial" without a footer, i.e. exactly what a crash leaves behind.
  ~StorageWriter();

  // Appends one record. Called from a single thread (the collector).
  [[nodiscard]] Status append(const RecordMeta& meta, ByteSpan payload, std::int64_t enqueue_ns);

  // Seals builders whose first record is older than chunk_max_age.
  [[nodiscard]] Status seal_aged(std::chrono::steady_clock::time_point now);

  void set_quality_counters(StreamId stream, const StreamQualityCounters& counters);

  // Seals everything, waits for the pipeline, writes footer and trailer,
  // syncs, and atomically renames the .partial file to its final name.
  [[nodiscard]] Result<WriteSummary> close();

  [[nodiscard]] const std::filesystem::path& partial_file() const noexcept;

 private:
  struct Impl;
  // Passkey: only open() can construct, but std::make_unique still works.
  struct PrivateTag {
    explicit PrivateTag() = default;
  };

 public:
  StorageWriter(PrivateTag tag, std::unique_ptr<Impl> impl);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace mr
