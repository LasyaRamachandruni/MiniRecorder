#pragma once

// RecordRing: bounded SPSC ring of fixed-size byte slots for sensor records.
//
// One ring per producer thread. All slot memory is allocated once in the
// constructor (capacity * stride bytes); pushing and popping never allocate.
// Each slot holds a small in-memory header (RingHeader) followed by up to
// max_payload bytes of payload, which the producer can fill in place.
//
// Index protocol and memory ordering are the same as SpscQueue (see
// spsc_queue.hpp). The one addition is the drop-oldest overflow policy, in
// which the *producer* advances tail_ to discard the oldest record. That makes
// tail_ a two-writer variable, so in that mode:
//
//   * The consumer claims the oldest slot before reading it by CAS-ing tail_
//     from t to (t | kLockBit). The producer never evicts a locked slot.
//   * The producer evicts by CAS-ing tail_ from an *unlocked* t to t + 1. If
//     the consumer already claimed t, the CAS fails and the producer waits for
//     the (short) read to finish, which itself frees a slot.
//   * The consumer releases with tail_.store(t + 1, release), which clears the
//     lock bit.
//
// The consumer is expected to copy the record out and return quickly; the
// visitor passed to try_pop() must be noexcept so a slot can never stay
// locked because of an exception.

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>

#include <mr/bytes.hpp>
#include <mr/record.hpp>
#include <mr/spsc_queue.hpp>

namespace mr {

enum class OverflowPolicy : std::uint8_t {
  kDropNewest,  // reject the incoming record (cheapest, keeps history)
  kDropOldest,  // evict the oldest queued record (keeps the freshest data)
  kBlock,       // producer waits for space (lossless, may stall the sensor)
};

[[nodiscard]] std::string_view to_string(OverflowPolicy policy) noexcept;
[[nodiscard]] std::optional<OverflowPolicy> parse_overflow_policy(std::string_view text) noexcept;

enum class PushResult : std::uint8_t {
  kOk,        // stored (under drop-oldest an older record may have been evicted)
  kDropped,   // rejected because the ring was full (drop-newest)
  kStopped,   // ring was closed
  kTooLarge,  // payload exceeds the slot size
};

// In-memory slot header. Not persisted; the storage format is defined in
// format.hpp.
struct RingHeader {
  RecordMeta meta;
  std::int64_t enqueue_ns = 0;  // steady clock when the producer pushed it
  std::uint32_t payload_size = 0;
};
static_assert(std::is_trivially_copyable_v<RingHeader>);

struct RingCounters {
  std::uint64_t pushed = 0;
  std::uint64_t dropped_newest = 0;
  std::uint64_t evicted_oldest = 0;
  std::uint64_t too_large = 0;
  std::uint64_t blocked_ns = 0;

  [[nodiscard]] std::uint64_t lost() const noexcept {
    return dropped_newest + evicted_oldest + too_large;
  }
};

namespace detail {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

// Spin briefly, then yield, then sleep. Used only on the slow (full) path.
class Backoff {
 public:
  void pause() noexcept {
    if (step_ < kSpinSteps) {
      cpu_relax();
    } else if (step_ < kYieldSteps) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    ++step_;
  }

