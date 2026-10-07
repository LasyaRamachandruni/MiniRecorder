// mr_record: runs the simulated (or KITTI-fed) sensor set and records a log.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <format>
#include <fstream>
#include <iostream>
#include <thread>

#include <mr/recorder.hpp>

#include "cli.hpp"

namespace {

// Set from the signal handler; lock-free atomics are async-signal-safe.
std::atomic<bool> g_interrupted{false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void on_signal(int /*sig*/) { g_interrupted.store(true); }

std::string json_escape(std::string_view s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

mr::Result<mr::FaultConfig> parse_faults(const std::string& text) {
  mr::FaultConfig f;
  for (const std::string& item : mr::cli::split(text, ',')) {
    const auto kv = mr::cli::split(item, '=');
    if (kv.size() != 2) {
      return mr::make_error(mr::Errc::kInvalidArgument, "faults: expected key=value, got '" + item + "'");
    }
    double v = 0;
    try {
      v = std::stod(kv[1]);
    } catch (const std::exception&) {
      return mr::make_error(mr::Errc::kInvalidArgument, "faults: bad number '" + kv[1] + "'");
    }
    if (kv[0] == "gap") {
      f.gap_probability = v;
    } else if (kv[0] == "regress") {
      f.regression_probability = v;
    } else if (kv[0] == "stall") {
      f.stall_probability = v;
    } else if (kv[0] == "stall_ms") {
      f.stall_duration = std::chrono::milliseconds(static_cast<std::int64_t>(v));
    } else {
      return mr::make_error(mr::Errc::kInvalidArgument, "faults: unknown key '" + kv[0] + "'");
    }
  }
  return f;
}

// "--policy camera_front=drop-newest,imu=block" / "--ring imu=4096"
mr::Status apply_per_stream(const std::string& text, std::vector<mr::StreamSetup>& streams,
                            bool is_policy) {
  for (const std::string& item : mr::cli::split(text, ',')) {
    const auto kv = mr::cli::split(item, '=');
    if (kv.size() != 2) {
      return mr::make_error(mr::Errc::kInvalidArgument, "expected stream=value, got '" + item + "'");
    }
    bool matched = false;
    for (mr::StreamSetup& s : streams) {
      if (kv[0] != s.name && kv[0] != "all") {
        continue;
      }
      matched = true;
      if (is_policy) {
        const auto p = mr::parse_overflow_policy(kv[1]);
        if (!p) {
          return mr::make_error(mr::Errc::kInvalidArgument, "unknown overflow policy '" + kv[1] + "'");
        }
        s.policy = *p;
      } else {
        s.ring_capacity = static_cast<std::size_t>(std::stoull(kv[1]));
      }
    }
    if (!matched) {
      return mr::make_error(mr::Errc::kInvalidArgument, "unknown stream '" + kv[0] + "'");
    }
  }
  return mr::ok_status();
}

void write_summary_json(const std::string& path, const mr::RecorderConfig& cfg,
                        const mr::RecorderReport& r) {
  const mr::WriterMetrics& m = r.write.metrics;
  std::ofstream out(path);
  out << "{\n";
  out << std::format("  \"output\": \"{}\",\n", json_escape(r.write.path.string()));
  out << std::format("  \"mode\": \"{}\",\n", cfg.mode == mr::RecordMode::kTriggered ? "triggered" : "continuous");
  out << std::format("  \"zstd_level\": {},\n", cfg.writer.zstd_level);
  out << std::format("  \"fsync\": \"{}\",\n", mr::describe(cfg.writer.fsync));
  out << std::format("  \"pacing\": \"{}\",\n", cfg.pacing == mr::Pacing::kUnpaced ? "unpaced" : "realtime");
  out << std::format("  \"rate_scale\": {},\n", cfg.rate_scale);
  out << std::format("  \"elapsed_s\": {:.6f},\n", r.elapsed_s);
  out << std::format("  \"records_written\": {},\n", m.records_written);
  out << std::format("  \"record_bytes\": {},\n", m.record_bytes);
  out << std::format("  \"file_bytes\": {},\n", m.file_bytes);
  out << std::format("  \"chunks_written\": {},\n", m.chunks_written);
  out << std::format("  \"fsync_count\": {},\n", m.fsync_count);
  out << std::format("  \"compress_cpu_s\": {:.6f},\n", static_cast<double>(m.compress_ns) / 1e9);
  out << std::format("  \"write_s\": {:.6f},\n", static_cast<double>(m.write_ns) / 1e9);
  out << std::format("  \"fsync_s\": {:.6f},\n", static_cast<double>(m.fsync_ns) / 1e9);
  out << std::format("  \"append_blocked_s\": {:.6f},\n", static_cast<double>(m.append_blocked_ns) / 1e9);
  out << std::format("  \"quality_events\": {},\n", r.quality_events);
  out << std::format("  \"triggers\": {},\n", r.triggers);
  auto latency = [&out](const char* name, const std::vector<std::int64_t>& v) {
    const auto p = mr::cli::percentiles(v);
    out << std::format(
        "  \"{}\": {{\"count\": {}, \"p50_ms\": {:.4f}, \"p90_ms\": {:.4f}, \"p99_ms\": {:.4f}, "
        "\"p999_ms\": {:.4f}, \"max_ms\": {:.4f}, \"mean_ms\": {:.4f}}},\n",
        name, p.count, p.p50_ns / 1e6, p.p90_ns / 1e6, p.p99_ns / 1e6, p.p999_ns / 1e6, p.max_ns / 1e6,
        p.mean_ns / 1e6);
  };
  latency("write_latency", m.write_latency_ns);
  latency("durable_latency", m.durable_latency_ns);
  out << "  \"streams\": [\n";
  for (std::size_t i = 0; i < r.write.footer.streams.size(); ++i) {
    const mr::format::StreamStats& st = r.write.footer.streams[i];
    const mr::StreamReport* sr = nullptr;
    for (const auto& candidate : r.streams) {
      if (candidate.id == st.id) {
        sr = &candidate;
      }
    }
    out << std::format(
        "    {{\"id\": {}, \"name\": \"{}\", \"policy\": \"{}\", \"produced\": {}, \"records\": {}, "
        "\"payload_bytes\": {}, \"stored_bytes\": {}, \"ring_dropped_newest\": {}, "
        "\"ring_evicted_oldest\": {}, \"producer_blocked_s\": {:.6f}, \"seq_gaps\": {}, "
        "\"ts_regressions\": {}, \"rate_events\": {}, \"stale_events\": {}}}{}\n",
        st.id, sr != nullptr ? json_escape(sr->name) : "_events",
        sr != nullptr ? mr::to_string(sr->policy) : "n/a", sr != nullptr ? sr->sensor.emitted : st.records,
        st.records, st.payload_bytes, st.stored_bytes, sr != nullptr ? sr->ring.dropped_newest : 0,
        sr != nullptr ? sr->ring.evicted_oldest : 0,
        sr != nullptr ? static_cast<double>(sr->ring.blocked_ns) / 1e9 : 0.0, st.seq_gaps,
        st.ts_regressions, st.rate_events, st.stale_events,
        i + 1 < r.write.footer.streams.size() ? "," : "");
  }
  out << "  ]\n}\n";
}

void declare_options(mr::cli::ArgParser& args) {
  args.option("out", "output file (written as <out>.partial until closed)", "recording.mrlog");
  args.option("duration", "seconds to record", "10");
  args.option("mode", "continuous | triggered", "continuous");
  args.option("zstd-level", "zstd level for chunks; 0 stores raw", "3");
  args.option("fsync", "chunk | interval:<ms> | close", "interval:100");
  args.option("chunk-kb", "target chunk size in KiB", "1024");
  args.option("chunk-age-ms", "seal a chunk after this long even if small", "500");
  args.option("compression-threads", "zstd worker threads", "2");
  args.option("inflight", "max chunks between seal and write", "8");
  args.option("camera", "camera WIDTHxHEIGHT@HZ, or 'off'", "640x480@30");
  args.option("imu-hz", "IMU rate", "200");
  args.option("gps-hz", "GPS rate", "10");
  args.option("wheel-hz", "wheel speed CAN rate", "100");
  args.option("policy", "overflow policy per stream, e.g. camera_front=drop-newest,all=block", "");
  args.option("ring", "ring capacity per stream, e.g. imu=4096", "");
  args.option("rate-scale", "run sensors N times faster than real time", "1");
  args.flag("unpaced", "produce as fast as possible (throughput/overload tests)");
  args.option("faults", "inject faults: gap=P,regress=P,stall=P,stall_ms=MS", "");
  args.option("pre-s", "triggered mode: seconds kept before a trigger", "3");
  args.option("post-s", "triggered mode: seconds recorded after a trigger", "2");
  args.option("brake-threshold", "hard-brake deceleration threshold, m/s^2", "5");
  args.flag("trigger-on-quality", "triggered mode: data-quality faults also trigger");
  args.option("brake-period-s", "simulated vehicle brakes once per this many seconds", "30");
  args.option("gps-decimals", "privacy: round GPS to N decimals (-1 = off)", "-1");
  args.option("roi", "privacy: camera regions to zero, 'x,y,w,h;x,y,w,h'", "");
  args.option("kitti", "KITTI raw oxts directory to replay as IMU+GPS", "");
  args.option("seed", "random seed", "42");
  args.flag("track-latency", "measure enqueue-to-write and enqueue-to-durable latency per record");
  args.option("summary-json", "write a JSON summary here", "");
  args.option("progress-fd", "write 'durable <chunks>' lines to this fd after each sync", "-1");
  args.flag("quiet", "no progress output");
}

struct Job {
  mr::RecorderConfig cfg;
  double duration_s = 0;
  bool quiet = false;
  std::string summary_json;
};

mr::Result<Job> build_job(const mr::cli::ArgParser& args) {
  // Collects the first parse error so option handling stays linear.
  std::optional<mr::Error> parse_error;
  auto take = [&parse_error]<typename T>(mr::Result<T> r) -> T {
    if (!r) {
      if (!parse_error) {
        parse_error = r.error();
      }
      return T{};
    }
    return r.value();
  };

  mr::RecorderConfig cfg;
  cfg.output = args.get("out");
  const double duration = take(args.get_double("duration"));
  const std::int64_t level = take(args.get_int("zstd-level"));
  const std::int64_t chunk_kb = take(args.get_int("chunk-kb"));
  const std::int64_t chunk_age = take(args.get_int("chunk-age-ms"));
  const std::int64_t threads = take(args.get_int("compression-threads"));
  const std::int64_t inflight = take(args.get_int("inflight"));
  const double imu_hz = take(args.get_double("imu-hz"));
  const double gps_hz = take(args.get_double("gps-hz"));
  const double wheel_hz = take(args.get_double("wheel-hz"));
  const double scale = take(args.get_double("rate-scale"));
  const double pre = take(args.get_double("pre-s"));
  const double post = take(args.get_double("post-s"));
  const double brake = take(args.get_double("brake-threshold"));
  const double brake_period = take(args.get_double("brake-period-s"));
  const std::int64_t decimals = take(args.get_int("gps-decimals"));
  const std::int64_t seed = take(args.get_int("seed"));
  const std::int64_t progress_fd = take(args.get_int("progress-fd"));
  if (parse_error) {
    return *parse_error;
  }
  if (chunk_kb <= 0 || threads <= 0 || inflight <= 0 || chunk_age <= 0) {
    return mr::make_error(mr::Errc::kInvalidArgument,
                          "chunk-kb, chunk-age-ms, compression-threads and inflight must be positive");
  }

  // Streams.
  cfg.streams.clear();
  auto add_stream = [&cfg](std::string name, mr::SensorKind kind, double hz, std::size_t ring) {
    mr::StreamSetup s;
    s.name = std::move(name);
    s.kind = kind;
    s.rate_hz = hz;
    s.ring_capacity = ring;
    cfg.streams.push_back(std::move(s));
  };
  add_stream("imu", mr::SensorKind::kImu, imu_hz, 2048);
  add_stream("gps", mr::SensorKind::kGps, gps_hz, 128);
  add_stream("wheel_can", mr::SensorKind::kWheelCan, wheel_hz, 1024);
  if (args.get("camera") != "off") {
    unsigned w = 0;
    unsigned h = 0;
    double hz = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err34-c): simple fixed format
    if (std::sscanf(args.get("camera").c_str(), "%ux%u@%lf", &w, &h, &hz) != 3 || w == 0 || h == 0 ||
        w > 4096 || h > 4096 || !(hz > 0)) {
      return mr::make_error(mr::Errc::kInvalidArgument, "--camera expects WIDTHxHEIGHT@HZ");
    }
    add_stream("camera_front", mr::SensorKind::kCamera, hz, 16);
    cfg.streams.back().camera_width = static_cast<std::uint16_t>(w);
    cfg.streams.back().camera_height = static_cast<std::uint16_t>(h);
  }
  if (!args.get("faults").empty()) {
    auto faults = parse_faults(args.get("faults"));
    if (!faults) {
      return faults.error();
    }
    for (auto& s : cfg.streams) {
      s.faults = *faults;
    }
  }
  if (auto st = apply_per_stream(args.get("policy"), cfg.streams, true); !st) {
    return st.error();
  }
  if (auto st = apply_per_stream(args.get("ring"), cfg.streams, false); !st) {
    return st.error();
  }

  // Writer.
  auto fsync = mr::parse_fsync_policy(args.get("fsync"));
  if (!fsync) {
    return fsync.error();
  }
  cfg.writer.fsync = *fsync;
  cfg.writer.zstd_level = static_cast<int>(level);
  cfg.writer.chunk_target_bytes = static_cast<std::size_t>(chunk_kb) * 1024;
  cfg.writer.chunk_max_age = std::chrono::milliseconds(chunk_age);
  cfg.writer.compression_threads = static_cast<unsigned>(threads);
  cfg.writer.max_inflight_chunks = static_cast<std::size_t>(inflight);
  cfg.writer.track_latency = args.is_set("track-latency");
  if (progress_fd >= 0) {
    const int fd = static_cast<int>(progress_fd);
    cfg.writer.on_durable = [fd](std::uint64_t chunks) {
      const std::string line = std::format("durable {}\n", chunks);
      // Best effort; the reader side may have gone away.
      [[maybe_unused]] const auto n = ::write(fd, line.data(), line.size());
    };
  }

  // Modes and filters.
  const std::string& mode = args.get("mode");
  if (mode == "triggered") {
    cfg.mode = mr::RecordMode::kTriggered;
  } else if (mode != "continuous") {
    return mr::make_error(mr::Errc::kInvalidArgument, "--mode must be continuous or triggered");
  }
  cfg.trigger.pre_window = std::chrono::milliseconds(static_cast<std::int64_t>(pre * 1000));
  cfg.trigger.post_window = std::chrono::milliseconds(static_cast<std::int64_t>(post * 1000));
  cfg.trigger.hard_brake_mps2 = brake;
  cfg.trigger.on_quality_fault = args.is_set("trigger-on-quality");
  cfg.vehicle.cycle_s = brake_period;
  cfg.vehicle.brake_start_s = std::max(0.5, brake_period - 10.0);
  if (decimals >= 0) {
    cfg.privacy.gps_decimals = static_cast<int>(decimals);
  }
  for (const std::string& roi_text : mr::cli::split(args.get("roi"), ';')) {
    auto roi = mr::parse_roi(roi_text);
    if (!roi) {
      return roi.error();
    }
    cfg.privacy.camera_rois.push_back(*roi);
  }
  cfg.pacing = args.is_set("unpaced") ? mr::Pacing::kUnpaced : mr::Pacing::kRealTime;
  cfg.rate_scale = scale;
  cfg.seed = static_cast<std::uint64_t>(seed);
  if (!args.get("kitti").empty()) {
    cfg.kitti_oxts_dir = args.get("kitti");
  }


  return Job{.cfg = std::move(cfg),
             .duration_s = duration,
             .quiet = args.is_set("quiet"),
             .summary_json = args.get("summary-json")};
}

void print_report(const mr::RecorderReport& r) {
  const mr::WriterMetrics& m = r.write.metrics;
  std::cout << std::format(
      "wrote {} ({} records, {} chunks, {:.1f} MB on disk, {:.1f} MB of records, ratio {:.2f})\n"
      "elapsed {:.2f} s: {:.0f} records/s, {:.1f} MB/s of records, {:.1f} MB/s to disk\n"
      "quality events: {}, triggers: {}\n",
      r.write.path.string(), m.records_written, m.chunks_written, static_cast<double>(m.file_bytes) / 1e6,
      static_cast<double>(m.record_bytes) / 1e6,
      m.file_bytes > 0 ? static_cast<double>(m.record_bytes) / static_cast<double>(m.file_bytes) : 0.0,
      r.elapsed_s, static_cast<double>(m.records_written) / r.elapsed_s,
      static_cast<double>(m.record_bytes) / 1e6 / r.elapsed_s,
      static_cast<double>(m.file_bytes) / 1e6 / r.elapsed_s, r.quality_events, r.triggers);
  for (const auto& s : r.streams) {
    std::cout << std::format("  {:<13} produced {:>8}  dropped {:>6}  evicted {:>6}  blocked {:.3f} s\n",
                             s.name, s.sensor.emitted, s.ring.dropped_newest, s.ring.evicted_oldest,
                             static_cast<double>(s.ring.blocked_ns) / 1e9);
  }
}

int fail(const mr::Error& e) {
  std::cerr << "mr_record: " << e.describe() << "\n";
  return 2;
}

int record(const Job& job) {
  auto recorder = mr::Recorder::create(job.cfg);
  if (!recorder) {
    return fail(recorder.error());
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  if (auto st = (*recorder)->start(); !st) {
    return fail(st.error());
  }
  const auto t0 = std::chrono::steady_clock::now();
  const auto end = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(job.duration_s));
  auto next_print = t0 + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < end && !g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!job.quiet && std::chrono::steady_clock::now() >= next_print) {
      next_print += std::chrono::seconds(1);
      std::cerr << std::format("  {:.0f}s  {} records collected\n",
                               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                               (*recorder)->collected_records());
    }
  }
  auto report = (*recorder)->stop();
  if (!report) {
    return fail(report.error());
  }
  if (!job.quiet) {
    print_report(report.value());
  }
  if (!job.summary_json.empty()) {
    write_summary_json(job.summary_json, job.cfg, report.value());
  }
  return 0;
}

int run(int argc, const char* const* argv) {
  mr::cli::ArgParser args("mr_record",
                          "Records simulated vehicle sensors (IMU, GPS, wheel CAN, camera) into a\n"
                          "crash-safe chunked log. Data is synthetic unless --kitti is given.");
  declare_options(args);
  if (auto s = args.parse(argc, argv); !s) {
    std::cerr << "mr_record: " << s.error().message << "\n\n" << args.usage();
    return 2;
  }
  if (args.help_requested()) {
    std::cout << args.usage();
    return 0;
  }
  auto job = build_job(args);
  if (!job) {
    return fail(job.error());
  }
  return record(job.value());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "mr_record: fatal: " << e.what() << "\n";
    return 1;
  }
}
