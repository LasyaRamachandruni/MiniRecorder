// Ring buffer micro-benchmarks: lock-free SPSC vs mutex-based queues on the
// same machine, for throughput (one producer thread, one consumer thread)
// and round-trip latency (ping-pong over two queues).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include <mr/record_ring.hpp>
#include <mr/spsc_queue.hpp>

namespace {

// 64-byte message, about the size of an IMU record plus metadata.
struct Msg {
  std::uint64_t seq = 0;
  std::array<std::uint64_t, 7> body{};
};
static_assert(sizeof(Msg) == 64);

constexpr std::size_t kCapacity = 1024;

// Baseline 1: std::queue under a std::mutex, non-blocking try_push/try_pop
// (same spin structure as the SPSC queue, so only the synchronization differs).
template <typename T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : capacity_(capacity) {}
  bool try_push(const T& v) {
    const std::scoped_lock lock(mu_);
    if (q_.size() >= capacity_) {
      return false;
    }
    q_.push(v);
    return true;
  }
  bool try_pop(T& out) {
    const std::scoped_lock lock(mu_);
    if (q_.empty()) {
      return false;
    }
    out = q_.front();
    q_.pop();
    return true;
  }

 private:
  std::mutex mu_;
  std::queue<T> q_;
  std::size_t capacity_;
};

// Baseline 2: the common blocking producer/consumer queue with condition
// variables (threads sleep instead of spinning).
template <typename T>
class CondvarQueue {
 public:
  explicit CondvarQueue(std::size_t capacity) : capacity_(capacity) {}
  void push(const T& v) {
    std::unique_lock lock(mu_);
    not_full_.wait(lock, [this] { return q_.size() < capacity_; });
    q_.push(v);
    lock.unlock();
    not_empty_.notify_one();
  }
  void pop(T& out) {
    std::unique_lock lock(mu_);
    not_empty_.wait(lock, [this] { return !q_.empty(); });
    out = q_.front();
    q_.pop();
    lock.unlock();
    not_full_.notify_one();
  }

 private:
  std::mutex mu_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::queue<T> q_;
  std::size_t capacity_;
};

constexpr std::int64_t kItemsPerIteration = 1 << 18;

// ---------------------------------------------------------------- throughput

template <typename Queue>
void spin_throughput(benchmark::State& state) {
  for (auto _ : state) {
    Queue q(kCapacity);
    std::thread producer([&q] {
      Msg m;
      for (std::int64_t i = 0; i < kItemsPerIteration; ++i) {
        m.seq = static_cast<std::uint64_t>(i);
        while (!q.try_push(m)) {
        }
      }
    });
    Msg out;
    std::uint64_t sum = 0;
    for (std::int64_t i = 0; i < kItemsPerIteration; ++i) {
      while (!q.try_pop(out)) {
      }
      sum += out.seq;
    }
    producer.join();
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * kItemsPerIteration);
  state.SetBytesProcessed(state.iterations() * kItemsPerIteration *
                          static_cast<std::int64_t>(sizeof(Msg)));
}

void BM_Throughput_SpscQueue(benchmark::State& state) {
  spin_throughput<mr::SpscQueue<Msg>>(state);
}
void BM_Throughput_MutexQueue(benchmark::State& state) { spin_throughput<MutexQueue<Msg>>(state); }

void BM_Throughput_CondvarQueue(benchmark::State& state) {
  for (auto _ : state) {
    CondvarQueue<Msg> q(kCapacity);
    std::thread producer([&q] {
      Msg m;
      for (std::int64_t i = 0; i < kItemsPerIteration; ++i) {
        m.seq = static_cast<std::uint64_t>(i);
        q.push(m);
      }
    });
    Msg out;
    std::uint64_t sum = 0;
    for (std::int64_t i = 0; i < kItemsPerIteration; ++i) {
      q.pop(out);
      sum += out.seq;
    }
    producer.join();
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * kItemsPerIteration);
  state.SetBytesProcessed(state.iterations() * kItemsPerIteration *
                          static_cast<std::int64_t>(sizeof(Msg)));
}

// RecordRing with a given payload size (48 B = IMU sample, 307216 B = a
// 640x480 GRAY8 camera frame plus header). Producer fills in place, consumer
// copies out, exactly like the recorder.
void BM_Throughput_RecordRing(benchmark::State& state) {
  const auto payload = static_cast<std::size_t>(state.range(0));
  const std::int64_t items = payload > 4096 ? 2048 : kItemsPerIteration;
  std::vector<std::byte> source(payload, std::byte{0x5A});
  std::vector<std::byte> sink(payload);
  for (auto _ : state) {
    mr::RecordRing ring(payload > 4096 ? 16 : kCapacity, payload, mr::OverflowPolicy::kBlock);
    std::thread producer([&] {
      for (std::int64_t i = 0; i < items; ++i) {
        const mr::RecordMeta meta{.timestamp_ns = i, .seq = static_cast<std::uint64_t>(i)};
        (void)ring.push(meta, 0, source);
      }
    });
    std::uint64_t sum = 0;
    for (std::int64_t i = 0; i < items;) {
      if (ring.try_pop([&](const mr::RingHeader& h, mr::ByteSpan p) noexcept {
            std::memcpy(sink.data(), p.data(), p.size());
            sum += h.meta.seq;
          })) {
        ++i;
      }
    }
    producer.join();
    benchmark::DoNotOptimize(sum);
    benchmark::DoNotOptimize(sink.data());
  }
  state.SetItemsProcessed(state.iterations() * items);
  state.SetBytesProcessed(state.iterations() * items * static_cast<std::int64_t>(payload));
}

// ---------------------------------------------------------------- latency

// One iteration = one message to the echo thread and back. Reports p50/p99 of
// the round trip in nanoseconds as counters.
template <typename Queue>
void ping_pong(benchmark::State& state) {
  Queue to_echo(kCapacity);
  Queue from_echo(kCapacity);
  std::atomic<bool> stop{false};
  std::thread echo([&] {
    Msg m;
    while (!stop.load(std::memory_order_relaxed)) {
      if (to_echo.try_pop(m)) {
        while (!from_echo.try_push(m)) {
        }
      }
    }
  });
  std::vector<std::int64_t> rtt;
  rtt.reserve(1 << 20);
  Msg m;
  Msg back;
  for (auto _ : state) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!to_echo.try_push(m)) {
    }
    while (!from_echo.try_pop(back)) {
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (rtt.size() < rtt.capacity()) {
      rtt.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    ++m.seq;
  }
  stop.store(true);
  echo.join();
  std::sort(rtt.begin(), rtt.end());
  if (!rtt.empty()) {
    state.counters["rtt_p50_ns"] = static_cast<double>(rtt[rtt.size() / 2]);
    state.counters["rtt_p99_ns"] = static_cast<double>(rtt[rtt.size() * 99 / 100]);
  }
}

void BM_PingPong_SpscQueue(benchmark::State& state) { ping_pong<mr::SpscQueue<Msg>>(state); }
void BM_PingPong_MutexQueue(benchmark::State& state) { ping_pong<MutexQueue<Msg>>(state); }

}  // namespace

BENCHMARK(BM_Throughput_SpscQueue)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Throughput_MutexQueue)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Throughput_CondvarQueue)->Unit(benchmark::kMillisecond)->UseRealTime();
BENCHMARK(BM_Throughput_RecordRing)
    ->Arg(48)
    ->Arg(640 * 480 + 16)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
BENCHMARK(BM_PingPong_SpscQueue)->UseRealTime();
BENCHMARK(BM_PingPong_MutexQueue)->UseRealTime();

BENCHMARK_MAIN();
