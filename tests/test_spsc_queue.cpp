#include <cstdint>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include <mr/spsc_queue.hpp>

namespace {

TEST(SpscQueue, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ(mr::SpscQueue<int>(0).capacity(), 2U);
  EXPECT_EQ(mr::SpscQueue<int>(5).capacity(), 8U);
  EXPECT_EQ(mr::SpscQueue<int>(64).capacity(), 64U);
}

TEST(SpscQueue, FifoAndFullEmpty) {
  mr::SpscQueue<int> q(4);
  int out = 0;
  EXPECT_FALSE(q.try_pop(out));
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(q.try_push(i));
  }
  EXPECT_FALSE(q.try_push(99)) << "queue should be full";
  EXPECT_EQ(q.size_approx(), 4U);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueue, WrapsAroundManyTimes) {
  mr::SpscQueue<std::uint64_t> q(8);
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(q.try_push(next_in++));
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(q.try_pop(v));
      ASSERT_EQ(v, next_out++);
    }
  }
}

TEST(SpscQueue, MoveOnlyPayload) {
  mr::SpscQueue<std::unique_ptr<int>> q(2);
  ASSERT_TRUE(q.try_push(std::make_unique<int>(7)));
  std::unique_ptr<int> out;
  ASSERT_TRUE(q.try_pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 7);
}

// Two threads, small queue so it is constantly full/empty. The consumer checks
// that it sees every value exactly once and in order.
TEST(SpscQueueStress, ProducerConsumerPreservesOrder) {
  constexpr std::uint64_t kCount = 2'000'000;
  mr::SpscQueue<std::uint64_t> q(64);
  std::thread producer([&q] {
    for (std::uint64_t i = 0; i < kCount;) {
      if (q.try_push(i)) {
        ++i;
      }
    }
  });
  std::uint64_t expected = 0;
  std::uint64_t value = 0;
  bool in_order = true;
  while (expected < kCount) {
    if (q.try_pop(value)) {
      in_order = in_order && (value == expected);
      ++expected;
    }
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(q.size_approx(), 0U);
}

}  // namespace
