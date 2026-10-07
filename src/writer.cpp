#include <mr/writer.hpp>

#include <fcntl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>

#include <mr/chunk.hpp>
#include <mr/file_util.hpp>

namespace mr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::ptrdiff_t kMaxInflightLimit = 1024;

std::int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

std::uint64_t elapsed_ns(Clock::time_point since) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}

// Recycles byte buffers between the collector, the compressors and the writer
// so that steady-state operation does not allocate per chunk.
class BufferPool {
 public:
  explicit BufferPool(std::size_t max_cached) : max_cached_(max_cached) {}

  std::vector<std::byte> acquire() {
    const std::scoped_lock lock(mu_);
    if (free_.empty()) {
      return {};
    }
    std::vector<std::byte> buf = std::move(free_.back());
    free_.pop_back();
    buf.clear();
    return buf;
  }

  void release(std::vector<std::byte>&& buf) {
    const std::scoped_lock lock(mu_);
    if (free_.size() < max_cached_) {
      free_.push_back(std::move(buf));
    }
  }

 private:
  std::mutex mu_;
  std::vector<std::vector<std::byte>> free_;
  std::size_t max_cached_;
};

struct ChunkJob {
  format::ChunkHeader meta;
  std::vector<std::byte> records;
  std::vector<std::int64_t> enqueue_ns;
};

struct EncodedChunk {
  format::ChunkHeader header;
  std::vector<std::byte> bytes;
  std::vector<std::int64_t> enqueue_ns;
  std::optional<Error> error;
};

struct Builder {
  std::vector<std::byte> records;
  std::vector<std::int64_t> enqueue_ns;
  format::ChunkHeader meta;
  Clock::time_point opened_at;
  bool open = false;
};

}  // namespace

Result<FsyncPolicy> parse_fsync_policy(std::string_view text) {
  if (text == "chunk") {
    return FsyncPolicy{FsyncMode::kEveryChunk, std::chrono::milliseconds(0)};
  }
  if (text == "close") {
    return FsyncPolicy{FsyncMode::kOnClose, std::chrono::milliseconds(0)};
  }
  constexpr std::string_view kPrefix = "interval:";
  if (text.starts_with(kPrefix)) {
    const std::string_view num = text.substr(kPrefix.size());
    std::int64_t ms = 0;
    const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), ms);
    if (ec == std::errc() && ptr == num.data() + num.size() && ms > 0) {
      return FsyncPolicy{FsyncMode::kInterval, std::chrono::milliseconds(ms)};
    }
  }
  return make_error(Errc::kInvalidArgument,
                    "fsync policy must be 'chunk', 'close' or 'interval:<ms>', got '" +
                        std::string(text) + "'");
}

std::string describe(const FsyncPolicy& policy) {
  switch (policy.mode) {
    case FsyncMode::kEveryChunk:
      return "chunk";
    case FsyncMode::kOnClose:
      return "close";
    case FsyncMode::kInterval:
      return "interval:" + std::to_string(policy.interval.count());
  }
  return "unknown";
}

struct StorageWriter::Impl {
  explicit Impl(WriterOptions o)
      : opts(std::move(o)),
        partial(partial_path(opts.path)),
        inflight(static_cast<std::ptrdiff_t>(opts.max_inflight_chunks)),
        pool(opts.max_inflight_chunks * 2 + kMaxStreams) {}

  // ---- configuration and file --------------------------------------------
  WriterOptions opts;
  std::filesystem::path partial;
  FileDescriptor fd;
  std::uint64_t offset = 0;  // owned by the writer thread after open()

  // ---- collector-side state (single caller thread) -------------------------
  std::array<Builder, kMaxStreams> builders;
  std::array<format::StreamStats, kMaxStreams> stats{};
  std::array<StreamQualityCounters, kMaxStreams> quality{};
  std::uint64_t known_streams = 0;  // bitmask of streams declared in the header
  std::uint64_t next_chunk_seq = 0;
  std::uint64_t append_blocked_ns = 0;
  bool closed = false;
  bool stopped = false;

