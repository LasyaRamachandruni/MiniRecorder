// mr_replay: replays a log in timestamp order at N x speed into a sink and
// measures how precisely messages are delivered on schedule.

#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <thread>

#include <mr/crc32c.hpp>
#include <mr/reader.hpp>

#include "cli.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Where replayed messages go. A real system would publish to middleware;
// here the sinks only touch the bytes so the work is not optimized away.
class Sink {
 public:
  Sink() = default;
  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;
  Sink(Sink&&) = delete;
  Sink& operator=(Sink&&) = delete;
  virtual ~Sink() = default;
  virtual void deliver(const mr::Message& m) = 0;
  [[nodiscard]] virtual std::uint64_t digest() const = 0;
};

class NullSink final : public Sink {
 public:
  void deliver(const mr::Message& m) override {
    acc_ += m.payload.size() + (m.payload.empty() ? 0 : std::to_integer<std::uint64_t>(m.payload[0]));
  }
  [[nodiscard]] std::uint64_t digest() const override { return acc_; }

 private:
  std::uint64_t acc_ = 0;
};

class ChecksumSink final : public Sink {
 public:
  void deliver(const mr::Message& m) override { crc_ = mr::crc32c::extend(crc_, m.payload); }
  [[nodiscard]] std::uint64_t digest() const override { return crc_; }

 private:
  std::uint32_t crc_ = 0;
};

int run(int argc, const char* const* argv) {
  mr::cli::ArgParser args("mr_replay",
                          "Replays a log in timestamp order at N x speed and reports how late each\n"
                          "message was delivered relative to its schedule (p50/p99/max).");
  args.positional("file", "log file (*.mrlog)");
  args.option("speed", "replay speed multiplier (0 = as fast as possible)", "1");
  args.option("streams", "comma separated stream names (default: all sensor streams)", "");
  args.option("start-s", "start offset in seconds from the beginning of the log", "0");
  args.option("duration-s", "seconds of log time to replay (0 = all)", "0");
  args.option("spin-us", "busy-wait this long before each deadline instead of sleeping", "0");
  args.option("sink", "null | checksum", "null");
  args.option("json", "write results as JSON to this file", "");
  if (auto s = args.parse(argc, argv); !s) {
    std::cerr << "mr_replay: " << s.error().message << "\n\n" << args.usage();
    return 2;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return 0;
  }
  const auto speed = args.get_double("speed");
  const auto start_s = args.get_double("start-s");
  const auto duration_s = args.get_double("duration-s");
  const auto spin_us = args.get_int("spin-us");
  for (const auto* e : {speed ? nullptr : &speed.error(), start_s ? nullptr : &start_s.error(),
                        duration_s ? nullptr : &duration_s.error(), spin_us ? nullptr : &spin_us.error()}) {
    if (e != nullptr) {
      std::cerr << "mr_replay: " << e->message << "\n";
      return 2;
    }
  }

  const std::filesystem::path path = args.get_positional(0).value_or(std::string{});
  auto reader = mr::Reader::open(path);
  if (!reader) {
    std::cerr << "mr_replay: " << path.string() << ": " << reader.error().describe() << "\n";
    return 1;
  }

  mr::ReadOptions opts;
  opts.stream_mask = ~(std::uint64_t{1} << mr::kEventStreamId);
  if (!args.get("streams").empty()) {
    opts.stream_mask = 0;
    for (const std::string& name : mr::cli::split(args.get("streams"), ',')) {
      bool found = false;
      for (const auto& s : reader->header().streams) {
        if (s.name == name) {
          opts.stream_mask |= std::uint64_t{1} << s.id;
          found = true;
        }
      }
      if (!found) {
        std::cerr << "mr_replay: unknown stream '" << name << "'\n";
        return 2;
      }
    }
  }
  const std::int64_t t0 = reader->footer().start_time_ns + static_cast<std::int64_t>(*start_s * 1e9);
  opts.start_ns = t0;
  if (*duration_s > 0) {
    opts.end_ns = t0 + static_cast<std::int64_t>(*duration_s * 1e9);
  }

  std::unique_ptr<Sink> sink;
  if (args.get("sink") == "checksum") {
    sink = std::make_unique<ChecksumSink>();
  } else if (args.get("sink") == "null") {
    sink = std::make_unique<NullSink>();
  } else {
    std::cerr << "mr_replay: --sink must be null or checksum\n";
    return 2;
  }

  const auto spin = std::chrono::microseconds(*spin_us);
  auto it = reader->messages(opts);
  mr::Message m;
  std::vector<std::int64_t> lateness;
  std::uint64_t bytes = 0;
  std::uint64_t messages = 0;
  std::int64_t first_ts = 0;
  bool first = true;
  Clock::time_point wall0;
  const auto run_start = Clock::now();
  for (;;) {
    auto more = it.next(m);
    if (!more) {
      std::cerr << "mr_replay: " << more.error().describe() << "\n";
      return 1;
    }
    if (!more.value()) {
      break;
    }
    if (first) {
      first = false;
      first_ts = m.meta.timestamp_ns;
      wall0 = Clock::now();
    }
    if (*speed > 0) {
      const auto offset = std::chrono::nanoseconds(
          static_cast<std::int64_t>(static_cast<double>(m.meta.timestamp_ns - first_ts) / *speed));
      const Clock::time_point deadline = wall0 + offset;
      if (Clock::now() < deadline - spin) {
        std::this_thread::sleep_until(deadline - spin);
      }
      while (Clock::now() < deadline) {
        // busy-wait the last stretch for tighter delivery
      }
      sink->deliver(m);
      lateness.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - deadline).count());
    } else {
      sink->deliver(m);
    }
    bytes += m.payload.size();
    ++messages;
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - run_start).count();
  const auto p = mr::cli::percentiles(lateness);
  std::cout << std::format("replayed {} messages ({:.2f} MB) in {:.3f} s at speed {}x, sink={} digest={:x}\n",
                           messages, static_cast<double>(bytes) / 1e6,
                           elapsed, *speed, args.get("sink"), sink->digest());
  if (*speed > 0) {
    std::cout << std::format(
        "schedule lateness: p50 {:.1f} us, p90 {:.1f} us, p99 {:.1f} us, p99.9 {:.1f} us, max {:.1f} us\n",
        p.p50_ns / 1e3, p.p90_ns / 1e3, p.p99_ns / 1e3, p.p999_ns / 1e3, p.max_ns / 1e3);
  } else {
    std::cout << std::format("throughput: {:.1f} MB/s\n", static_cast<double>(bytes) / 1e6 / elapsed);
  }
  if (!args.get("json").empty()) {
    std::ofstream out(args.get("json"));
    out << std::format(
        "{{\"file\": \"{}\", \"speed\": {}, \"spin_us\": {}, \"messages\": {}, \"bytes\": {}, \"elapsed_s\": {:.6f}, "
        "\"lateness_us\": {{\"p50\": {:.2f}, \"p90\": {:.2f}, \"p99\": {:.2f}, \"p999\": {:.2f}, \"max\": {:.2f}}}}}\n",
        path.string(), *speed, *spin_us, messages, bytes, elapsed, p.p50_ns / 1e3, p.p90_ns / 1e3,
        p.p99_ns / 1e3, p.p999_ns / 1e3, p.max_ns / 1e3);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "mr_replay: fatal: " << e.what() << "\n";
    return 1;
  }
}
