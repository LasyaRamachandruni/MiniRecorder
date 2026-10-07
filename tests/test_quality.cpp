#include <gtest/gtest.h>

#include <vector>

#include <mr/quality.hpp>

namespace {

using mr::EventKind;
using mr::QualityEvent;

constexpr std::int64_t kMs = 1'000'000;

class QualityTest : public ::testing::Test {
 protected:
  QualityTest() : monitor_(config(), streams_) {}

  static mr::QualityConfig config() {
    mr::QualityConfig c;
    c.rate_tolerance = 0.2;
    c.rate_window = std::chrono::milliseconds(1000);
    c.stale_periods = 10;
    c.stale_min = std::chrono::milliseconds(100);
    return c;
  }

  // Feeds `n` records at `hz` starting at seq/ts; arrival == timestamp.
  void feed(mr::StreamId id, std::uint64_t& seq, std::int64_t& ts, int n, double hz) {
    for (int i = 0; i < n; ++i) {
      monitor_.observe({.timestamp_ns = ts, .seq = seq++, .stream_id = id}, ts, events_);
      ts += static_cast<std::int64_t>(1e9 / hz);
    }
  }

  std::vector<QualityEvent> of_kind(EventKind k) const {
    std::vector<QualityEvent> out;
    for (const auto& e : events_) {
      if (e.kind == k) {
        out.push_back(e);
      }
    }
    return out;
  }

  std::vector<mr::MonitoredStream> streams_{{.id = 0, .nominal_rate_hz = 100.0},
                                            {.id = 1, .nominal_rate_hz = 10.0}};
  mr::QualityMonitor monitor_;
  std::vector<QualityEvent> events_;
};

TEST_F(QualityTest, CleanStreamProducesNoEvents) {
  std::uint64_t seq = 0;
  std::int64_t ts = 0;
  feed(0, seq, ts, 1000, 100.0);
  EXPECT_TRUE(events_.empty());
}

TEST_F(QualityTest, DetectsSequenceGap) {
  std::uint64_t seq = 0;
  std::int64_t ts = 0;
  feed(0, seq, ts, 10, 100.0);
  seq += 3;
  feed(0, seq, ts, 10, 100.0);
  const auto gaps = of_kind(EventKind::kSeqGap);
  ASSERT_EQ(gaps.size(), 1U);
  EXPECT_EQ(gaps[0].value, 3);
  EXPECT_EQ(gaps[0].stream, 0);
}

TEST_F(QualityTest, DetectsTimestampRegression) {
  std::uint64_t seq = 0;
  std::int64_t ts = 0;
  feed(0, seq, ts, 10, 100.0);
  ts -= 25 * kMs;
  feed(0, seq, ts, 1, 100.0);
  const auto regressions = of_kind(EventKind::kTimestampRegression);
  ASSERT_EQ(regressions.size(), 1U);
  EXPECT_EQ(regressions[0].value, 15 * kMs);  // last was 90 ms, regressed one is 75 ms
}

TEST_F(QualityTest, RateDeviationReportedOnTransitionsOnly) {
  std::uint64_t seq = 0;
  std::int64_t ts = 0;
  feed(0, seq, ts, 300, 100.0);  // 3 s nominal
  feed(0, seq, ts, 150, 50.0);   // 3 s at half rate
  feed(0, seq, ts, 300, 100.0);  // back to nominal
  const auto bad = of_kind(EventKind::kRateOutOfBounds);
  const auto good = of_kind(EventKind::kRateRecovered);
  ASSERT_EQ(bad.size(), 1U);
  ASSERT_EQ(good.size(), 1U);
  EXPECT_NEAR(static_cast<double>(bad[0].value) / 1000.0, 50.0, 2.0);
  EXPECT_EQ(bad[0].expected, 100'000);
  EXPECT_LT(bad[0].timestamp_ns, good[0].timestamp_ns);
}

TEST_F(QualityTest, StaleStreamReportedOnceThenResumed) {
  std::uint64_t seq = 0;
  std::int64_t ts = 0;
  feed(1, seq, ts, 5, 10.0);  // last arrival at 400 ms; stale threshold = 10 periods = 1 s
  monitor_.check_stale(1200 * kMs, events_);
  EXPECT_TRUE(of_kind(EventKind::kStreamStale).empty());
  monitor_.check_stale(1500 * kMs, events_);
  monitor_.check_stale(1900 * kMs, events_);
  ASSERT_EQ(of_kind(EventKind::kStreamStale).size(), 1U);
  monitor_.observe({.timestamp_ns = 2000 * kMs, .seq = seq++, .stream_id = 1}, 2000 * kMs, events_);
  const auto resumed = of_kind(EventKind::kStreamResumed);
  ASSERT_EQ(resumed.size(), 1U);
  EXPECT_EQ(resumed[0].value, 1600 * kMs);
}

TEST_F(QualityTest, UnmonitoredStreamIgnored) {
  monitor_.observe({.timestamp_ns = 10, .seq = 0, .stream_id = 7}, 10, events_);
  monitor_.observe({.timestamp_ns = 5, .seq = 9, .stream_id = 7}, 10, events_);
  EXPECT_TRUE(events_.empty());
}

TEST(EventCodec, RoundTripAndAccumulate) {
  const QualityEvent e{.stream = 4, .kind = EventKind::kRecordsDropped, .timestamp_ns = -5, .value = 12, .expected = 7};
  std::array<std::byte, mr::kEventPayloadSize> buf{};
  mr::encode_event(e, buf);
  const auto d = mr::decode_event(buf);
  ASSERT_TRUE(d);
  EXPECT_EQ(d->stream, 4);
  EXPECT_EQ(d->kind, EventKind::kRecordsDropped);
  EXPECT_EQ(d->timestamp_ns, -5);
  EXPECT_EQ(d->value, 12);
  std::array<mr::StreamQualityCounters, mr::kMaxStreams> counters{};
  mr::accumulate_event(*d, counters);
  mr::accumulate_event(*d, counters);
  EXPECT_EQ(counters[4].dropped, 24U);
  EXPECT_FALSE(mr::decode_event(mr::ByteSpan(buf).first(10)));
  buf[2] = std::byte{99};
  EXPECT_FALSE(mr::decode_event(buf));
}

}  // namespace
