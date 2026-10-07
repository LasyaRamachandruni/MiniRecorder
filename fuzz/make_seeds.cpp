// Writes a few valid inputs (compressed chunk, raw chunk, record body, small
// file image with footer) into a directory to seed the fuzzer corpus.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include <mr/chunk.hpp>
#include <mr/events.hpp>
#include <mr/format.hpp>

namespace {

void write(const std::filesystem::path& p, const std::vector<std::byte>& bytes) {
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));  // NOLINT
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: make_seeds <dir>\n");  // NOLINT
    return 2;
  }
  const std::filesystem::path dir = argv[1];  // NOLINT
  std::filesystem::create_directories(dir);

  std::vector<std::byte> records;
  mr::format::ChunkHeader meta;
  for (std::uint64_t i = 0; i < 20; ++i) {
    std::vector<std::byte> payload(8 + i * 3, std::byte{static_cast<unsigned char>(i)});
    const mr::RecordMeta m{.timestamp_ns = static_cast<std::int64_t>(1000 + i * 10), .seq = i, .stream_id = 2, .flags = 0};
    mr::format::append_record(records, m, payload);
    ++meta.record_count;
    meta.stream_mask = 1U << 2;
    meta.min_time_ns = std::min(meta.min_time_ns, m.timestamp_ns);
    meta.max_time_ns = std::max(meta.max_time_ns, m.timestamp_ns);
  }
  std::array<std::byte, mr::kEventPayloadSize> ev{};
  mr::encode_event({.stream = 2, .kind = mr::EventKind::kSeqGap, .timestamp_ns = 1, .value = 3, .expected = 0}, ev);
  mr::format::append_record(records, {.timestamp_ns = 1100, .seq = 0, .stream_id = mr::kEventStreamId, .flags = 0}, ev);
  ++meta.record_count;
  meta.stream_mask |= std::uint64_t{1} << mr::kEventStreamId;
  meta.max_time_ns = 1100;
  write(dir / "records.bin", records);

  std::vector<std::byte> scratch;
  std::vector<std::byte> chunk;
  mr::ZstdCompressor zstd(3);
  if (!mr::encode_chunk(meta, records, zstd, scratch, chunk)) {
    return 1;
  }
  write(dir / "chunk_zstd.bin", chunk);
  mr::ZstdCompressor raw(0);
  if (!mr::encode_chunk(meta, records, raw, scratch, chunk)) {
    return 1;
  }
  write(dir / "chunk_raw.bin", chunk);

  mr::format::FileHeader header;
  header.streams.push_back({.id = 2, .kind = mr::SensorKind::kImu, .nominal_rate_hz = 100, .name = "imu"});
  std::vector<std::byte> file = mr::format::encode_file_header(header);
  const std::uint64_t chunk_offset = file.size();
  file.insert(file.end(), chunk.begin(), chunk.end());
  mr::format::Footer footer;
  footer.chunks.push_back({.offset = chunk_offset, .stream_mask = meta.stream_mask, .min_time_ns = 1000,
                           .max_time_ns = 1100, .record_count = meta.record_count,
                           .total_size = static_cast<std::uint32_t>(chunk.size()),
                           .uncompressed_size = static_cast<std::uint32_t>(records.size())});
  footer.streams.push_back({.id = 2, .records = 20});
  const auto footer_bytes = mr::format::encode_footer(footer, file.size());
  file.insert(file.end(), footer_bytes.begin(), footer_bytes.end());
  write(dir / "file.bin", file);
  return 0;
}
