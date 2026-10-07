#pragma once

// Chunk codec: turns a buffer of encoded records into an on-disk chunk
// (header + optionally zstd-compressed body) and back, verifying CRCs.

#include <cstddef>
#include <memory>
#include <vector>

#include <mr/bytes.hpp>
#include <mr/format.hpp>
#include <mr/result.hpp>

// Forward declarations keep <zstd.h> out of the public headers.
struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace mr {

class ZstdCompressor {
 public:
  // level <= 0 disables compression (chunks are stored raw).
  explicit ZstdCompressor(int level);
  [[nodiscard]] int level() const noexcept { return level_; }

  // Compresses `in` into `out` (resized to the compressed size).
  [[nodiscard]] Status compress(ByteSpan in, std::vector<std::byte>& out);

 private:
  struct Deleter {
    void operator()(ZSTD_CCtx_s* ctx) const noexcept;
  };
  int level_;
  std::unique_ptr<ZSTD_CCtx_s, Deleter> ctx_;
};

class ZstdDecompressor {
 public:
  ZstdDecompressor();
  // Decompresses `in`, which must expand to exactly `expected_size` bytes.
  [[nodiscard]] Status decompress(ByteSpan in, std::size_t expected_size,
                                  std::vector<std::byte>& out);

 private:
  struct Deleter {
    void operator()(ZSTD_DCtx_s* ctx) const noexcept;
  };
  std::unique_ptr<ZSTD_DCtx_s, Deleter> ctx_;
};

// Builds a complete chunk (header + body) into `out`. `meta` supplies
// record_count, stream_mask, time range and chunk_seq; sizes, compression and
// CRC are filled in here. If compression does not shrink the body it is stored
// raw. Returns the final header.
[[nodiscard]] Result<format::ChunkHeader> encode_chunk(const format::ChunkHeader& meta,
                                                       ByteSpan records, ZstdCompressor& compressor,
                                                       std::vector<std::byte>& scratch,
                                                       std::vector<std::byte>& out);

// Verifies the body CRC and produces the uncompressed record bytes in `out`.
[[nodiscard]] Status decode_chunk_body(const format::ChunkHeader& header, ByteSpan body,
                                       ZstdDecompressor& decompressor, std::vector<std::byte>& out);

// Parses the chunk starting at `data` (header, bounds, CRC, decompression)
// and checks that the records inside are well formed and agree with the
// header. Used by recovery, the reader's verify mode and the fuzzer.
[[nodiscard]] Result<format::ChunkHeader> parse_and_verify_chunk(ByteSpan data,
                                                                 ZstdDecompressor& decompressor,
                                                                 std::vector<std::byte>& out);

}  // namespace mr
