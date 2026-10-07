#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <mr/chunk.hpp>
#include <mr/format.hpp>

namespace {

namespace fmt = mr::format;
using mr::ByteSpan;
using mr::Errc;

std::vector<std::byte> make_payload(std::size_t n, std::uint64_t seed, bool compressible) {
  std::vector<std::byte> out(n);
  std::mt19937_64 rng(seed);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = compressible ? static_cast<std::byte>((i / 16) & 0xFFU)
                          : static_cast<std::byte>(rng() & 0xFFU);
  }
  return out;
}

struct BuiltChunk {
  std::vector<std::byte> records;
  fmt::ChunkHeader meta;
  std::vector<std::vector<std::byte>> payloads;
};

BuiltChunk build_records(int count, bool compressible) {
  BuiltChunk b;
  b.meta.chunk_seq = 5;
  for (int i = 0; i < count; ++i) {
    mr::RecordMeta m{.timestamp_ns = 1'000'000 + i * 5'000, .seq = static_cast<std::uint64_t>(i),
                     .stream_id = static_cast<mr::StreamId>(i % 3), .flags = 0};
    b.payloads.push_back(make_payload(static_cast<std::size_t>(i * 7 % 300), static_cast<std::uint64_t>(i), compressible));
    fmt::append_record(b.records, m, b.payloads.back());
    b.meta.record_count++;
    b.meta.stream_mask |= std::uint64_t{1} << m.stream_id;
    b.meta.min_time_ns = std::min(b.meta.min_time_ns, m.timestamp_ns);
    b.meta.max_time_ns = std::max(b.meta.max_time_ns, m.timestamp_ns);
  }
  return b;
}

TEST(FileHeader, RoundTrip) {
  fmt::FileHeader h;
  h.created_unix_ns = 1'700'000'000'000'000'000;
  h.streams.push_back({.id = 0, .kind = mr::SensorKind::kImu, .nominal_rate_hz = 200.0, .name = "imu"});
  h.streams.push_back({.id = 3, .kind = mr::SensorKind::kCamera, .nominal_rate_hz = 29.97, .name = "camera_front"});
  const auto bytes = fmt::encode_file_header(h);
  auto decoded = fmt::decode_file_header(bytes);
  ASSERT_TRUE(decoded) << decoded.error().describe();
  EXPECT_EQ(decoded->encoded_size, bytes.size());
  EXPECT_EQ(decoded->created_unix_ns, h.created_unix_ns);
  ASSERT_EQ(decoded->streams.size(), 2U);
  EXPECT_EQ(decoded->streams[1].name, "camera_front");
  EXPECT_EQ(decoded->streams[1].kind, mr::SensorKind::kCamera);
  EXPECT_DOUBLE_EQ(decoded->streams[1].nominal_rate_hz, 29.97);
}

TEST(FileHeader, DetectsCorruptionAndTruncation) {
  fmt::FileHeader h;
  h.streams.push_back({.id = 1, .kind = mr::SensorKind::kGps, .nominal_rate_hz = 10.0, .name = "gps"});
  auto bytes = fmt::encode_file_header(h);
  EXPECT_EQ(fmt::decode_file_header(ByteSpan(bytes).first(bytes.size() - 1)).error().code, Errc::kTruncated);
  bytes[40] ^= std::byte{1};
  EXPECT_EQ(fmt::decode_file_header(bytes).error().code, Errc::kCorrupt);
  bytes[0] = std::byte{'X'};
  EXPECT_EQ(fmt::decode_file_header(bytes).error().code, Errc::kCorrupt);
}

