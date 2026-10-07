#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <mr/record_ring.hpp>

namespace {

using mr::ByteSpan;
using mr::MutableByteSpan;
using mr::OverflowPolicy;
using mr::PushResult;
using mr::RecordMeta;
using mr::RecordRing;
using mr::RingHeader;

// Deterministic payload for sequence number `seq`; size varies with seq so the
// test also covers variable-length slots.
std::size_t payload_size_for(std::uint64_t seq, std::size_t max_payload) {
  return static_cast<std::size_t>((seq * 37) % (max_payload + 1));
}

void fill_payload(std::uint64_t seq, MutableByteSpan out) noexcept {
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>((seq + i * 13) & 0xFFU);
  }
}

bool payload_matches(std::uint64_t seq, ByteSpan in) noexcept {
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] != static_cast<std::byte>((seq + i * 13) & 0xFFU)) {
      return false;
    }
  }
  return true;
}

PushResult push_seq(RecordRing& ring, std::uint64_t seq) {
  RecordMeta meta{.timestamp_ns = static_cast<std::int64_t>(seq) * 1000, .seq = seq, .stream_id = 3};
  return ring.push_with(meta, 0, payload_size_for(seq, ring.max_payload()),
                        [seq](MutableByteSpan out) noexcept { fill_payload(seq, out); });
}

TEST(RecordRing, RoundTripsHeaderAndPayload) {
  RecordRing ring(4, 128, OverflowPolicy::kDropNewest);
  const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
  RecordMeta meta{.timestamp_ns = 123, .seq = 9, .stream_id = 2, .flags = 1};
  ASSERT_EQ(ring.push(meta, 555, payload), PushResult::kOk);

  bool seen = false;
  ASSERT_TRUE(ring.try_pop([&](const RingHeader& h, ByteSpan p) noexcept {
    seen = true;
    EXPECT_EQ(h.meta.timestamp_ns, 123);
    EXPECT_EQ(h.meta.seq, 9U);
    EXPECT_EQ(h.meta.stream_id, 2);
    EXPECT_EQ(h.meta.flags, 1);
    EXPECT_EQ(h.enqueue_ns, 555);
    ASSERT_EQ(p.size(), 3U);
    EXPECT_EQ(std::memcmp(p.data(), payload.data(), 3), 0);
  }));
  EXPECT_TRUE(seen);
  EXPECT_FALSE(ring.try_pop([](const RingHeader&, ByteSpan) noexcept {}));
}

TEST(RecordRing, RejectsOversizedPayload) {
  RecordRing ring(4, 16, OverflowPolicy::kBlock);
  const std::vector<std::byte> big(17);
  EXPECT_EQ(ring.push({}, 0, big), PushResult::kTooLarge);
  EXPECT_EQ(ring.counters().too_large, 1U);
}

TEST(RecordRing, DropNewestKeepsOldest) {
  RecordRing ring(4, 64, OverflowPolicy::kDropNewest);
  for (std::uint64_t i = 0; i < 4; ++i) {
    ASSERT_EQ(push_seq(ring, i), PushResult::kOk);
  }
  EXPECT_EQ(push_seq(ring, 4), PushResult::kDropped);
  EXPECT_EQ(push_seq(ring, 5), PushResult::kDropped);
  EXPECT_EQ(ring.counters().dropped_newest, 2U);

  std::vector<std::uint64_t> seqs;
  while (ring.try_pop([&](const RingHeader& h, ByteSpan) noexcept { seqs.push_back(h.meta.seq); })) {
  }
  EXPECT_EQ(seqs, (std::vector<std::uint64_t>{0, 1, 2, 3}));
}

