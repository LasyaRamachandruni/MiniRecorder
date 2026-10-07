#pragma once

// Read path: memory-mapped file, footer index, O(log n) seek by time, and a
// k-way merge across streams in timestamp order.

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <mr/chunk.hpp>
#include <mr/file_util.hpp>
#include <mr/format.hpp>
#include <mr/result.hpp>

namespace mr {

struct ReadOptions {
  std::uint64_t stream_mask = ~std::uint64_t{0};
  std::int64_t start_ns = std::numeric_limits<std::int64_t>::min();  // inclusive
  std::int64_t end_ns = std::numeric_limits<std::int64_t>::max();    // inclusive
};

struct Message {
  RecordMeta meta;
  ByteSpan payload;  // valid until the next call to MessageIterator::next()
};

class Reader;

// Yields records from all selected streams merged by (timestamp, stream id).
// Within a stream, records keep their file order; if a stream contains
// timestamp regressions the merged output is ordered "per stream head", which
// is the best a streaming merge can do without buffering.
class MessageIterator {
 public:
  // true: `out` holds the next message; false: done; error: corrupt chunk.
  [[nodiscard]] Result<bool> next(Message& out);

 private:
  friend class Reader;

  struct Cursor {
    StreamId stream = 0;
    std::size_t next_chunk = 0;  // position in Reader::chunks_for_stream(stream)
    std::vector<std::byte> body;
    format::RecordParser parser{ByteSpan{}};
    format::RecordView current;
  };

  MessageIterator(const Reader& reader, ReadOptions options);
  [[nodiscard]] Result<bool> advance(Cursor& cursor);
  [[nodiscard]] Status prime();
  void push_heap_entry(std::size_t cursor_index);
  [[nodiscard]] bool later(std::size_t a, std::size_t b) const noexcept;

  const Reader* reader_;
  ReadOptions options_;
  ZstdDecompressor decompressor_;
  std::vector<Cursor> cursors_;
  std::vector<std::size_t> heap_;  // min-heap of cursor indices
  std::optional<std::size_t> pending_advance_;
  bool primed_ = false;
};

class Reader {
 public:
  // Opens a cleanly closed file. Files without a valid footer (crash
  // leftovers) are rejected with Errc::kNotFound; run recovery first.
  [[nodiscard]] static Result<Reader> open(const std::filesystem::path& path);

  [[nodiscard]] const format::FileHeader& header() const noexcept { return header_; }
  [[nodiscard]] const format::Footer& footer() const noexcept { return footer_; }
  [[nodiscard]] ByteSpan file_bytes() const noexcept { return file_.bytes(); }
  [[nodiscard]] const format::StreamInfo* stream_info(StreamId id) const noexcept;
  [[nodiscard]] const format::StreamStats* stream_stats(StreamId id) const noexcept;

  // Indices into footer().chunks of chunks containing `stream`, in file order.
  [[nodiscard]] std::span<const std::size_t> chunks_for_stream(StreamId stream) const noexcept;

  // Position (in chunks_for_stream(stream)) of the first chunk that can hold a
  // record of `stream` with timestamp >= t. Every chunk before it only holds
  // older records. Binary search over the running maximum of chunk end times,
  // so it stays correct even when chunk time ranges overlap. O(log n).
  [[nodiscard]] std::size_t seek_chunk(StreamId stream, std::int64_t t) const noexcept;

  // Reads, CRC-checks and decompresses chunk `chunk_index` (into footer().chunks).
  [[nodiscard]] Status read_chunk(std::size_t chunk_index, ZstdDecompressor& decompressor,
                                  std::vector<std::byte>& out) const;

  // The iterator borrows this Reader, which must outlive it.
  [[nodiscard]] MessageIterator messages(const ReadOptions& options = {}) const;

 private:
  Reader() = default;

  MappedFile file_;
  format::FileHeader header_;
  format::Footer footer_;
  std::array<std::vector<std::size_t>, kMaxStreams> stream_chunks_;
  std::array<std::vector<std::int64_t>, kMaxStreams> running_max_end_;
};

}  // namespace mr