TEST(Records, ParserRoundTripAndBounds) {
  const BuiltChunk b = build_records(50, false);
  fmt::RecordParser parser(b.records);
  fmt::RecordView rec;
  int n = 0;
  while (true) {
    auto more = parser.next(rec);
    ASSERT_TRUE(more);
    if (!more.value()) {
      break;
    }
    EXPECT_EQ(rec.meta.seq, static_cast<std::uint64_t>(n));
    ASSERT_EQ(rec.payload.size(), b.payloads[static_cast<std::size_t>(n)].size());
    EXPECT_TRUE(std::equal(rec.payload.begin(), rec.payload.end(), b.payloads[static_cast<std::size_t>(n)].begin()));
    ++n;
  }
  EXPECT_EQ(n, 50);

  // Truncating anywhere inside the last record must be reported, not read past.
  for (std::size_t cut = 1; cut < 24; ++cut) {
    fmt::RecordParser p2(ByteSpan(b.records).first(b.records.size() - cut));
    Errc last = Errc::kIo;
    bool failed = false;
    while (true) {
      auto more = p2.next(rec);
      if (!more) {
        failed = true;
        last = more.error().code;
        break;
      }
      if (!more.value()) {
        break;
      }
    }
    EXPECT_TRUE(failed) << "cut " << cut;
    EXPECT_EQ(last, Errc::kCorrupt);
  }
}

class ChunkRoundTrip : public ::testing::TestWithParam<std::tuple<int, bool>> {};

TEST_P(ChunkRoundTrip, EncodeDecodeVerify) {
  const auto [level, compressible] = GetParam();
  const BuiltChunk b = build_records(200, compressible);
  mr::ZstdCompressor compressor(level);
  mr::ZstdDecompressor decompressor;
  std::vector<std::byte> scratch;
  std::vector<std::byte> chunk;
  auto header = mr::encode_chunk(b.meta, b.records, compressor, scratch, chunk);
  ASSERT_TRUE(header) << header.error().describe();
  EXPECT_EQ(chunk.size(), header->total_size());
  EXPECT_EQ(header->uncompressed_size, b.records.size());
  if (level > 0 && compressible) {
    EXPECT_EQ(header->compression, fmt::Compression::kZstd);
    EXPECT_LT(header->compressed_size, header->uncompressed_size);
  }
  if (level == 0) {
    EXPECT_EQ(header->compression, fmt::Compression::kNone);
  }

  std::vector<std::byte> body;
  auto verified = mr::parse_and_verify_chunk(chunk, decompressor, body);
  ASSERT_TRUE(verified) << verified.error().describe();
  EXPECT_EQ(verified->chunk_seq, 5U);
  EXPECT_EQ(verified->record_count, 200U);
  EXPECT_EQ(body, b.records);
}

INSTANTIATE_TEST_SUITE_P(LevelsAndData, ChunkRoundTrip,
                         ::testing::Combine(::testing::Values(0, 1, 3, 9), ::testing::Bool()));

TEST(Chunk, IncompressibleBodyIsStoredRaw) {
  fmt::ChunkHeader meta;
  std::vector<std::byte> records;
  const auto payload = make_payload(64 * 1024, 99, false);
  fmt::append_record(records, {.timestamp_ns = 1, .seq = 0, .stream_id = 0, .flags = 0}, payload);
  meta.record_count = 1;
  meta.stream_mask = 1;
  meta.min_time_ns = meta.max_time_ns = 1;
  mr::ZstdCompressor compressor(3);
  std::vector<std::byte> scratch;
  std::vector<std::byte> chunk;
  auto header = mr::encode_chunk(meta, records, compressor, scratch, chunk);
  ASSERT_TRUE(header);
  EXPECT_EQ(header->compression, fmt::Compression::kNone);
  EXPECT_EQ(header->compressed_size, records.size());
}

TEST(Chunk, DetectsEveryHeaderByteFlip) {
  const BuiltChunk b = build_records(20, true);
  mr::ZstdCompressor compressor(3);
  mr::ZstdDecompressor decompressor;
  std::vector<std::byte> scratch;
  std::vector<std::byte> chunk;
  ASSERT_TRUE(mr::encode_chunk(b.meta, b.records, compressor, scratch, chunk));
  std::vector<std::byte> body;
  for (std::size_t i = 0; i < fmt::kChunkHeaderSize; ++i) {
    auto copy = chunk;
    copy[i] ^= std::byte{0x10};
    EXPECT_FALSE(mr::parse_and_verify_chunk(copy, decompressor, body)) << "byte " << i;
  }
}

