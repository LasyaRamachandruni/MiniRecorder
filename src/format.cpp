#include <mr/format.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <mr/crc32c.hpp>

namespace mr::format {
namespace {

Error corrupt(std::string what) { return make_error(Errc::kCorrupt, std::move(what)); }
Error truncated(std::string what) { return make_error(Errc::kTruncated, std::move(what)); }

bool magic_equals(ByteSpan data, const std::array<std::byte, 8>& magic) {
  return data.size() >= magic.size() && std::equal(magic.begin(), magic.end(), data.begin());
}

std::uint32_t hz_to_millihz(double hz) {
  if (!(hz > 0.0)) {
    return 0;
  }
  const double millihz = std::round(hz * 1000.0);
  return millihz >= 4.0e9 ? 4'000'000'000U : static_cast<std::uint32_t>(millihz);
}

}  // namespace

// ---------------------------------------------------------------- file header

std::vector<std::byte> encode_file_header(const FileHeader& header) {
  std::vector<std::byte> out;
  out.reserve(kFileHeaderFixedSize + header.streams.size() * kStreamEntrySize + 4);
  ByteWriter w(out);
  w.put_bytes(kFileMagic);
  w.put<std::uint16_t>(kVersion);
  w.put<std::uint16_t>(static_cast<std::uint16_t>(header.streams.size()));
  w.put<std::uint32_t>(
      static_cast<std::uint32_t>(kFileHeaderFixedSize + header.streams.size() * kStreamEntrySize + 4));
  w.put<std::int64_t>(header.created_unix_ns);
  w.put<std::uint32_t>(header.flags);
  w.put<std::uint32_t>(0);  // reserved
  for (const StreamInfo& s : header.streams) {
    w.put<std::uint16_t>(s.id);
    w.put<std::uint16_t>(static_cast<std::uint16_t>(s.kind));
    w.put<std::uint32_t>(hz_to_millihz(s.nominal_rate_hz));
    w.put_fixed_string(s.name, kStreamNameMax);
  }
  w.put<std::uint32_t>(crc32c::value(out));
  return out;
}

Result<FileHeader> decode_file_header(ByteSpan data) {
  if (data.size() < kFileHeaderFixedSize + 4) {
    return truncated("file header shorter than fixed part");
  }
  if (!magic_equals(data, kFileMagic)) {
    return corrupt("bad file magic (not a MiniRecorder log)");
  }
  ByteReader r(data.subspan(kFileMagic.size()));
  const auto version = r.get<std::uint16_t>();
  const auto stream_count = r.get<std::uint16_t>();
  const auto header_size = r.get<std::uint32_t>();
  if (version != kVersion) {
    return make_error(Errc::kUnsupported, "unsupported format version " + std::to_string(version));
  }
  if (stream_count > kMaxStreams ||
      header_size != kFileHeaderFixedSize + stream_count * kStreamEntrySize + 4) {
    return corrupt("inconsistent file header size");
  }
  if (data.size() < header_size) {
    return truncated("file header truncated");
  }
  const auto stored_crc = load_le<std::uint32_t>(data.data() + header_size - 4);
  if (crc32c::value(data.first(header_size - 4)) != stored_crc) {
    return corrupt("file header CRC mismatch");
  }

  FileHeader header;
  header.created_unix_ns = r.get<std::int64_t>();
  header.flags = r.get<std::uint32_t>();
  r.skip(4);
  std::uint64_t seen_ids = 0;
  for (std::uint16_t i = 0; i < stream_count; ++i) {
    StreamInfo s;
    s.id = r.get<std::uint16_t>();
    s.kind = static_cast<SensorKind>(r.get<std::uint16_t>());
    s.nominal_rate_hz = static_cast<double>(r.get<std::uint32_t>()) / 1000.0;
    s.name = r.get_fixed_string(kStreamNameMax);
    if (s.id >= kMaxStreams || (seen_ids & (std::uint64_t{1} << s.id)) != 0) {
      return corrupt("invalid or duplicate stream id in file header");
    }
    seen_ids |= std::uint64_t{1} << s.id;
    header.streams.push_back(std::move(s));
  }
  if (!r.ok()) {
    return truncated("file header truncated");
  }
  header.encoded_size = header_size;
  return header;
}

// ---------------------------------------------------------------- chunk header

void encode_chunk_header(const ChunkHeader& h, std::span<std::byte, kChunkHeaderSize> out) {
  std::byte* p = out.data();
  store_le<std::uint32_t>(p + 0, kChunkMagic);
  store_le<std::uint16_t>(p + 4, kVersion);
  store_le<std::uint16_t>(p + 6, static_cast<std::uint16_t>(kChunkHeaderSize));
  store_le<std::uint32_t>(p + 8, static_cast<std::uint32_t>(h.compression));
  store_le<std::uint32_t>(p + 12, h.record_count);
  store_le<std::uint64_t>(p + 16, h.stream_mask);
  store_le<std::int64_t>(p + 24, h.min_time_ns);
  store_le<std::int64_t>(p + 32, h.max_time_ns);
  store_le<std::uint32_t>(p + 40, h.uncompressed_size);
  store_le<std::uint32_t>(p + 44, h.compressed_size);
  store_le<std::uint64_t>(p + 48, h.chunk_seq);
  store_le<std::uint32_t>(p + 56, h.payload_crc);
  store_le<std::uint32_t>(p + 60, crc32c::value(ByteSpan(p, 60)));
}

Result<ChunkHeader> decode_chunk_header(ByteSpan data) {
  if (data.size() < kChunkHeaderSize) {
    return truncated("chunk header truncated");
  }
  const std::byte* p = data.data();
  if (load_le<std::uint32_t>(p) != kChunkMagic) {
    return corrupt("bad chunk magic");
  }
  if (crc32c::value(data.first(60)) != load_le<std::uint32_t>(p + 60)) {
    return corrupt("chunk header CRC mismatch");
  }
  if (load_le<std::uint16_t>(p + 4) != kVersion ||
      load_le<std::uint16_t>(p + 6) != kChunkHeaderSize) {
    return make_error(Errc::kUnsupported, "unsupported chunk version");
  }
  ChunkHeader h;
  const auto compression = load_le<std::uint32_t>(p + 8);
  if (compression > static_cast<std::uint32_t>(Compression::kZstd)) {
    return make_error(Errc::kUnsupported, "unknown chunk compression");
  }
  h.compression = static_cast<Compression>(compression);
  h.record_count = load_le<std::uint32_t>(p + 12);
  h.stream_mask = load_le<std::uint64_t>(p + 16);
  h.min_time_ns = load_le<std::int64_t>(p + 24);
  h.max_time_ns = load_le<std::int64_t>(p + 32);
  h.uncompressed_size = load_le<std::uint32_t>(p + 40);
  h.compressed_size = load_le<std::uint32_t>(p + 44);
  h.chunk_seq = load_le<std::uint64_t>(p + 48);
  h.payload_crc = load_le<std::uint32_t>(p + 56);
  if (h.uncompressed_size > kMaxChunkBytes || h.compressed_size > kMaxChunkBytes) {
    return corrupt("chunk size exceeds limit");
  }
  if (h.compression == Compression::kNone && h.compressed_size != h.uncompressed_size) {
    return corrupt("uncompressed chunk with mismatched sizes");
  }
  if (static_cast<std::uint64_t>(h.record_count) * kRecordHeaderSize > h.uncompressed_size) {
    return corrupt("record count does not fit in chunk");
  }
  if (h.record_count > 0 && h.min_time_ns > h.max_time_ns) {
    return corrupt("chunk time range inverted");
  }
  return h;
}

// ---------------------------------------------------------------- records

void append_record(std::vector<std::byte>& out, const RecordMeta& meta, ByteSpan payload) {
  const std::size_t pos = out.size();
  out.resize(pos + kRecordHeaderSize + payload.size());
  std::byte* p = out.data() + pos;
  store_le<std::int64_t>(p + 0, meta.timestamp_ns);
  store_le<std::uint64_t>(p + 8, meta.seq);
  store_le<std::uint16_t>(p + 16, meta.stream_id);
  store_le<std::uint16_t>(p + 18, meta.flags);
  store_le<std::uint32_t>(p + 20, static_cast<std::uint32_t>(payload.size()));
  if (!payload.empty()) {
    std::memcpy(p + kRecordHeaderSize, payload.data(), payload.size());
  }
}

Result<bool> RecordParser::next(RecordView& out) {
  if (pos_ == body_.size()) {
    return false;
  }
  if (body_.size() - pos_ < kRecordHeaderSize) {
    return corrupt("record header crosses chunk end");
  }
  const std::byte* p = body_.data() + pos_;
  out.meta.timestamp_ns = load_le<std::int64_t>(p + 0);
  out.meta.seq = load_le<std::uint64_t>(p + 8);
  out.meta.stream_id = load_le<std::uint16_t>(p + 16);
  out.meta.flags = load_le<std::uint16_t>(p + 18);
  const auto size = load_le<std::uint32_t>(p + 20);
  if (out.meta.stream_id >= kMaxStreams) {
    return corrupt("record stream id out of range");
  }
  if (size > kMaxRecordPayload || body_.size() - pos_ - kRecordHeaderSize < size) {
    return corrupt("record payload crosses chunk end");
  }
  out.payload = body_.subspan(pos_ + kRecordHeaderSize, size);
  pos_ += kRecordHeaderSize + size;
  return true;
}

// ---------------------------------------------------------------- footer

std::vector<std::byte> encode_footer(const Footer& footer, std::uint64_t footer_offset) {
  std::vector<std::byte> out;
  out.reserve(kFooterFixedSize + footer.chunks.size() * kChunkIndexEntrySize +
              footer.streams.size() * kStreamStatsEntrySize + kTrailerSize);
  ByteWriter w(out);
  w.put<std::uint32_t>(kFooterMagic);
  w.put<std::uint16_t>(kVersion);
  w.put<std::uint16_t>(footer.flags);
  w.put<std::uint32_t>(static_cast<std::uint32_t>(footer.chunks.size()));
  w.put<std::uint32_t>(static_cast<std::uint32_t>(footer.streams.size()));
  w.put<std::int64_t>(footer.start_time_ns);
  w.put<std::int64_t>(footer.end_time_ns);
  for (const ChunkIndexEntry& c : footer.chunks) {
    w.put<std::uint64_t>(c.offset);
    w.put<std::uint64_t>(c.stream_mask);
    w.put<std::int64_t>(c.min_time_ns);
    w.put<std::int64_t>(c.max_time_ns);
    w.put<std::uint32_t>(c.record_count);
    w.put<std::uint32_t>(c.total_size);
    w.put<std::uint32_t>(c.uncompressed_size);
    w.put<std::uint32_t>(0);
  }
  for (const StreamStats& s : footer.streams) {
    w.put<std::uint16_t>(s.id);
    w.put<std::uint16_t>(0);
    w.put<std::uint32_t>(0);
    w.put<std::uint64_t>(s.records);
    w.put<std::uint64_t>(s.payload_bytes);
    w.put<std::uint64_t>(s.stored_bytes);
    w.put<std::int64_t>(s.first_time_ns);
    w.put<std::int64_t>(s.last_time_ns);
    w.put<std::uint64_t>(s.dropped);
    w.put<std::uint64_t>(s.seq_gaps);
    w.put<std::uint64_t>(s.ts_regressions);
    w.put<std::uint64_t>(s.rate_events);
    w.put<std::uint64_t>(s.stale_events);
  }
  const auto footer_size = static_cast<std::uint32_t>(out.size());
  const std::uint32_t footer_crc = crc32c::value(out);
  w.put<std::uint64_t>(footer_offset);
  w.put<std::uint32_t>(footer_size);
  w.put<std::uint32_t>(footer_crc);
  w.put_bytes(kTrailerMagic);
  return out;
}

Result<Trailer> decode_trailer(ByteSpan last_bytes) {
  if (last_bytes.size() < kTrailerSize) {
    return truncated("file too small for trailer");
  }
  const ByteSpan t = last_bytes.last(kTrailerSize);
  if (!magic_equals(t.subspan(16), kTrailerMagic)) {
    return make_error(Errc::kNotFound, "no trailer (file was not closed cleanly)");
  }
  Trailer trailer;
  trailer.footer_offset = load_le<std::uint64_t>(t.data());
  trailer.footer_size = load_le<std::uint32_t>(t.data() + 8);
  trailer.footer_crc = load_le<std::uint32_t>(t.data() + 12);
  return trailer;
}

Result<Footer> decode_footer(ByteSpan bytes, const Trailer& trailer) {
  if (bytes.size() != trailer.footer_size || bytes.size() < kFooterFixedSize) {
    return corrupt("footer size mismatch");
  }
  if (crc32c::value(bytes) != trailer.footer_crc) {
    return corrupt("footer CRC mismatch");
  }
  ByteReader r(bytes);
  if (r.get<std::uint32_t>() != kFooterMagic) {
    return corrupt("bad footer magic");
  }
  if (r.get<std::uint16_t>() != kVersion) {
    return make_error(Errc::kUnsupported, "unsupported footer version");
  }
  Footer f;
  f.flags = r.get<std::uint16_t>();
  const auto chunk_count = r.get<std::uint32_t>();
  const auto stream_count = r.get<std::uint32_t>();
  if (stream_count > kMaxStreams ||
      bytes.size() != kFooterFixedSize + std::uint64_t{chunk_count} * kChunkIndexEntrySize +
                          std::uint64_t{stream_count} * kStreamStatsEntrySize) {
    return corrupt("footer counts inconsistent with footer size");
  }
  f.start_time_ns = r.get<std::int64_t>();
  f.end_time_ns = r.get<std::int64_t>();
  f.chunks.reserve(chunk_count);
  for (std::uint32_t i = 0; i < chunk_count; ++i) {
    ChunkIndexEntry c;
    c.offset = r.get<std::uint64_t>();
    c.stream_mask = r.get<std::uint64_t>();
    c.min_time_ns = r.get<std::int64_t>();
    c.max_time_ns = r.get<std::int64_t>();
    c.record_count = r.get<std::uint32_t>();
    c.total_size = r.get<std::uint32_t>();
    c.uncompressed_size = r.get<std::uint32_t>();
    r.skip(4);
    if (c.offset > trailer.footer_offset || trailer.footer_offset - c.offset < c.total_size ||
        c.total_size < kChunkHeaderSize) {
      return corrupt("chunk index entry points outside the data region");
    }
    f.chunks.push_back(c);
  }
  f.streams.reserve(stream_count);
  for (std::uint32_t i = 0; i < stream_count; ++i) {
    StreamStats s;
    s.id = r.get<std::uint16_t>();
    r.skip(6);
    s.records = r.get<std::uint64_t>();
    s.payload_bytes = r.get<std::uint64_t>();
    s.stored_bytes = r.get<std::uint64_t>();
    s.first_time_ns = r.get<std::int64_t>();
    s.last_time_ns = r.get<std::int64_t>();
    s.dropped = r.get<std::uint64_t>();
    s.seq_gaps = r.get<std::uint64_t>();
    s.ts_regressions = r.get<std::uint64_t>();
    s.rate_events = r.get<std::uint64_t>();
    s.stale_events = r.get<std::uint64_t>();
    f.streams.push_back(s);
  }
  if (!r.ok()) {
    return truncated("footer truncated");
  }
  return f;
}

Result<Footer> read_footer(ByteSpan file) {
  auto trailer = decode_trailer(file);
  if (!trailer) {
    return std::move(trailer).error();
  }
  const Trailer& t = trailer.value();
  if (t.footer_offset > file.size() || file.size() - t.footer_offset < kTrailerSize ||
      file.size() - t.footer_offset - kTrailerSize != t.footer_size) {
    return corrupt("trailer footer location inconsistent with file size");
  }
  return decode_footer(file.subspan(t.footer_offset, t.footer_size), t);
}

}  // namespace mr::format