 private:
  static constexpr int kSpinSteps = 64;
  static constexpr int kYieldSteps = 128;
  int step_ = 0;
};

// Single-writer counter: the owning thread uses load+store (no locked RMW
// instruction); other threads may read it at any time.
class OwnedCounter {
 public:
  void add(std::uint64_t n = 1) noexcept {
    value_.store(value_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t get() const noexcept {
    return value_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<std::uint64_t> value_{0};
};

}  // namespace detail

// The members are deliberately laid out on separate cache lines, which the
// padding analyzer flags as wasted space.
class RecordRing {  // NOLINT(clang-analyzer-optin.performance.Padding)
 public:
  static constexpr std::size_t kSlotHeaderBytes = 64;
  static_assert(sizeof(RingHeader) <= kSlotHeaderBytes);

  // capacity is rounded up to a power of two.
  RecordRing(std::size_t capacity, std::size_t max_payload, OverflowPolicy policy);

  RecordRing(const RecordRing&) = delete;
  RecordRing& operator=(const RecordRing&) = delete;
  RecordRing(RecordRing&&) = delete;
  RecordRing& operator=(RecordRing&&) = delete;
  ~RecordRing() = default;

  // ---- producer side (exactly one thread) --------------------------------

  // Reserves a slot and lets `fill` write the payload in place.
  template <typename Fill>
    requires std::invocable<Fill&, MutableByteSpan>
  PushResult push_with(const RecordMeta& meta, std::int64_t enqueue_ns, std::size_t payload_size,
                       Fill&& fill) {
    if (payload_size > max_payload_) {
      producer_.too_large.add();
      return PushResult::kTooLarge;
    }
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (const PushResult r = reserve(head); r != PushResult::kOk) {
      return r;
    }
    std::byte* slot = slot_at(head);
    const RingHeader header{meta, enqueue_ns, static_cast<std::uint32_t>(payload_size)};
    std::memcpy(slot, &header, sizeof(header));
    std::invoke(std::forward<Fill>(fill), MutableByteSpan(slot + kSlotHeaderBytes, payload_size));
    head_.store(head + 1, std::memory_order_release);
    producer_.pushed.add();
    return PushResult::kOk;
  }

  PushResult push(const RecordMeta& meta, std::int64_t enqueue_ns, ByteSpan payload) {
    return push_with(meta, enqueue_ns, payload.size(), [payload](MutableByteSpan dst) noexcept {
      std::memcpy(dst.data(), payload.data(), payload.size());
    });
  }

  // ---- consumer side (exactly one thread) --------------------------------

  // Calls visit(const RingHeader&, ByteSpan payload) for the oldest record and
  // releases the slot. Returns false if the ring was empty.
  template <typename Visit>
    requires std::is_nothrow_invocable_v<Visit&, const RingHeader&, ByteSpan>
  bool try_pop(Visit&& visit) noexcept {
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    for (;;) {
      // `>=` rather than `==`: under drop-oldest the producer can move tail_
      // past our cached head, and we must re-acquire head_ before reading.
      if (tail >= consumer_cached_head_) {
        consumer_cached_head_ = head_.load(std::memory_order_acquire);
        if (tail >= consumer_cached_head_) {
          return false;
        }
      }
      if (policy_ != OverflowPolicy::kDropOldest) {
        break;
      }
      if (tail_.compare_exchange_weak(tail, tail | kLockBit, std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        break;
      }
      // Failed: the producer evicted `tail` (or a spurious failure); `tail`
      // now holds the current value, retry.
    }
    const std::byte* slot = slot_at(tail);
    RingHeader header;
    std::memcpy(&header, slot, sizeof(header));
    std::invoke(std::forward<Visit>(visit), static_cast<const RingHeader&>(header),
                ByteSpan(slot + kSlotHeaderBytes, header.payload_size));
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // ---- either side ---------------------------------------------------------

  // After close() every push returns kStopped and blocked producers wake up.
  // Records already queued can still be popped.
  void close() noexcept { closed_.store(true, std::memory_order_release); }
  [[nodiscard]] bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

  [[nodiscard]] RingCounters counters() const noexcept;
  [[nodiscard]] std::size_t size_approx() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t max_payload() const noexcept { return max_payload_; }
  [[nodiscard]] OverflowPolicy policy() const noexcept { return policy_; }

 private:
  static constexpr std::uint64_t kLockBit = std::uint64_t{1} << 63;
  static constexpr std::uint64_t kIndexMask = ~kLockBit;

  [[nodiscard]] std::byte* slot_at(std::uint64_t index) const noexcept {
    return slots_.get() + (index & mask_) * stride_;
  }

  // Ensures slot `head` is writable, applying the overflow policy if full.
  PushResult reserve(std::uint64_t head) {
    if (head - producer_.cached_tail < capacity_) {
      return closed() ? PushResult::kStopped : PushResult::kOk;
    }
    producer_.cached_tail = tail_.load(std::memory_order_acquire) & kIndexMask;
    if (head - producer_.cached_tail < capacity_) {
      return closed() ? PushResult::kStopped : PushResult::kOk;
    }
    switch (policy_) {
      case OverflowPolicy::kDropNewest:
        producer_.dropped_newest.add();
        return PushResult::kDropped;
      case OverflowPolicy::kDropOldest:
        return evict_oldest(head);
      case OverflowPolicy::kBlock:
        return wait_for_space(head);
    }
    return PushResult::kDropped;
  }

  PushResult evict_oldest(std::uint64_t head);
  PushResult wait_for_space(std::uint64_t head);

  // Shared, read-only after construction.
  const std::size_t capacity_;
  const std::size_t mask_;
  const std::size_t max_payload_;
  const std::size_t stride_;
  const OverflowPolicy policy_;
  const std::unique_ptr<std::byte[]> slots_;

  // Producer-owned state (written only by the producer thread).
  struct alignas(kCacheLineSize) ProducerState {
    std::uint64_t cached_tail = 0;
    detail::OwnedCounter pushed;
    detail::OwnedCounter dropped_newest;
    detail::OwnedCounter evicted_oldest;
    detail::OwnedCounter too_large;
    detail::OwnedCounter blocked_ns;
  };

  alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
  ProducerState producer_;
  alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
  alignas(kCacheLineSize) std::uint64_t consumer_cached_head_{0};
  alignas(kCacheLineSize) std::atomic<bool> closed_{false};
};

}  // namespace mr
