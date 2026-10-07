#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <vector>

#include <mr/events.hpp>
#include <mr/file_util.hpp>
#include <mr/reader.hpp>
#include <mr/recovery.hpp>

#include "test_util.hpp"

namespace {

using mr::test::TempDir;
namespace fmt = mr::format;

std::vector<std::byte> read_file(const std::filesystem::path& p) {
  auto m = mr::MappedFile::open(p);
  EXPECT_TRUE(m);
  return {m->bytes().begin(), m->bytes().end()};
}

void write_file(const std::filesystem::path& p, mr::ByteSpan bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));  // NOLINT
}

std::uint64_t count_records(const mr::Reader& r) {
  auto it = r.messages();
  mr::Message m;
  std::uint64_t n = 0;
  for (;;) {
    auto more = it.next(m);
    EXPECT_TRUE(more);
    if (!more || !more.value()) {
      return n;
    }
    ++n;
  }
}

// Builds a closed file, then exposes its data region (header + chunks) as a
// "crash image" plus the original chunk index for computing expectations.
class RecoveryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto closed = dir_.file("source.mrlog");
    mr::test::write_test_file(closed, 3, 300, 1500);
    auto reader = mr::Reader::open(closed);
    ASSERT_TRUE(reader);
    index_ = reader->footer().chunks;
    auto bytes = read_file(closed);
    auto trailer = fmt::decode_trailer(bytes);
    ASSERT_TRUE(trailer);
    bytes.resize(trailer->footer_offset);  // drop footer + trailer: what a crash leaves
    data_ = std::move(bytes);
    header_size_ = index_.front().offset;
  }

  // Expected (chunks, records) recovered when the file is cut at `size`.
  [[nodiscard]] std::pair<std::size_t, std::uint64_t> expected_for(std::uint64_t size) const {
    std::size_t chunks = 0;
    std::uint64_t records = 0;
    for (const auto& c : index_) {
      if (c.offset + c.total_size <= size) {
        ++chunks;
        records += c.record_count;
      }
    }
    return {chunks, records};
  }

  void check_cut(std::uint64_t cut, bool verify = true) {
    const auto partial = dir_.file("cut.mrlog.partial");
    const auto out = dir_.file("cut.mrlog");
    std::filesystem::remove(out);
    write_file(partial, mr::ByteSpan(data_).first(cut));
    auto report = mr::recover_file(partial, out, {.verify_records = verify, .dry_run = false});
    ASSERT_TRUE(report) << "cut " << cut << ": " << report.error().describe();
    const auto [chunks, records] = expected_for(cut);
    EXPECT_EQ(report->chunks_recovered, chunks) << "cut " << cut;
    EXPECT_EQ(report->records_recovered, records) << "cut " << cut;
    EXPECT_FALSE(std::filesystem::exists(partial));
    auto reader = mr::Reader::open(out);
    ASSERT_TRUE(reader) << "cut " << cut << ": " << reader.error().describe();
    EXPECT_EQ(reader->footer().flags & fmt::footer_flags::kRecovered, fmt::footer_flags::kRecovered);
    EXPECT_EQ(count_records(*reader), records) << "cut " << cut;
  }

  TempDir dir_;
  std::vector<fmt::ChunkIndexEntry> index_;
  std::vector<std::byte> data_;
  std::uint64_t header_size_ = 0;
};

TEST_F(RecoveryTest, CompleteDataRegionRecoversEverything) {
  check_cut(data_.size());
  EXPECT_GT(index_.size(), 10U);
}