  std::counting_semaphore<kMaxInflightLimit> inflight;
  BufferPool pool;

  // ---- compression stage ---------------------------------------------------
  std::mutex job_mu;
  std::condition_variable job_cv;
  std::deque<ChunkJob> jobs;
  bool jobs_closed = false;
  std::atomic<std::uint64_t> compress_ns{0};

  // ---- ordered write stage -------------------------------------------------
  std::mutex ready_mu;
  std::condition_variable ready_cv;
  std::map<std::uint64_t, EncodedChunk> ready;
  bool ready_closed = false;

  // Owned by the writer thread; read by close() only after it was joined.
  std::vector<format::ChunkIndexEntry> index;
  std::array<std::uint64_t, kMaxStreams> stored_bytes{};
  WriterMetrics metrics;

  // Set by the writer thread on failure so append() can fail fast.
  std::atomic<bool> failed{false};
  std::mutex error_mu;
  std::optional<Error> first_error;

  std::vector<std::jthread> workers;
  std::jthread writer_thread;

  void record_error(const Error& e) {
    const std::scoped_lock lock(error_mu);
    if (!first_error) {
      first_error = e;
    }
    failed.store(true, std::memory_order_release);
  }
  Error current_error() {
    const std::scoped_lock lock(error_mu);
    return first_error.value_or(make_error(Errc::kIo, "writer failed"));
  }

