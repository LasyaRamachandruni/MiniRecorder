#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <tuple>
#include <vector>

#include <mr/reader.hpp>

#include "test_util.hpp"

namespace {

using mr::test::TempDir;
using mr::test::WrittenRecord;

std::vector<mr::Message> read_all(const mr::Reader& reader, const mr::ReadOptions& opts,
                                  std::vector<std::vector<std::byte>>* payloads = nullptr) {
  std::vector<mr::Message> out;
  auto it = reader.messages(opts);
  mr::Message m;
  for (;;) {
    auto more = it.next(m);
    EXPECT_TRUE(more) << more.error().describe();
    if (!more || !more.value()) {
      break;
    }
    out.push_back(m);
    if (payloads != nullptr) {
      payloads->emplace_back(m.payload.begin(), m.payload.end());
    }
  }
  return out;
}

// Reference ordering the merge must reproduce.
std::vector<WrittenRecord> expected_order(std::vector<WrittenRecord> recs, const mr::ReadOptions& opts) {
  std::erase_if(recs, [&](const WrittenRecord& r) {
    return (opts.stream_mask & (std::uint64_t{1} << r.meta.stream_id)) == 0 ||
           r.meta.timestamp_ns < opts.start_ns || r.meta.timestamp_ns > opts.end_ns;
  });
  std::stable_sort(recs.begin(), recs.end(), [](const WrittenRecord& a, const WrittenRecord& b) {
    return std::tie(a.meta.timestamp_ns, a.meta.stream_id) < std::tie(b.meta.timestamp_ns, b.meta.stream_id);
  });
  return recs;
}

class ReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    written_ = mr::test::write_test_file(dir_.file("r.mrlog"), 4, 800, 2048);
    auto r = mr::Reader::open(dir_.file("r.mrlog"));
    ASSERT_TRUE(r) << r.error().describe();
    reader_.emplace(std::move(r).value());
  }

  TempDir dir_;
  std::vector<WrittenRecord> written_;
  std::optional<mr::Reader> reader_;
};

TEST_F(ReaderTest, MergedIterationIsTimestampOrderedAndComplete) {
  std::vector<std::vector<std::byte>> payloads;
  const auto msgs = read_all(*reader_, {}, &payloads);
  const auto expected = expected_order(written_, {});
  ASSERT_EQ(msgs.size(), expected.size());
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    ASSERT_EQ(msgs[i].meta.timestamp_ns, expected[i].meta.timestamp_ns) << i;
    ASSERT_EQ(msgs[i].meta.stream_id, expected[i].meta.stream_id) << i;
    ASSERT_EQ(msgs[i].meta.seq, expected[i].meta.seq) << i;
    EXPECT_EQ(payloads[i], mr::test::test_payload(expected[i].meta.stream_id, expected[i].meta.seq,
                                                  expected[i].payload_size));
  }
}

TEST_F(ReaderTest, StreamFilter) {
  mr::ReadOptions opts;
  opts.stream_mask = (1U << 1) | (1U << 3);
  const auto msgs = read_all(*reader_, opts);
  EXPECT_EQ(msgs.size(), 1600U);
  for (const auto& m : msgs) {
    EXPECT_TRUE(m.meta.stream_id == 1 || m.meta.stream_id == 3);
  }
}

TEST_F(ReaderTest, SeekMatchesBruteForceForRandomRanges) {
  std::mt19937_64 rng(1234);
  const std::int64_t t0 = reader_->footer().start_time_ns;
  const std::int64_t t1 = reader_->footer().end_time_ns;
  for (int round = 0; round < 60; ++round) {
    mr::ReadOptions opts;
    const auto span = static_cast<std::uint64_t>(t1 - t0 + 2);
    opts.start_ns = t0 - 1 + static_cast<std::int64_t>(rng() % span);
    opts.end_ns = opts.start_ns + static_cast<std::int64_t>(rng() % 500'000'000);
    opts.stream_mask = (rng() % 15) + 1;
    const auto msgs = read_all(*reader_, opts);
    const auto expected = expected_order(written_, opts);
    ASSERT_EQ(msgs.size(), expected.size()) << "round " << round;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
      ASSERT_EQ(msgs[i].meta.timestamp_ns, expected[i].meta.timestamp_ns);
      ASSERT_EQ(msgs[i].meta.stream_id, expected[i].meta.stream_id);
    }
  }
}

