// Codec micro-benchmarks: CRC32C (slice-by-8 vs SSE4.2) and zstd chunk
// encoding of each sensor type at several levels, reporting the compression
// ratio as a counter. Payloads come from the simulator's generators
// (SYNTHETIC data; camera frames are smooth noise plus sensor noise).

#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include <mr/chunk.hpp>
#include <mr/crc32c.hpp>
#include <mr/format.hpp>
#include <mr/sensors.hpp>

namespace {

std::vector<std::byte> random_bytes(std::size_t n) {
  std::vector<std::byte> v(n);
  std::uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (auto& b : v) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    b = static_cast<std::byte>(x & 0xFFU);
  }
  return v;
}

void BM_Crc32c_Portable(benchmark::State& state) {
  const auto data = random_bytes(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(mr::crc32c::extend_portable(0, data));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void BM_Crc32c_Hardware(benchmark::State& state) {
  if (!mr::crc32c::hw_available()) {
    state.SkipWithError("SSE4.2 not available");
    return;
  }
  const auto data = random_bytes(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    benchmark::DoNotOptimize(mr::crc32c::extend_hw(0, data));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// Builds ~1 MiB of encoded records for one sensor kind (one chunk's worth).
std::vector<std::byte> chunk_body(mr::SensorKind kind, mr::format::ChunkHeader& meta) {
  const mr::VehicleProfile profile;
  std::unique_ptr<mr::SignalGenerator> gen;
  double hz = 100;
  switch (kind) {
    case mr::SensorKind::kImu:
      gen = mr::make_imu_generator(profile, 1);
      hz = 200;
      break;
    case mr::SensorKind::kGps:
      gen = mr::make_gps_generator(profile, 2);
      hz = 10;
      break;
    case mr::SensorKind::kWheelCan:
      gen = mr::make_wheel_generator(profile, 3);
      hz = 100;
      break;
    default:
      gen = mr::make_camera_generator(640, 480, 4);
      hz = 30;
      break;
  }
  std::vector<std::byte> records;
  std::vector<std::byte> payload(gen->payload_size());
  for (std::uint64_t i = 0; records.size() < (1U << 20); ++i) {
    const double t = static_cast<double>(i) / hz;
    gen->generate(t, i, payload);
    const mr::RecordMeta m{.timestamp_ns = static_cast<std::int64_t>(t * 1e9), .seq = i};
    mr::format::append_record(records, m, payload);
    ++meta.record_count;
    meta.stream_mask = 1;
    meta.min_time_ns = std::min(meta.min_time_ns, m.timestamp_ns);
    meta.max_time_ns = std::max(meta.max_time_ns, m.timestamp_ns);
  }
  return records;
}

void BM_EncodeChunk(benchmark::State& state) {
  const auto kind = static_cast<mr::SensorKind>(state.range(0));
  const int level = static_cast<int>(state.range(1));
  mr::format::ChunkHeader meta;
  const auto records = chunk_body(kind, meta);
  mr::ZstdCompressor compressor(level);
  std::vector<std::byte> scratch;
  std::vector<std::byte> out;
  std::size_t encoded = 0;
  for (auto _ : state) {
    auto h = mr::encode_chunk(meta, records, compressor, scratch, out);
    if (!h) {
      state.SkipWithError(h.error().message.c_str());
      return;
    }
    encoded = out.size();
    benchmark::DoNotOptimize(out.data());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(records.size()));
  state.counters["ratio"] = static_cast<double>(records.size()) / static_cast<double>(encoded);
  state.SetLabel(std::string(mr::to_string(kind)));
}

void BM_DecodeChunk(benchmark::State& state) {
  const auto kind = static_cast<mr::SensorKind>(state.range(0));
  mr::format::ChunkHeader meta;
  const auto records = chunk_body(kind, meta);
  mr::ZstdCompressor compressor(3);
  mr::ZstdDecompressor decompressor;
  std::vector<std::byte> scratch;
  std::vector<std::byte> chunk;
  if (!mr::encode_chunk(meta, records, compressor, scratch, chunk)) {
    state.SkipWithError("encode failed");
    return;
  }
  std::vector<std::byte> body;
  for (auto _ : state) {
    auto h = mr::parse_and_verify_chunk(chunk, decompressor, body);
    benchmark::DoNotOptimize(h.ok());
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(records.size()));
  state.SetLabel(std::string(mr::to_string(kind)));
}

constexpr auto kImu = static_cast<std::int64_t>(mr::SensorKind::kImu);
constexpr auto kGps = static_cast<std::int64_t>(mr::SensorKind::kGps);
constexpr auto kWheel = static_cast<std::int64_t>(mr::SensorKind::kWheelCan);
constexpr auto kCamera = static_cast<std::int64_t>(mr::SensorKind::kCamera);

}  // namespace

BENCHMARK(BM_Crc32c_Portable)->Arg(4096)->Arg(1 << 20);
BENCHMARK(BM_Crc32c_Hardware)->Arg(4096)->Arg(1 << 20);
BENCHMARK(BM_EncodeChunk)
    ->ArgsProduct({{kImu, kGps, kWheel, kCamera}, {1, 3, 9}})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_DecodeChunk)->Args({kImu})->Args({kCamera})->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
