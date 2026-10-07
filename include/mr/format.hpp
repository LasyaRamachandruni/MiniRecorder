#pragma once

// On-disk format of a MiniRecorder log (.mrlog). See docs/FORMAT.md for the
// byte-level description; this header is the single source of truth for the
// constants and the encode/decode functions.
//
//   +-------------+---------+---------+-----+---------+--------+---------+
//   | FileHeader  | Chunk 0 | Chunk 1 | ... | Chunk N | Footer | Trailer |
//   +-------------+---------+---------+-----+---------+--------+---------+
//
// Every structure carries its own CRC32C. Chunks are self-describing, so a
// file without footer/trailer (a crash leftover) can be recovered by scanning
// chunks from the front.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <mr/bytes.hpp>
#include <mr/record.hpp>
#include <mr/result.hpp>

namespace mr::format {

inline constexpr std::array<std::byte, 8> kFileMagic{
    std::byte{'M'}, std::byte{'R'},  std::byte{'L'},  std::byte{'O'},
    std::byte{'G'}, std::byte{'\r'}, std::byte{'\n'}, std::byte{0x1A}};
inline constexpr std::array<std::byte, 8> kTrailerMagic{
    std::byte{'M'}, std::byte{'R'}, std::byte{'L'}, std::byte{'O'},
    std::byte{'G'}, std::byte{'E'}, std::byte{'N'}, std::byte{'D'}};

inline constexpr std::uint16_t kVersion = 1;

inline constexpr std::size_t kFileHeaderFixedSize = 32;
inline constexpr std::size_t kStreamEntrySize = 48;
inline constexpr std::size_t kStreamNameMax = 40;

inline constexpr std::uint32_t kChunkMagic = 0x4B43524DU;  // "MRCK" little-endian
inline constexpr std::size_t kChunkHeaderSize = 64;
inline constexpr std::uint32_t kFooterMagic = 0x5446524DU;  // "MRFT" little-endian
inline constexpr std::size_t kFooterFixedSize = 32;
inline constexpr std::size_t kChunkIndexEntrySize = 48;
inline constexpr std::size_t kStreamStatsEntrySize = 88;
inline constexpr std::size_t kTrailerSize = 24;
inline constexpr std::size_t kRecordHeaderSize = 24;

// Sanity limits applied by every parser so corrupt size fields cannot trigger
// huge allocations.
inline constexpr std::uint32_t kMaxChunkBytes = 64U << 20;
inline constexpr std::uint32_t kMaxRecordPayload = 16U << 20;

namespace footer_flags {
inline constexpr std::uint16_t kRecovered = 1U << 0;
}  // namespace footer_flags

enum class Compression : std::uint32_t { kNone = 0, kZstd = 1 };

// ---------------------------------------------------------------- file header

struct StreamInfo {
  StreamId id = 0;
  SensorKind kind = SensorKind::kUnknown;
  double nominal_rate_hz = 0.0;  // stored as integer millihertz
  std::string name;              // at most kStreamNameMax bytes
};

struct FileHeader {
  std::int64_t created_unix_ns = 0;
  std::uint32_t flags = 0;
  std::vector<StreamInfo> streams;
  std::size_t encoded_size = 0;  // filled by decode/encode
};

[[nodiscard]] std::vector<std::byte> encode_file_header(const FileHeader& header);
[[nodiscard]] Result<FileHeader> decode_file_header(ByteSpan data);

// ---------------------------------------------------------------- chunk header

struct ChunkHeader {
  Compression compression = Compression::kNone;
  std::uint32_t record_count = 0;
  std::uint64_t stream_mask = 0;
  std::int64_t min_time_ns = std::numeric_limits<std::int64_t>::max();
  std::int64_t max_time_ns = std::numeric_limits<std::int64_t>::min();
  std::uint32_t uncompressed_size = 0;
  std::uint32_t compressed_size = 0;  // bytes following the header
  std::uint64_t chunk_seq = 0;        // 0,1,2,... in file order
  std::uint32_t payload_crc = 0;      // CRC32C of the compressed bytes

  [[nodiscard]] std::uint64_t total_size() const noexcept {
    return kChunkHeaderSize + compressed_size;
  }
};

void encode_chunk_header(const ChunkHeader& header, std::span<std::byte, kChunkHeaderSize> out);
// Validates magic, version, header CRC and size limits.
[[nodiscard]] Result<ChunkHeader> decode_chunk_header(ByteSpan data);

// ---------------------------------------------------------------- records

struct RecordView {
  RecordMeta meta;
  ByteSpan payload;
};

void append_record(std::vector<std::byte>& out, const RecordMeta& meta, ByteSpan payload);

[[nodiscard]] constexpr std::size_t encoded_record_size(std::size_t payload_size) noexcept {
  return kRecordHeaderSize + payload_size;
}

// Iterates records in an uncompressed chunk body with full bounds checking.
class RecordParser {
 public:
  explicit RecordParser(ByteSpan body) noexcept : body_(body) {}
  // true: `out` holds the next record; false: clean end of body; error: malformed.
  [[nodiscard]] Result<bool> next(RecordView& out);
  [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

 private:
  ByteSpan body_;
  std::size_t pos_ = 0;
};

// ---------------------------------------------------------------- footer

struct ChunkIndexEntry {
  std::uint64_t offset = 0;
  std::uint64_t stream_mask = 0;
  std::int64_t min_time_ns = 0;
  std::int64_t max_time_ns = 0;
  std::uint32_t record_count = 0;
  std::uint32_t total_size = 0;  // header + compressed payload
  std::uint32_t uncompressed_size = 0;
};

struct StreamStats {
  StreamId id = 0;
  std::uint64_t records = 0;
  std::uint64_t payload_bytes = 0;  // sum of record payload sizes
  std::uint64_t stored_bytes = 0;   // bytes of chunks holding only this stream
  std::int64_t first_time_ns = 0;
  std::int64_t last_time_ns = 0;
  std::uint64_t dropped = 0;  // lost in the ring (drop-newest + drop-oldest)
  std::uint64_t seq_gaps = 0;
  std::uint64_t ts_regressions = 0;
  std::uint64_t rate_events = 0;
  std::uint64_t stale_events = 0;
};

struct Footer {
  std::uint16_t flags = 0;
  std::int64_t start_time_ns = 0;
  std::int64_t end_time_ns = 0;
  std::vector<ChunkIndexEntry> chunks;
  std::vector<StreamStats> streams;
};

struct Trailer {
  std::uint64_t footer_offset = 0;
  std::uint32_t footer_size = 0;
  std::uint32_t footer_crc = 0;
};

// Encodes footer followed by trailer; `footer_offset` is the file offset at
// which the returned bytes will be written.
[[nodiscard]] std::vector<std::byte> encode_footer(const Footer& footer,
                                                   std::uint64_t footer_offset);
[[nodiscard]] Result<Trailer> decode_trailer(ByteSpan last_bytes);
[[nodiscard]] Result<Footer> decode_footer(ByteSpan footer_bytes, const Trailer& trailer);
// Convenience: locate and decode footer from a complete file image.
[[nodiscard]] Result<Footer> read_footer(ByteSpan file);

}  // namespace mr::format