  // ---- compression worker ----------------------------------------------------
  void worker_main() {
    ZstdCompressor compressor(opts.zstd_level);
    std::vector<std::byte> scratch;
    for (;;) {
      ChunkJob job;
      {
        std::unique_lock lock(job_mu);
        job_cv.wait(lock, [this] { return !jobs.empty() || jobs_closed; });
        if (jobs.empty()) {
          return;
        }
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      EncodedChunk enc;
      enc.bytes = pool.acquire();
      const auto t0 = Clock::now();
      auto header = encode_chunk(job.meta, job.records, compressor, scratch, enc.bytes);
      compress_ns.fetch_add(elapsed_ns(t0), std::memory_order_relaxed);
      if (header) {
        enc.header = header.value();
      } else {
        enc.error = std::move(header).error();
      }
      enc.enqueue_ns = std::move(job.enqueue_ns);
      pool.release(std::move(job.records));
      {
        const std::scoped_lock lock(ready_mu);
        ready.emplace(job.meta.chunk_seq, std::move(enc));
      }
      ready_cv.notify_one();
    }
  }

  // ---- writer thread ---------------------------------------------------------
  struct WriterState {
    std::uint64_t next_seq = 0;
    std::uint64_t written = 0;
    bool dirty = false;
    Clock::time_point last_sync = Clock::now();
    std::vector<std::vector<std::int64_t>> awaiting_durable;
  };

  void sync_now(WriterState& w) {
    const auto t0 = Clock::now();
    if (auto s = sync_data(fd.get()); !s) {
      record_error(s.error());
    }
    metrics.fsync_ns += elapsed_ns(t0);
    ++metrics.fsync_count;
    const std::int64_t now = steady_ns();
    for (const auto& batch : w.awaiting_durable) {
      for (const std::int64_t e : batch) {
        metrics.durable_latency_ns.push_back(now - e);
      }
    }
    w.awaiting_durable.clear();
    w.dirty = false;
    w.last_sync = Clock::now();
    if (opts.on_durable && !failed.load(std::memory_order_acquire)) {
      opts.on_durable(w.written);
    }
  }

  void write_chunk(WriterState& w, EncodedChunk& chunk) {
    if (chunk.error) {
      record_error(*chunk.error);
    }
    if (!failed.load(std::memory_order_acquire)) {
      const auto t0 = Clock::now();
      if (auto s = write_all(fd.get(), chunk.bytes); !s) {
        record_error(s.error());
      } else {
        metrics.write_ns += elapsed_ns(t0);
        const format::ChunkHeader& h = chunk.header;
        index.push_back({.offset = offset,
                         .stream_mask = h.stream_mask,
                         .min_time_ns = h.min_time_ns,
                         .max_time_ns = h.max_time_ns,
                         .record_count = h.record_count,
                         .total_size = static_cast<std::uint32_t>(h.total_size()),
                         .uncompressed_size = h.uncompressed_size});
        offset += chunk.bytes.size();
        if (std::popcount(h.stream_mask) == 1) {
          stored_bytes[static_cast<std::size_t>(std::countr_zero(h.stream_mask))] += chunk.bytes.size();
        }
        ++metrics.chunks_written;
        metrics.records_written += h.record_count;
        metrics.record_bytes += h.uncompressed_size;
        ++w.written;
        w.dirty = true;
        if (opts.track_latency) {
          const std::int64_t now = steady_ns();
          for (const std::int64_t e : chunk.enqueue_ns) {
            metrics.write_latency_ns.push_back(now - e);
          }
          w.awaiting_durable.push_back(std::move(chunk.enqueue_ns));
        }
        if (opts.fsync.mode == FsyncMode::kEveryChunk) {
          sync_now(w);
        }
      }
    }
    pool.release(std::move(chunk.bytes));
    inflight.release();
    ++w.next_seq;
  }

  void writer_main() {
    WriterState w;
    for (;;) {
      std::optional<EncodedChunk> chunk;
      bool finished = false;
      {
        std::unique_lock lock(ready_mu);
        auto ready_or_closed = [&] {
          return (!ready.empty() && ready.begin()->first == w.next_seq) || ready_closed;
        };
        if (w.dirty && opts.fsync.mode == FsyncMode::kInterval) {
          ready_cv.wait_until(lock, w.last_sync + opts.fsync.interval, ready_or_closed);
        } else {
          ready_cv.wait(lock, ready_or_closed);
        }
        if (!ready.empty() && ready.begin()->first == w.next_seq) {
          chunk = std::move(ready.begin()->second);
          ready.erase(ready.begin());
        } else if (ready_closed) {
          if (!ready.empty()) {
            record_error(make_error(Errc::kIo, "internal: missing chunk sequence number"));
          }
          finished = true;
        }
      }
      if (chunk) {
        write_chunk(w, *chunk);
      }
      if (w.dirty && opts.fsync.mode == FsyncMode::kInterval &&
          Clock::now() - w.last_sync >= opts.fsync.interval) {
        sync_now(w);
      }
      if (finished) {
        break;
      }
    }
    // Everything written is made durable before the footer goes out, so that
    // durable latency is also measured for the on-close policy.
    if (w.dirty) {
      sync_now(w);
    }
  }

  // ---- collector-side helpers ----------------------------------------------
  Status seal(StreamId id) {
    Builder& b = builders[id];
    if (!b.open) {
      return ok_status();
    }
    b.open = false;
    if (b.meta.record_count == 0) {
      return ok_status();
    }
    const auto t0 = Clock::now();
    inflight.acquire();
    append_blocked_ns += elapsed_ns(t0);

    ChunkJob job;
    job.meta = b.meta;
    job.meta.chunk_seq = next_chunk_seq++;
    job.records = std::move(b.records);
    job.enqueue_ns = std::move(b.enqueue_ns);
    b.records = {};
    b.enqueue_ns = {};
    {
      const std::scoped_lock lock(job_mu);
      jobs.push_back(std::move(job));
    }
    job_cv.notify_one();
    return ok_status();
  }

  void stop_pipeline() {
    if (stopped) {
      return;
    }
    stopped = true;
    {
      const std::scoped_lock lock(job_mu);
      jobs_closed = true;
    }
    job_cv.notify_all();
    workers.clear();  // joins
    {
      const std::scoped_lock lock(ready_mu);
      ready_closed = true;
    }
    ready_cv.notify_all();
    if (writer_thread.joinable()) {
      writer_thread.join();
    }
  }
};

StorageWriter::StorageWriter(PrivateTag /*tag*/, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

StorageWriter::~StorageWriter() {
  if (impl_ && !impl_->closed) {
    impl_->stop_pipeline();
  }
}

const std::filesystem::path& StorageWriter::partial_file() const noexcept { return impl_->partial; }

Result<std::unique_ptr<StorageWriter>> StorageWriter::open(WriterOptions options) {
  if (options.max_inflight_chunks == 0 ||
      options.max_inflight_chunks > static_cast<std::size_t>(kMaxInflightLimit)) {
    return make_error(Errc::kInvalidArgument, "max_inflight_chunks must be in [1, 1024]");
  }
  if (options.compression_threads == 0) {
    return make_error(Errc::kInvalidArgument, "compression_threads must be >= 1");
  }
  if (options.chunk_target_bytes == 0 || options.chunk_target_bytes > format::kMaxChunkBytes / 2) {
    return make_error(Errc::kInvalidArgument, "chunk_target_bytes out of range");
  }
  auto impl = std::make_unique<Impl>(std::move(options));

  format::FileHeader header;
  header.created_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  for (const format::StreamInfo& s : impl->opts.streams) {
    if (s.id >= kMaxStreams || (impl->known_streams & (std::uint64_t{1} << s.id)) != 0) {
      return make_error(Errc::kInvalidArgument, "invalid or duplicate stream id " + std::to_string(s.id));
    }
    if (s.name.size() > format::kStreamNameMax) {
      return make_error(Errc::kInvalidArgument, "stream name too long: " + s.name);
    }
    impl->known_streams |= std::uint64_t{1} << s.id;
    header.streams.push_back(s);
  }

  auto fd = open_file(impl->partial, O_WRONLY | O_CREAT | O_TRUNC);
  if (!fd) {
    return std::move(fd).error();
  }
  impl->fd = std::move(fd).value();
  const std::vector<std::byte> header_bytes = format::encode_file_header(header);
  MR_RETURN_IF_ERROR(write_all(impl->fd.get(), header_bytes));
  // Make the header and the directory entry durable up front: recovery needs
  // the header to interpret anything that follows.
  MR_RETURN_IF_ERROR(sync_data(impl->fd.get()));
  MR_RETURN_IF_ERROR(sync_directory(impl->partial.parent_path()));
  impl->offset = header_bytes.size();

  Impl* raw = impl.get();
  for (unsigned i = 0; i < raw->opts.compression_threads; ++i) {
    raw->workers.emplace_back([raw] { raw->worker_main(); });
  }
  raw->writer_thread = std::jthread([raw] { raw->writer_main(); });
  return std::make_unique<StorageWriter>(PrivateTag{}, std::move(impl));
}

Status StorageWriter::append(const RecordMeta& meta, ByteSpan payload, std::int64_t enqueue_ns) {
  Impl& s = *impl_;
  if (s.closed || s.stopped) {
    return make_error(Errc::kClosed, "writer is closed");
  }
  if (s.failed.load(std::memory_order_acquire)) {
    return s.current_error();
  }
  if (meta.stream_id >= kMaxStreams || (s.known_streams & (std::uint64_t{1} << meta.stream_id)) == 0) {
    return make_error(Errc::kInvalidArgument, "record for undeclared stream " + std::to_string(meta.stream_id));
  }
  if (payload.size() > format::kMaxRecordPayload) {
    return make_error(Errc::kInvalidArgument, "record payload exceeds limit");
  }

  Builder& b = s.builders[meta.stream_id];
  const std::size_t encoded = format::encoded_record_size(payload.size());
  if (b.open && b.records.size() + encoded > format::kMaxChunkBytes) {
    MR_RETURN_IF_ERROR(s.seal(meta.stream_id));
  }
  if (!b.open) {
    b.records = s.pool.acquire();
    b.records.reserve(std::max(s.opts.chunk_target_bytes + encoded, b.records.capacity()));
    b.enqueue_ns.clear();
    b.meta = format::ChunkHeader{};
    b.opened_at = Clock::now();
    b.open = true;
  }
  format::append_record(b.records, meta, payload);
  b.meta.record_count++;
  b.meta.stream_mask |= std::uint64_t{1} << meta.stream_id;
  b.meta.min_time_ns = std::min(b.meta.min_time_ns, meta.timestamp_ns);
  b.meta.max_time_ns = std::max(b.meta.max_time_ns, meta.timestamp_ns);
  if (s.opts.track_latency) {
    b.enqueue_ns.push_back(enqueue_ns);
  }

  format::StreamStats& st = s.stats[meta.stream_id];
  if (st.records == 0) {
    st.first_time_ns = meta.timestamp_ns;
    st.last_time_ns = meta.timestamp_ns;
  }
  st.first_time_ns = std::min(st.first_time_ns, meta.timestamp_ns);
  st.last_time_ns = std::max(st.last_time_ns, meta.timestamp_ns);
  ++st.records;
  st.payload_bytes += payload.size();

  if (b.records.size() >= s.opts.chunk_target_bytes) {
    return s.seal(meta.stream_id);
  }
  return ok_status();
}

Status StorageWriter::seal_aged(Clock::time_point now) {
  Impl& s = *impl_;
  if (s.closed || s.stopped) {
    return make_error(Errc::kClosed, "writer is closed");
  }
  for (std::size_t id = 0; id < kMaxStreams; ++id) {
    const Builder& b = s.builders[id];
    if (b.open && now - b.opened_at >= s.opts.chunk_max_age) {
      MR_RETURN_IF_ERROR(s.seal(static_cast<StreamId>(id)));
    }
  }
  return ok_status();
}

void StorageWriter::set_quality_counters(StreamId stream, const StreamQualityCounters& counters) {
  if (stream < kMaxStreams) {
    impl_->quality[stream] = counters;
  }
}

Result<WriteSummary> StorageWriter::close() {
  Impl& s = *impl_;
  if (s.closed || s.stopped) {
    return make_error(Errc::kClosed, "writer already closed");
  }
  for (std::size_t id = 0; id < kMaxStreams; ++id) {
    if (auto st = s.seal(static_cast<StreamId>(id)); !st) {
      s.record_error(st.error());
    }
  }
  s.stop_pipeline();
  if (s.failed.load(std::memory_order_acquire)) {
    return s.current_error();
  }

  format::Footer footer;
  footer.chunks = std::move(s.index);
  bool any = false;
  for (const format::StreamInfo& info : s.opts.streams) {
    format::StreamStats st = s.stats[info.id];
    st.id = info.id;
    st.stored_bytes = s.stored_bytes[info.id];
    const StreamQualityCounters& q = s.quality[info.id];
    st.dropped = q.dropped;
    st.seq_gaps = q.seq_gaps;
    st.ts_regressions = q.ts_regressions;
    st.rate_events = q.rate_events;
    st.stale_events = q.stale_events;
    if (st.records > 0) {
      footer.start_time_ns = any ? std::min(footer.start_time_ns, st.first_time_ns) : st.first_time_ns;
      footer.end_time_ns = any ? std::max(footer.end_time_ns, st.last_time_ns) : st.last_time_ns;
      any = true;
    }
    footer.streams.push_back(st);
  }

  const std::vector<std::byte> footer_bytes = format::encode_footer(footer, s.offset);
  MR_RETURN_IF_ERROR(write_all(s.fd.get(), footer_bytes));
  s.offset += footer_bytes.size();
  MR_RETURN_IF_ERROR(sync_data(s.fd.get()));
  ++s.metrics.fsync_count;
  MR_RETURN_IF_ERROR(s.fd.close());
  MR_RETURN_IF_ERROR(durable_rename(s.partial, s.opts.path));
  s.closed = true;

  WriteSummary summary;
  summary.path = s.opts.path;
  summary.footer = std::move(footer);
  summary.metrics = std::move(s.metrics);
  summary.metrics.file_bytes = s.offset;
  summary.metrics.compress_ns = s.compress_ns.load(std::memory_order_relaxed);
  summary.metrics.append_blocked_ns = s.append_blocked_ns;
  return summary;
}

}  // namespace mr
