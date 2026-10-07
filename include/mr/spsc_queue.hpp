#pragma once

// Bounded lock-free single-producer/single-consumer queue for small value
// types. This is the textbook building block; RecordRing (record_ring.hpp)
// applies the same index protocol to variable-size byte slots and adds
// overflow policies.
//
// Protocol
//   head_ : next slot to write. Written only by the producer.
//   tail_ : next slot to read.  Written only by the consumer.
//   Both counters increase monotonically (64-bit, never wrap in practice)
//   and are mapped to slots with `index & mask_`.
//
// Memory ordering
//   The producer writes the slot, then publishes it with head_.store(release).
//   The consumer reads head_ with acquire, so the slot contents are visible
//   before it reads them. Symmetrically the consumer releases a slot with
//   tail_.store(release) after it finished reading, and the producer's
//   tail_.load(acquire) guarantees the read happened-before the overwrite.
//   Each side keeps a private cached copy of the other side's counter and only
//   re-reads the shared atomic when the cache says full/empty, which keeps the
//   cache line of the other side's counter out of the common path.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace mr {

// 64 bytes on all x86-64 and most ARM cores. std::hardware_destructive_
// interference_size is not used because GCC warns that its value may differ
// between translation units compiled with different -mtune.
inline constexpr std::size_t kCacheLineSize = 64;

[[nodiscard]] constexpr std::size_t round_up_pow2(std::size_t n) noexcept {
  return n < 2 ? 2 : std::bit_ceil(n);
}

template <typename T>
  requires std::is_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>
class SpscQueue {
 public:
  // Capacity is rounded up to a power of two (minimum 2).
  explicit SpscQueue(std::size_t capacity)
      : capacity_(round_up_pow2(capacity)),
        mask_(capacity_ - 1),
        slots_(std::make_unique<T[]>(capacity_)) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;
  ~SpscQueue() = default;

  // Producer side.
  template <typename U>
    requires std::is_assignable_v<T&, U&&>
  [[nodiscard]] bool try_push(U&& value) noexcept(std::is_nothrow_assignable_v<T&, U&&>) {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - producer_cached_tail_ == capacity_) {
      producer_cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head - producer_cached_tail_ == capacity_) {
        return false;
      }
    }
    slots_[head & mask_] = std::forward<U>(value);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == consumer_cached_head_) {
      consumer_cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == consumer_cached_head_) {
        return false;
      }
    }
    out = std::move(slots_[tail & mask_]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Approximate when called concurrently; exact when both sides are quiet.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(head - tail);
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  // Read-only after construction; shared by both threads.
  const std::size_t capacity_;
  const std::size_t mask_;
  const std::unique_ptr<T[]> slots_;

  // Each counter and each private cache lives on its own cache line so the
  // producer and consumer never write to the same line. Because the class is
  // over-aligned, sizeof() is a multiple of the line size and the last member
  // cannot share a line with whatever follows the object.
  alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
  alignas(kCacheLineSize) std::uint64_t producer_cached_tail_{0};
  alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
  alignas(kCacheLineSize) std::uint64_t consumer_cached_head_{0};
};

}  // namespace mr