TEST(RecordRing, DropOldestKeepsNewest) {
  RecordRing ring(4, 64, OverflowPolicy::kDropOldest);
  for (std::uint64_t i = 0; i < 7; ++i) {
    ASSERT_EQ(push_seq(ring, i), PushResult::kOk);
  }
  EXPECT_EQ(ring.counters().evicted_oldest, 3U);

  std::vector<std::uint64_t> seqs;
  while (ring.try_pop([&](const RingHeader& h, ByteSpan p) noexcept {
    EXPECT_TRUE(payload_matches(h.meta.seq, p));
    seqs.push_back(h.meta.seq);
  })) {
  }
  EXPECT_EQ(seqs, (std::vector<std::uint64_t>{3, 4, 5, 6}));
}

TEST(RecordRing, CloseUnblocksBlockedProducer) {
  RecordRing ring(2, 8, OverflowPolicy::kBlock);
  ASSERT_EQ(push_seq(ring, 0), PushResult::kOk);
  ASSERT_EQ(push_seq(ring, 1), PushResult::kOk);
  std::atomic<bool> returned{false};
  PushResult result = PushResult::kOk;
  std::thread producer([&] {
    result = push_seq(ring, 2);
    returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(returned.load()) << "producer should be blocked on a full ring";
  ring.close();
  producer.join();
  EXPECT_EQ(result, PushResult::kStopped);
  EXPECT_GT(ring.counters().blocked_ns, 0U);
  EXPECT_EQ(push_seq(ring, 3), PushResult::kStopped);
}

// Multi-threaded stress for every overflow policy. The consumer is
// deliberately slowed down at intervals so the ring overflows. Invariants:
//   * every record the consumer sees has an intact payload,
//   * sequence numbers are strictly increasing (FIFO, no duplicates),
//   * received + lost == produced (nothing disappears unaccounted).
class RecordRingStress : public ::testing::TestWithParam<OverflowPolicy> {};

TEST_P(RecordRingStress, ConcurrentPushPopAccountsForEveryRecord) {
  constexpr std::uint64_t kCount = 200'000;
  RecordRing ring(64, 200, GetParam());

  std::thread producer([&ring] {
    for (std::uint64_t seq = 0; seq < kCount; ++seq) {
      const PushResult r = push_seq(ring, seq);
      ASSERT_NE(r, PushResult::kStopped);
      ASSERT_NE(r, PushResult::kTooLarge);
    }
  });

  std::uint64_t received = 0;
  std::uint64_t corrupt = 0;
  std::uint64_t out_of_order = 0;
  std::int64_t last_seq = -1;
  std::uint64_t pops = 0;
  auto drain_one = [&]() {
    return ring.try_pop([&](const RingHeader& h, ByteSpan p) noexcept {
      ++received;
      if (!payload_matches(h.meta.seq, p) ||
          p.size() != payload_size_for(h.meta.seq, ring.max_payload())) {
        ++corrupt;
      }
      if (static_cast<std::int64_t>(h.meta.seq) <= last_seq) {
        ++out_of_order;
      }
      last_seq = static_cast<std::int64_t>(h.meta.seq);
    });
  };

  for (;;) {
    if (drain_one()) {
      if (++pops % 4096 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      continue;
    }
    const mr::RingCounters c = ring.counters();
    if (c.pushed + c.dropped_newest == kCount && ring.size_approx() == 0) {
      break;
    }
  }
  producer.join();
  while (drain_one()) {
  }

  const mr::RingCounters c = ring.counters();
  EXPECT_EQ(corrupt, 0U);
  EXPECT_EQ(out_of_order, 0U);
  EXPECT_EQ(received + c.dropped_newest + c.evicted_oldest, kCount);
  if (GetParam() == OverflowPolicy::kBlock) {
    EXPECT_EQ(received, kCount);
  }
}

INSTANTIATE_TEST_SUITE_P(AllPolicies, RecordRingStress,
                         ::testing::Values(OverflowPolicy::kDropNewest, OverflowPolicy::kDropOldest,
                                           OverflowPolicy::kBlock),
                         [](const auto& param_info) {
                           std::string name{mr::to_string(param_info.param)};
                           std::erase(name, '-');
                           return name;
                         });

}  // namespace
