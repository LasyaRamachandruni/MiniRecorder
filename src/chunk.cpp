#include <mr/chunk.hpp>

#include <zstd.h>

#include <cstring>
#include <new>
#include <string>

#include <mr/crc32c.hpp>

namespace mr {

void ZstdCompressor::Deleter::operator()(ZSTD_CCtx_s* ctx) const noexcept { ZSTD_freeCCtx(ctx); }
void ZstdDecompressor::Deleter::operator()(ZSTD_DCtx_s* ctx) const noexcept { ZSTD_freeDCtx(ctx); }

ZstdCompressor::ZstdCompressor(int level) : level_(level) {
  if (level_ > 0) {
    ctx_.reset(ZSTD_createCCtx());
    if (!ctx_) {
      throw std::bad_alloc();
    }
  }
}

Status ZstdCompressor::compress(ByteSpan in, std::vector<std::byte>& out) {
  if (!ctx_) {
    return make_error(Errc::kInvalidArgument, "compression disabled");
  }
  out.resize(ZSTD_compressBound(in.size()));
  const std::size_t n =
      ZSTD_compressCCtx(ctx_.get(), out.data(), out.size(), in.data(), in.size(), level_);
  if (ZSTD_isError(n) != 0U) {
    return make_error(Errc::kCompression, ZSTD_getErrorName(n));
  }
  out.resize(n);
  return ok_status();
}

ZstdDecompressor::ZstdDecompressor() : ctx_(ZSTD_createDCtx()) {
  if (!ctx_) {
    throw std::bad_alloc();
  }
}

Status ZstdDecompressor::decompress(ByteSpan in, std::size_t expected_size,
                                    std::vector<std::byte>& out) {
  out.resize(expected_size);
  const std::size_t n = ZSTD_decompressDCtx(ctx_.get(), out.data(), out.size(), in.data(), in.size());
  if (ZSTD_isError(n) != 0U) {
    return make_error(Errc::kCorrupt, std::string("zstd: ") + ZSTD_getErrorName(n));
  }
  if (n != expected_size) {
    return make_error(Errc::kCorrupt, "decompressed size does not match chunk header");
  }
  return ok_status();
}

Result<format::ChunkHeader> encode_chunk(const format::ChunkHeader& meta, ByteSpan records,
                                         ZstdCompressor& compressor, std::vector<std::byte>& scratch,
                                         std::vector<std::byte>& out) {
  if (records.size() > format::kMaxChunkBytes) {
    return make_error(Errc::kInvalidArgument, "chunk body exceeds kMaxChunkBytes");
  }
  format::ChunkHeader header = meta;
  header.uncompressed_size = static_cast<std::uint32_t>(records.size());

  ByteSpan body = records;
  header.compression = format::Compression::kNone;
  if (compressor.level() > 0) {
    MR_RETURN_IF_ERROR(compressor.compress(records, scratch));
    if (scratch.size() < records.size()) {
      body = scratch;
      header.compression = format::Compression::kZstd;
    }
  }
  header.compressed_size = static_cast<std::uint32_t>(body.size());
  header.payload_crc = crc32c::value(body);

  out.resize(format::kChunkHeaderSize + body.size());
  format::encode_chunk_header(header, std::span<std::byte, format::kChunkHeaderSize>(
                                          out.data(), format::kChunkHeaderSize));
  if (!body.empty()) {
    std::memcpy(out.data() + format::kChunkHeaderSize, body.data(), body.size());
  }
  return header;
}

Status decode_chunk_body(const format::ChunkHeader& header, ByteSpan body,
                         ZstdDecompressor& decompressor, std::vector<std::byte>& out) {
  if (body.size() != header.compressed_size) {
    return make_error(Errc::kTruncated, "chunk body size mismatch");
  }
  if (crc32c::value(body) != header.payload_crc) {
    return make_error(Errc::kCorrupt, "chunk body CRC mismatch");
  }
  switch (header.compression) {
    case format::Compression::kNone:
      out.assign(body.begin(), body.end());
      return ok_status();
    case format::Compression::kZstd:
      return decompressor.decompress(body, header.uncompressed_size, out);
  }
  return make_error(Errc::kUnsupported, "unknown compression");
}

Result<format::ChunkHeader> parse_and_verify_chunk(ByteSpan data, ZstdDecompressor& decompressor,
                                                   std::vector<std::byte>& out) {
  auto header = format::decode_chunk_header(data);
  if (!header) {
    return std::move(header).error();
  }
  const format::ChunkHeader& h = header.value();
  if (data.size() - format::kChunkHeaderSize < h.compressed_size) {
    return make_error(Errc::kTruncated, "chunk body truncated");
  }
  MR_RETURN_IF_ERROR(decode_chunk_body(
      h, data.subspan(format::kChunkHeaderSize, h.compressed_size), decompressor, out));

  format::RecordParser parser(out);
  format::RecordView rec;
  std::uint32_t count = 0;
  std::uint64_t mask = 0;
  for (;;) {
    auto more = parser.next(rec);
    if (!more) {
      return std::move(more).error();
    }
    if (!more.value()) {
      break;
    }
    ++count;
    mask |= std::uint64_t{1} << rec.meta.stream_id;
    if (rec.meta.timestamp_ns < h.min_time_ns || rec.meta.timestamp_ns > h.max_time_ns) {
      return make_error(Errc::kCorrupt, "record timestamp outside chunk time range");
    }
  }
  if (count != h.record_count) {
    return make_error(Errc::kCorrupt, "record count does not match chunk header");
  }
  if ((mask & ~h.stream_mask) != 0) {
    return make_error(Errc::kCorrupt, "record stream not in chunk stream mask");
  }
  return h;
}

}  // namespace mr