TEST_F(RecoveryTest, TruncationAtEveryByteOfTheLastTwoChunks) {
  const auto& last2 = index_[index_.size() - 2];
  for (std::uint64_t cut = last2.offset; cut <= data_.size(); ++cut) {
    check_cut(cut);
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST_F(RecoveryTest, TruncationAroundEveryChunkBoundaryAndAtRandomOffsets) {
  std::set<std::uint64_t> cuts{header_size_};
  for (const auto& c : index_) {
    for (const std::uint64_t d : {0UL, 1UL, 63UL, 64UL, 65UL}) {
      if (c.offset + d <= data_.size()) {
        cuts.insert(c.offset + d);
      }
    }
  }
  std::mt19937_64 rng(99);
  for (int i = 0; i < 100; ++i) {
    cuts.insert(header_size_ + rng() % (data_.size() - header_size_));
  }
  for (const std::uint64_t cut : cuts) {
    check_cut(cut, /*verify=*/(cut % 2) == 0);
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST_F(RecoveryTest, CorruptedMiddleChunkStopsThere) {
  const std::size_t bad = index_.size() / 2;
  // Flip one byte in the body of chunk `bad`.
  data_[index_[bad].offset + fmt::kChunkHeaderSize + 5] ^= std::byte{0x40};
  const auto partial = dir_.file("c.mrlog.partial");
  write_file(partial, data_);
  auto report = mr::recover_file(partial, mr::default_recovery_output(partial));
  ASSERT_TRUE(report);
  EXPECT_EQ(report->chunks_recovered, bad);
  EXPECT_EQ(report->bytes_kept, index_[bad].offset);
  EXPECT_NE(report->stop_reason.find("CRC"), std::string::npos) << report->stop_reason;
  EXPECT_TRUE(mr::Reader::open(dir_.file("c.mrlog")));
}

TEST_F(RecoveryTest, GarbageTailIsDiscarded) {
  std::mt19937_64 rng(5);
  for (int i = 0; i < 1000; ++i) {
    data_.push_back(static_cast<std::byte>(rng() & 0xFFU));
  }
  const auto partial = dir_.file("g.mrlog.partial");
  write_file(partial, data_);
  auto report = mr::recover_file(partial, dir_.file("g.mrlog"));
  ASSERT_TRUE(report);
  EXPECT_EQ(report->chunks_recovered, index_.size());
  EXPECT_EQ(report->bytes_discarded, 1000U);
}

TEST_F(RecoveryTest, ReplayedChunkWithWrongSequenceIsRejected) {
  // Append a copy of chunk 0 at the end: valid CRCs but sequence number 0.
  const auto& c0 = index_.front();
  std::vector<std::byte> copy(data_.begin() + static_cast<std::ptrdiff_t>(c0.offset),
                              data_.begin() + static_cast<std::ptrdiff_t>(c0.offset + c0.total_size));
  data_.insert(data_.end(), copy.begin(), copy.end());
  const auto partial = dir_.file("s.mrlog.partial");
  write_file(partial, data_);
  auto report = mr::recover_file(partial, dir_.file("s.mrlog"));
  ASSERT_TRUE(report);
  EXPECT_EQ(report->chunks_recovered, index_.size());
  EXPECT_NE(report->stop_reason.find("sequence"), std::string::npos);
}

TEST_F(RecoveryTest, DryRunDoesNotModifyInput) {
  const auto partial = dir_.file("d.mrlog.partial");
  write_file(partial, mr::ByteSpan(data_).first(data_.size() - 10));
  auto report = mr::recover_file(partial, dir_.file("d.mrlog"), {.verify_records = true, .dry_run = true});
  ASSERT_TRUE(report);
  EXPECT_EQ(report->chunks_recovered, index_.size() - 1);
  EXPECT_EQ(read_file(partial).size(), data_.size() - 10);
  EXPECT_FALSE(std::filesystem::exists(dir_.file("d.mrlog")));
}

TEST_F(RecoveryTest, CorruptFileHeaderIsAnError) {
  data_[20] ^= std::byte{1};
  const auto partial = dir_.file("h.mrlog.partial");
  write_file(partial, data_);
  auto report = mr::recover_file(partial, dir_.file("h.mrlog"));
  ASSERT_FALSE(report);
  EXPECT_EQ(report.error().code, mr::Errc::kCorrupt);
}

TEST(Recovery, AlreadyCompleteFileIsOnlyRenamed) {
  TempDir dir;
  const auto closed = dir.file("done.mrlog");
  mr::test::write_test_file(closed, 2, 50);
  const auto renamed = dir.file("done2.mrlog");
  auto report = mr::recover_file(closed, renamed);
  ASSERT_TRUE(report);
  EXPECT_TRUE(report->already_complete);
  auto reader = mr::Reader::open(renamed);
  ASSERT_TRUE(reader);
  EXPECT_EQ(reader->footer().flags & fmt::footer_flags::kRecovered, 0);
}

TEST(Recovery, QualityCountersAreRebuiltFromEventStream) {
  TempDir dir;
  const auto path = dir.file("ev.mrlog");
  {
    mr::WriterOptions opts;
    opts.path = path;
    opts.streams = mr::test::test_streams(2);
    opts.streams.push_back({.id = mr::kEventStreamId, .kind = mr::SensorKind::kEvents,
                            .nominal_rate_hz = 0.0, .name = "_events"});
    opts.chunk_target_bytes = 1;  // every record seals its own chunk, so all reach the disk
    auto w = mr::StorageWriter::open(opts);
    ASSERT_TRUE(w);
    const std::vector<mr::QualityEvent> events{
        {.stream = 1, .kind = mr::EventKind::kSeqGap, .timestamp_ns = 10, .value = 3},
        {.stream = 1, .kind = mr::EventKind::kSeqGap, .timestamp_ns = 20, .value = 1},
        {.stream = 0, .kind = mr::EventKind::kTimestampRegression, .timestamp_ns = 30, .value = 5},
        {.stream = 0, .kind = mr::EventKind::kRecordsDropped, .timestamp_ns = 40, .value = 17},
        {.stream = 1, .kind = mr::EventKind::kStreamStale, .timestamp_ns = 50, .value = 1000},
    };
    std::uint64_t seq = 0;
    for (const auto& e : events) {
      std::array<std::byte, mr::kEventPayloadSize> buf{};
      mr::encode_event(e, buf);
      ASSERT_TRUE((*w)->append({.timestamp_ns = e.timestamp_ns, .seq = seq++, .stream_id = mr::kEventStreamId}, buf, 0));
    }
    // Destroyed without close(): crash. Sealed chunks are still written.
  }
  auto report = mr::recover_file(mr::partial_path(path), path);
  ASSERT_TRUE(report) << report.error().describe();
  auto reader = mr::Reader::open(path);
  ASSERT_TRUE(reader);
  const auto* s0 = reader->stream_stats(0);
  const auto* s1 = reader->stream_stats(1);
  ASSERT_NE(s0, nullptr);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s0->ts_regressions, 1U);
  EXPECT_EQ(s0->dropped, 17U);
  EXPECT_EQ(s1->seq_gaps, 2U);
  EXPECT_EQ(s1->stale_events, 1U);
  EXPECT_EQ(reader->stream_stats(mr::kEventStreamId)->records, 5U);
}

TEST(Recovery, DefaultOutputStripsPartialSuffix) {
  EXPECT_EQ(mr::default_recovery_output("/a/b.mrlog.partial"), std::filesystem::path("/a/b.mrlog"));
  EXPECT_EQ(mr::default_recovery_output("/a/b.mrlog"), std::filesystem::path("/a/b.mrlog"));
}

}  // namespace