TEST_F(ReaderTest, SeekChunkSkipsOnlyOlderChunks) {
  for (mr::StreamId s = 0; s < 4; ++s) {
    const auto chunks = reader_->chunks_for_stream(s);
    ASSERT_FALSE(chunks.empty());
    for (std::size_t k = 0; k < chunks.size(); ++k) {
      const auto& entry = reader_->footer().chunks[chunks[k]];
      // Seeking to a chunk's first timestamp lands on that chunk (or earlier).
      EXPECT_LE(reader_->seek_chunk(s, entry.min_time_ns), k);
      // Every chunk skipped ends before the seek time.
      const std::size_t pos = reader_->seek_chunk(s, entry.max_time_ns);
      for (std::size_t j = 0; j < pos; ++j) {
        EXPECT_LT(reader_->footer().chunks[chunks[j]].max_time_ns, entry.max_time_ns);
      }
    }
    EXPECT_EQ(reader_->seek_chunk(s, std::numeric_limits<std::int64_t>::max()), chunks.size());
    EXPECT_EQ(reader_->seek_chunk(s, std::numeric_limits<std::int64_t>::min()), 0U);
  }
}

TEST(Reader, RejectsPartialFileWithHint) {
  TempDir dir;
  const auto path = dir.file("p.mrlog");
  {
    mr::WriterOptions opts;
    opts.path = path;
    opts.streams = mr::test::test_streams(1);
    auto w = mr::StorageWriter::open(opts);
    ASSERT_TRUE(w);
    const auto p = mr::test::test_payload(0, 0, 10);
    ASSERT_TRUE((*w)->append({.timestamp_ns = 5}, p, 0));
  }
  auto r = mr::Reader::open(mr::partial_path(path));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, mr::Errc::kNotFound);
  EXPECT_NE(r.error().message.find("mr_recover"), std::string::npos);
}

TEST(Reader, OverlappingChunkRangesStillSeekCorrectly) {
  // A timestamp regression makes chunk time ranges of one stream overlap.
  TempDir dir;
  const auto path = dir.file("o.mrlog");
  mr::WriterOptions opts;
  opts.path = path;
  opts.streams = mr::test::test_streams(1);
  opts.chunk_target_bytes = 256;
  auto w = mr::StorageWriter::open(opts);
  ASSERT_TRUE(w);
  std::vector<std::int64_t> ts;
  for (int i = 0; i < 200; ++i) {
    std::int64_t t = i * 100;
    if (i % 37 == 36) {
      t -= 2500;  // jumps back across a chunk boundary
    }
    ts.push_back(t);
    const auto p = mr::test::test_payload(0, static_cast<std::uint64_t>(i), 32);
    ASSERT_TRUE((*w)->append({.timestamp_ns = t, .seq = static_cast<std::uint64_t>(i)}, p, 0));
  }
  ASSERT_TRUE((*w)->close());
  auto r = mr::Reader::open(path);
  ASSERT_TRUE(r);
  for (const std::int64_t start : {0L, 1000L, 3550L, 9000L, 15000L, 19900L}) {
    mr::ReadOptions ro;
    ro.start_ns = start;
    const auto msgs = read_all(*r, ro);
    const auto want = std::count_if(ts.begin(), ts.end(), [start](std::int64_t t) { return t >= start; });
    EXPECT_EQ(static_cast<std::ptrdiff_t>(msgs.size()), want) << "start " << start;
  }
}

}  // namespace