TEST(Chunk, DetectsBodyCorruptionAndTruncation) {
  const BuiltChunk b = build_records(20, true);
  mr::ZstdCompressor compressor(3);
  mr::ZstdDecompressor decompressor;
  std::vector<std::byte> scratch;
  std::vector<std::byte> chunk;
  ASSERT_TRUE(mr::encode_chunk(b.meta, b.records, compressor, scratch, chunk));
  std::vector<std::byte> body;

  auto flipped = chunk;
  flipped[fmt::kChunkHeaderSize + 3] ^= std::byte{0x01};
  auto r1 = mr::parse_and_verify_chunk(flipped, decompressor, body);
  ASSERT_FALSE(r1);
  EXPECT_EQ(r1.error().code, Errc::kCorrupt);

  auto r2 = mr::parse_and_verify_chunk(ByteSpan(chunk).first(chunk.size() - 1), decompressor, body);
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().code, Errc::kTruncated);
}

TEST(Footer, RoundTripAndCrc) {
  fmt::Footer f;
  f.start_time_ns = 10;
  f.end_time_ns = 99;
  f.chunks.push_back({.offset = 100, .stream_mask = 1, .min_time_ns = 10, .max_time_ns = 50,
                      .record_count = 3, .total_size = 200, .uncompressed_size = 300});
  f.chunks.push_back({.offset = 300, .stream_mask = 2, .min_time_ns = 20, .max_time_ns = 99,
                      .record_count = 4, .total_size = 100, .uncompressed_size = 400});
  f.streams.push_back({.id = 0, .records = 3, .payload_bytes = 30, .stored_bytes = 200,
                       .first_time_ns = 10, .last_time_ns = 50, .dropped = 1, .seq_gaps = 2,
                       .ts_regressions = 3, .rate_events = 4, .stale_events = 5});
  const std::uint64_t footer_offset = 400;
  std::vector<std::byte> file(footer_offset, std::byte{0});
  const auto encoded = fmt::encode_footer(f, footer_offset);
  file.insert(file.end(), encoded.begin(), encoded.end());

  auto decoded = fmt::read_footer(file);
  ASSERT_TRUE(decoded) << decoded.error().describe();
  ASSERT_EQ(decoded->chunks.size(), 2U);
  EXPECT_EQ(decoded->chunks[1].offset, 300U);
  EXPECT_EQ(decoded->chunks[1].max_time_ns, 99);
  ASSERT_EQ(decoded->streams.size(), 1U);
  EXPECT_EQ(decoded->streams[0].stale_events, 5U);
  EXPECT_EQ(decoded->end_time_ns, 99);

  auto corrupted = file;
  corrupted[footer_offset + 40] ^= std::byte{0x80};
  EXPECT_EQ(fmt::read_footer(corrupted).error().code, Errc::kCorrupt);

  auto no_trailer = file;
  no_trailer.resize(no_trailer.size() - 1);
  EXPECT_FALSE(fmt::read_footer(no_trailer));
}

TEST(Footer, RejectsIndexPointingPastData) {
  fmt::Footer f;
  f.chunks.push_back({.offset = 390, .stream_mask = 1, .min_time_ns = 0, .max_time_ns = 0,
                      .record_count = 1, .total_size = 100, .uncompressed_size = 24});
  std::vector<std::byte> file(400, std::byte{0});
  const auto encoded = fmt::encode_footer(f, 400);
  file.insert(file.end(), encoded.begin(), encoded.end());
  EXPECT_EQ(fmt::read_footer(file).error().code, Errc::kCorrupt);
}

}  // namespace
