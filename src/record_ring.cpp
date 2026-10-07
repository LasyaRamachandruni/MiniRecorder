#include <mr/record_ring.hpp>

#include <chrono>

namespace mr {
namespace {

std::size_t slot_stride(std::size_t max_payload) noexcept {
  const std::size_t raw = RecordRing::kSlotHeaderBytes + max_payload;
  return (raw + kCacheLineSize - 1) / kCacheLineSize * kCacheLineSize;
}

std::int64_t steady_now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::string_view to_string(OverflowPolicy policy) noexcept {
  switch (policy) {
    case OverflowPolicy::kDropNewest:
      return "drop-newest";
    case OverflowPolicy::kDropOldest:
      return "drop-oldest";
    case OverflowPolicy::kBlock:
      return "block";
  }
  return "unknown";
}

std::optional<OverflowPolicy> parse_overflow_policy(std::string_view text) noexcept {
  if (text == "drop-newest") {
    return OverflowPolicy::kDropNewest;
  }
  if (text == "drop-oldest") {
    return OverflowPolicy::kDropOldest;
  }
  if (text == "block") {
    return OverflowPolicy::kBlock;
  }
  return std::nullopt;
}

RecordRing::RecordRing(std::size_t capacity, std::size_t max_payload, OverflowPolicy policy)
    : capacity_(round_up_pow2(capacity)),
      mask_(capacity_ - 1),
      max_payload_(max_payload),
      stride_(slot_stride(max_payload)),
      policy_(policy),
      slots_(std::make_unique<std::byte[]>(capacity_ * stride_)) {}

PushResult RecordRing::evict_oldest(std::uint64_t head) {
  detail::Backoff backoff;
  for (;;) {
    std::uint64_t tail = tail_.load(std::memory_order_acquire);
    if ((tail & kLockBit) == 0) {
      if (head - tail < capacity_) {
        // The consumer freed space while we were looking.
        producer_.cached_tail = tail;
        return PushResult::kOk;
      }
      if (tail_.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
        producer_.cached_tail = tail + 1;
        producer_.evicted_oldest.add();
        return PushResult::kOk;
      }
      continue;
    }
    // The consumer is copying the oldest record out right now. Its release
    // frees a slot, so this wait is bounded by one record copy.
    if (closed()) {
      return PushResult::kStopped;
    }
    backoff.pause();
  }
}

PushResult RecordRing::wait_for_space(std::uint64_t head) {
  const std::int64_t start = steady_now_ns();
  detail::Backoff backoff;
  PushResult result = PushResult::kOk;
  for (;;) {
    if (closed()) {
      result = PushResult::kStopped;
      break;
    }
    backoff.pause();
    producer_.cached_tail = tail_.load(std::memory_order_acquire) & kIndexMask;
    if (head - producer_.cached_tail < capacity_) {
      break;
    }
  }
  producer_.blocked_ns.add(static_cast<std::uint64_t>(steady_now_ns() - start));
  return result;
}

RingCounters RecordRing::counters() const noexcept {
  RingCounters c;
  c.pushed = producer_.pushed.get();
  c.dropped_newest = producer_.dropped_newest.get();
  c.evicted_oldest = producer_.evicted_oldest.get();
  c.too_large = producer_.too_large.get();
  c.blocked_ns = producer_.blocked_ns.get();
  return c;
}

std::size_t RecordRing::size_approx() const noexcept {
  const std::uint64_t tail = tail_.load(std::memory_order_acquire) & kIndexMask;
  const std::uint64_t head = head_.load(std::memory_order_acquire);
  return head > tail ? static_cast<std::size_t>(head - tail) : 0;
}

}  // namespace mr
