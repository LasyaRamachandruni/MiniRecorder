#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>

#include <mr/chunk.hpp>
#include <mr/file_util.hpp>
#include <mr/format.hpp>
#include <mr/writer.hpp>

#include "test_util.hpp"

namespace {

using mr::test::TempDir;
namespace fmt = mr::format;

TEST(FsyncPolicy, Parse) {
  EXPECT_EQ(mr::parse_fsync_policy("chunk")->mode, mr::FsyncMode::kEveryChunk);
  EXPECT_EQ(mr::parse_fsync_policy("close")->mode, mr::FsyncMode::kOnClose);
  auto interval = mr::parse_fsync_policy("interval:250");
  ASSERT_TRUE(interval);
  EXPECT_EQ(interval->mode, mr::FsyncMode::kInterval);
  EXPECT_EQ(interval->interval.count(), 250);
  EXPECT_FALSE(mr::parse_fsync_policy("interval:"));
  EXPECT_FALSE(mr::parse_fsync_policy("interval:-5"));
  EXPECT_FALSE(mr::parse_fsync_policy("sometimes"));
}

TEST(StorageWriter, CloseRenamesPartialAndWritesValidFooter) {
  TempDir dir;
  const auto path = dir.file("out.mrlog");
  const auto written = mr::test::write_test_file(path, 3, 500);
  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_FALSE(std::filesystem::exists(mr::partial_path(path)));

  auto file = mr::MappedFile::open(path);
  ASSERT_TRUE(file);
  auto footer = fmt::read_footer(file->bytes());
  ASSERT_TRUE(footer) << footer.error().describe();
  EXPECT_GT(footer->chunks.size(), 3U);
  ASSERT_EQ(footer->streams.size(), 3U);

  std::uint64_t total_records = 0;
  for (const auto& s : footer->streams) {
    EXPECT_EQ(s.records, 500U);
    total_records += s.records;
  }
  EXPECT_EQ(total_records, written.size());
  EXPECT_EQ(footer->streams[2].first_time_ns, 1'000'000'000);
  EXPECT_EQ(footer->streams[2].last_time_ns, 1'000'000'000 + 499LL * 3 * 1'000'000);

  // Chunks are contiguous, in order, and each one verifies.
  mr::ZstdDecompressor dec;
  std::vector<std::byte> body;
  auto header = fmt::decode_file_header(file->bytes());
  ASSERT_TRUE(header);
  std::uint64_t expected_offset = header->encoded_size;
  std::uint64_t seq = 0;
  for (const auto& c : footer->chunks) {
    EXPECT_EQ(c.offset, expected_offset);
    auto h = mr::parse_and_verify_chunk(file->bytes().subspan(c.offset, c.total_size), dec, body);
    ASSERT_TRUE(h) << h.error().describe();
    EXPECT_EQ(h->chunk_seq, seq++);
    EXPECT_EQ(std::popcount(h->stream_mask), 1) << "writer produces single-stream chunks";
    expected_offset += c.total_size;
  }
}

TEST(StorageWriter, RejectsUndeclaredStreamAndOversizedPayload) {
  TempDir dir;
  mr::WriterOptions opts;
  opts.path = dir.file("x.mrlog");
  opts.streams = mr::test::test_streams(1);
  auto writer = mr::StorageWriter::open(opts);
  ASSERT_TRUE(writer);
  const std::vector<std::byte> small(8);
  EXPECT_EQ((*writer)->append({.stream_id = 5}, small, 0).error().code, mr::Errc::kInvalidArgument);
  const std::vector<std::byte> huge(fmt::kMaxRecordPayload + 1);
  EXPECT_EQ((*writer)->append({.stream_id = 0}, huge, 0).error().code, mr::Errc::kInvalidArgument);
  EXPECT_TRUE((*writer)->close());
}

TEST(StorageWriter, DestructorWithoutCloseLeavesPartialFile) {
  TempDir dir;
  const auto path = dir.file("crash.mrlog");
  {
    mr::WriterOptions opts;
    opts.path = path;
    opts.streams = mr::test::test_streams(1);
    opts.chunk_target_bytes = 1024;
    auto writer = mr::StorageWriter::open(opts);
    ASSERT_TRUE(writer);
    for (std::uint64_t i = 0; i < 200; ++i) {
      const auto p = mr::test::test_payload(0, i, 64);
      ASSERT_TRUE((*writer)->append({.timestamp_ns = static_cast<std::int64_t>(i), .seq = i}, p, 0));
    }
  }
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(std::filesystem::exists(mr::partial_path(path)));
  auto file = mr::MappedFile::open(mr::partial_path(path));
  ASSERT_TRUE(file);
  EXPECT_FALSE(fmt::read_footer(file->bytes())) << "no footer expected without close()";
}

TEST(StorageWriter, PerChunkFsyncReportsDurableProgressAndLatency) {
  TempDir dir;
  std::atomic<std::uint64_t> durable{0};
  mr::WriterOptions opts;
  opts.path = dir.file("d.mrlog");
  opts.streams = mr::test::test_streams(1);
  opts.chunk_target_bytes = 2048;
  opts.fsync = {mr::FsyncMode::kEveryChunk, std::chrono::milliseconds(0)};
  opts.track_latency = true;
  opts.on_durable = [&durable](std::uint64_t n) { durable.store(n); };
  auto writer = mr::StorageWriter::open(opts);
  ASSERT_TRUE(writer);
  for (std::uint64_t i = 0; i < 300; ++i) {
    const auto p = mr::test::test_payload(0, i, 100);
    ASSERT_TRUE((*writer)->append({.timestamp_ns = static_cast<std::int64_t>(i), .seq = i}, p, 1));
  }
  auto summary = (*writer)->close();
  ASSERT_TRUE(summary) << summary.error().describe();
  EXPECT_EQ(durable.load(), summary->metrics.chunks_written);
  EXPECT_GE(summary->metrics.fsync_count, summary->metrics.chunks_written);
  EXPECT_EQ(summary->metrics.durable_latency_ns.size(), 300U);
  EXPECT_EQ(summary->metrics.write_latency_ns.size(), 300U);
}

TEST(StorageWriter, SealAgedFlushesSlowStreams) {
  TempDir dir;
  mr::WriterOptions opts;
  opts.path = dir.file("aged.mrlog");
  opts.streams = mr::test::test_streams(1);
  opts.chunk_max_age = std::chrono::milliseconds(1);
  auto writer = mr::StorageWriter::open(opts);
  ASSERT_TRUE(writer);
  const auto p = mr::test::test_payload(0, 0, 10);
  ASSERT_TRUE((*writer)->append({.timestamp_ns = 1}, p, 0));
  ASSERT_TRUE((*writer)->seal_aged(std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
  ASSERT_TRUE((*writer)->append({.timestamp_ns = 2, .seq = 1}, p, 0));
  auto summary = (*writer)->close();
  ASSERT_TRUE(summary);
  EXPECT_EQ(summary->footer.chunks.size(), 2U);
}

}  // namespace
