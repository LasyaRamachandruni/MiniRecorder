#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <thread>

#include <mr/events.hpp>
#include <mr/kitti.hpp>
#include <mr/recorder.hpp>

namespace mr {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t to_ns(Clock::time_point t) noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// How many records the collector takes from one ring before moving on, so a
// busy stream cannot starve the others.
constexpr int kPopBatch = 64;
constexpr auto kHousekeepingPeriod = std::chrono::milliseconds(10);
constexpr auto kIdleSleep = std::chrono::microseconds(200);

std::size_t max_payload_for(const StreamSetup& s) {
  switch (s.kind) {
    case SensorKind::kImu:
      return ImuSample::kSize;
    case SensorKind::kGps:
      return GpsFix::kSize;
    case SensorKind::kWheelCan:
      return WheelSpeedFrame::kSize;
    case SensorKind::kCamera:
      return CameraFrameHeader::kSize + std::size_t{s.camera_width} * s.camera_height;
    case SensorKind::kEvents:
    case SensorKind::kUnknown:
      break;
  }
  return 0;
}

std::unique_ptr<SignalGenerator> make_generator(const StreamSetup& s, const VehicleProfile& v,
                                                std::uint64_t seed) {
  switch (s.kind) {
    case SensorKind::kImu:
      return make_imu_generator(v, seed);
    case SensorKind::kGps:
      return make_gps_generator(v, seed);
    case SensorKind::kWheelCan:
      return make_wheel_generator(v, seed);
    case SensorKind::kCamera:
      return make_camera_generator(s.camera_width, s.camera_height, seed);
    case SensorKind::kEvents:
    case SensorKind::kUnknown:
      break;
  }
  return nullptr;
}

}  // namespace

std::vector<StreamSetup> default_streams() {
  auto make = [](std::string name, SensorKind kind, double hz, std::size_t ring) {
    StreamSetup s;
    s.name = std::move(name);
    s.kind = kind;
    s.rate_hz = hz;
    s.ring_capacity = ring;
    return s;
  };
  return {make("imu", SensorKind::kImu, 200.0, 2048), make("gps", SensorKind::kGps, 10.0, 128),
          make("wheel_can", SensorKind::kWheelCan, 100.0, 1024),
          make("camera_front", SensorKind::kCamera, 30.0, 16)};
}

struct Recorder::Impl {
  struct Stream {
    StreamSetup setup;
    StreamId id = 0;
    std::unique_ptr<RecordRing> ring;
    std::unique_ptr<SensorProducer> producer;  // null for KITTI-fed streams
    std::uint64_t reported_lost = 0;
    std::int64_t last_ts = 0;
  };

  explicit Impl(RecorderConfig c) : config(std::move(c)) {}

  RecorderConfig config;
  std::vector<Stream> streams;
  std::unique_ptr<KittiOxtsReplay> kitti;
  std::unique_ptr<StorageWriter> writer;
  // Constructed once in create(); never null afterwards.
  std::unique_ptr<QualityMonitor> quality;
  std::unique_ptr<TriggerDetector> detector;
  std::unique_ptr<EventWindow> window;
  std::unique_ptr<PrivacyFilter> privacy;

  std::vector<std::jthread> producer_threads;
  std::jthread collector_thread;
  std::atomic<bool> producers_done{false};
  std::atomic<std::uint64_t> collected{0};
  Clock::time_point started_at;
  bool running = false;

  // Collector-owned state.
  std::vector<std::byte> scratch;
  std::vector<QualityEvent> events;
  std::array<StreamQualityCounters, kMaxStreams> quality_counters{};
  std::uint64_t event_seq = 0;
  std::uint64_t quality_event_count = 0;
  std::optional<Error> collector_error;

  Status emit_event(const QualityEvent& e) {
    accumulate_event(e, quality_counters);
    if (e.kind != EventKind::kTrigger) {
      ++quality_event_count;
    }
    std::array<std::byte, kEventPayloadSize> buf{};
    encode_event(e, buf);
    return writer->append({.timestamp_ns = e.timestamp_ns,
                           .seq = event_seq++,
                           .stream_id = kEventStreamId,
                           .flags = 0},
                          buf, to_ns(Clock::now()));
  }

  Status fire_trigger(TriggerReason reason, StreamId source, std::int64_t ts) {
    const auto post_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.trigger.post_window).count();
    MR_RETURN_IF_ERROR(emit_event({.stream = source,
                                   .kind = EventKind::kTrigger,
                                   .timestamp_ns = ts,
                                   .value = static_cast<std::int64_t>(reason),
                                   .expected = ts + post_ns}));
    return window->on_trigger(ts);
  }

  Status process(Stream& s, RingHeader header, MutableByteSpan payload) {
    const std::int64_t now = to_ns(Clock::now());
    s.last_ts = header.meta.timestamp_ns;

    events.clear();
    quality->observe(header.meta, now, events);

    if (privacy->apply(s.setup.kind, payload)) {
      header.meta.flags |= record_flags::kPrivacyFiltered;
    }

    std::optional<TriggerReason> reason;
    if (config.mode == RecordMode::kTriggered) {
      MR_RETURN_IF_ERROR(window->on_record(header.meta, payload, header.enqueue_ns));
      reason = detector->observe(s.setup.kind, header.meta, payload);
    } else {
      MR_RETURN_IF_ERROR(writer->append(header.meta, payload, header.enqueue_ns));
    }

    for (const QualityEvent& e : events) {
      MR_RETURN_IF_ERROR(emit_event(e));
      if (config.mode == RecordMode::kTriggered && !reason) {
        reason = detector->observe(e);
      }
    }
    if (reason) {
      MR_RETURN_IF_ERROR(fire_trigger(*reason, s.id, header.meta.timestamp_ns));
    }
    collected.fetch_add(1, std::memory_order_relaxed);
    return ok_status();
  }

  // Periodic work: stale detection, drop accounting, sealing old chunks.
  Status housekeeping(Clock::time_point now) {
    events.clear();
    quality->check_stale(to_ns(now), events);
    for (const QualityEvent& e : events) {
      MR_RETURN_IF_ERROR(emit_event(e));
      if (config.mode == RecordMode::kTriggered) {
        if (const auto reason = detector->observe(e)) {
          MR_RETURN_IF_ERROR(fire_trigger(*reason, e.stream, e.timestamp_ns));
        }
      }
    }
    MR_RETURN_IF_ERROR(report_drops());
    return writer->seal_aged(now);
  }

  Status report_drops() {
    for (Stream& s : streams) {
      const std::uint64_t lost = s.ring->counters().lost();
      if (lost > s.reported_lost) {
        // Before the first record of a stream was collected there is no
        // sensor timestamp yet; fall back to the clock the sensors use.
        const std::int64_t ts = s.last_ts != 0 ? s.last_ts : to_ns(Clock::now());
        MR_RETURN_IF_ERROR(emit_event({.stream = s.id,
                                       .kind = EventKind::kRecordsDropped,
                                       .timestamp_ns = ts,
                                       .value = static_cast<std::int64_t>(lost - s.reported_lost),
                                       .expected = 0}));
        s.reported_lost = lost;
      }
    }
    return ok_status();
  }

  void collector_main() {
    std::size_t len = 0;
    RingHeader header;
    auto last_housekeeping = Clock::now();
    for (;;) {
      // Read the flag before draining: if producers were already done, an
      // empty pass below means the rings are empty for good.
      const bool finishing = producers_done.load(std::memory_order_acquire);
      std::size_t popped = 0;
      for (Stream& s : streams) {
        for (int n = 0; n < kPopBatch; ++n) {
          // Copy the record out and release the slot immediately; the
          // producer never waits on quality checks, compression or I/O.
          const bool got = s.ring->try_pop([&](const RingHeader& h, ByteSpan p) noexcept {
            header = h;
            len = p.size();
            std::memcpy(scratch.data(), p.data(), p.size());
          });
          if (!got) {
            break;
          }
          ++popped;
          if (auto st = process(s, header, MutableByteSpan(scratch.data(), len)); !st) {
            collector_error = std::move(st).error();
            return;
          }
        }
      }
      const auto now = Clock::now();
      if (now - last_housekeeping >= kHousekeepingPeriod) {
        last_housekeeping = now;
        if (auto st = housekeeping(now); !st) {
          collector_error = std::move(st).error();
          return;
        }
      }
      if (popped == 0) {
        if (finishing) {
          break;
        }
        std::this_thread::sleep_for(kIdleSleep);
      }
    }
    if (auto st = report_drops(); !st) {
      collector_error = std::move(st).error();
    }
  }

  void stop_threads() {
    for (auto& t : producer_threads) {
      t.request_stop();
    }
    for (Stream& s : streams) {
      s.ring->close();  // wakes producers blocked on a full ring
    }
    producer_threads.clear();  // joins
    producers_done.store(true, std::memory_order_release);
    if (collector_thread.joinable()) {
      collector_thread.join();
    }
  }
};

Recorder::Recorder(PrivateTag /*tag*/, std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Recorder::~Recorder() {
  if (impl_ && impl_->running) {
    impl_->stop_threads();
  }
}

std::uint64_t Recorder::collected_records() const noexcept {
  return impl_->collected.load(std::memory_order_relaxed);
}

Result<std::unique_ptr<Recorder>> Recorder::create(RecorderConfig config) {
  if (config.streams.empty()) {
    return make_error(Errc::kInvalidArgument, "no streams configured");
  }
  if (config.streams.size() >= kEventStreamId) {
    return make_error(Errc::kInvalidArgument, "too many streams");
  }
  auto impl = std::make_unique<Impl>(std::move(config));
  RecorderConfig& cfg = impl->config;

  std::optional<std::vector<OxtsSample>> kitti_samples;
  if (cfg.kitti_oxts_dir) {
    auto loaded = load_oxts_dir(*cfg.kitti_oxts_dir);
    if (!loaded) {
      return std::move(loaded).error();
    }
    kitti_samples = std::move(loaded).value();
    // Monitor and label the replayed streams with the dataset's real rate
    // (10 Hz for synced KITTI drives, ~100 Hz for unsynced ones).
    const auto& samples = *kitti_samples;
    if (samples.size() > 1 && samples.back().timestamp_ns > 0) {
      const double hz = static_cast<double>(samples.size() - 1) * 1e9 /
                        static_cast<double>(samples.back().timestamp_ns);
      bool imu_done = false;
      bool gps_done = false;
      for (StreamSetup& s : cfg.streams) {  // the first IMU and GPS streams are fed by KITTI
        if (s.kind == SensorKind::kImu && !imu_done) {
          s.rate_hz = hz;
          imu_done = true;
        } else if (s.kind == SensorKind::kGps && !gps_done) {
          s.rate_hz = hz;
          gps_done = true;
        }
      }
    }
  }

  std::vector<MonitoredStream> monitored;
  std::size_t max_payload = kEventPayloadSize;
  WriterOptions wopts = cfg.writer;
  wopts.path = cfg.output;
  wopts.streams.clear();
  for (std::size_t i = 0; i < cfg.streams.size(); ++i) {
    const StreamSetup& setup = cfg.streams[i];
    const std::size_t payload = max_payload_for(setup);
    if (payload == 0) {
      return make_error(Errc::kInvalidArgument, "unsupported sensor kind for stream " + setup.name);
    }
    if (!(setup.rate_hz > 0)) {
      return make_error(Errc::kInvalidArgument, "rate must be positive for stream " + setup.name);
    }
    Impl::Stream s;
    s.setup = setup;
    s.id = static_cast<StreamId>(i);
    s.ring = std::make_unique<RecordRing>(setup.ring_capacity, payload, setup.policy);
    max_payload = std::max(max_payload, payload);
    monitored.push_back({.id = s.id, .nominal_rate_hz = setup.rate_hz});
    wopts.streams.push_back(
        {.id = s.id, .kind = setup.kind, .nominal_rate_hz = setup.rate_hz, .name = setup.name});
    impl->streams.push_back(std::move(s));
  }
  wopts.streams.push_back({.id = kEventStreamId,
                           .kind = SensorKind::kEvents,
                           .nominal_rate_hz = 0.0,
                           .name = "_events"});

  // Producers.
  RecordRing* kitti_imu = nullptr;
  RecordRing* kitti_gps = nullptr;
  StreamId kitti_imu_id = 0;
  StreamId kitti_gps_id = 0;
  for (Impl::Stream& s : impl->streams) {
    const bool from_kitti =
        kitti_samples && ((s.setup.kind == SensorKind::kImu && kitti_imu == nullptr) ||
                          (s.setup.kind == SensorKind::kGps && kitti_gps == nullptr));
    if (from_kitti) {
      if (s.setup.kind == SensorKind::kImu) {
        kitti_imu = s.ring.get();
        kitti_imu_id = s.id;
      } else {
        kitti_gps = s.ring.get();
        kitti_gps_id = s.id;
      }
      continue;
    }
    SensorConfig sc;
    sc.stream = s.id;
    sc.rate_hz = s.setup.rate_hz;
    sc.faults = s.setup.faults;
    sc.pacing = cfg.pacing;
    sc.rate_scale = cfg.rate_scale;
    sc.seed = cfg.seed * 1000 + s.id;
    s.producer = std::make_unique<SensorProducer>(sc, make_generator(s.setup, cfg.vehicle, sc.seed),
                                                  *s.ring);
  }
  if (kitti_samples) {
    if (kitti_imu == nullptr || kitti_gps == nullptr) {
      return make_error(Errc::kInvalidArgument, "KITTI replay needs one imu and one gps stream");
    }
    impl->kitti = std::make_unique<KittiOxtsReplay>(
        KittiReplayConfig{.imu_stream = kitti_imu_id,
                          .gps_stream = kitti_gps_id,
                          .rate_scale = cfg.rate_scale,
                          .unpaced = cfg.pacing == Pacing::kUnpaced,
                          .loop = cfg.kitti_loop},
        std::move(*kitti_samples), *kitti_imu, *kitti_gps);
  }

  auto writer = StorageWriter::open(std::move(wopts));
  if (!writer) {
    return std::move(writer).error();
  }
  impl->writer = std::move(writer).value();

  impl->scratch.resize(max_payload);
  impl->quality = std::make_unique<QualityMonitor>(cfg.quality, monitored);
  impl->detector = std::make_unique<TriggerDetector>(cfg.trigger);
  impl->privacy = std::make_unique<PrivacyFilter>(cfg.privacy);
  StorageWriter* w = impl->writer.get();
  impl->window = std::make_unique<EventWindow>(
      cfg.trigger,
      [w](const RecordMeta& m, ByteSpan p, std::int64_t e) { return w->append(m, p, e); });
  return std::make_unique<Recorder>(PrivateTag{}, std::move(impl));
}

Status Recorder::start() {
  Impl& s = *impl_;
  if (s.running) {
    return make_error(Errc::kInvalidArgument, "recorder already started");
  }
  s.running = true;
  // Give the threads a moment to spin up before the first sample deadline.
  s.started_at = Clock::now() + std::chrono::milliseconds(5);
  Impl* raw = impl_.get();
  s.collector_thread = std::jthread([raw] { raw->collector_main(); });
  for (const Impl::Stream& st : s.streams) {
    if (st.producer) {
      SensorProducer* p = st.producer.get();
      const auto start = s.started_at;
      s.producer_threads.emplace_back(
          [p, start](const std::stop_token& stop) { p->run(stop, start); });
    }
  }
  if (s.kitti) {
    KittiOxtsReplay* k = s.kitti.get();
    const auto start = s.started_at;
    s.producer_threads.emplace_back(
        [k, start](const std::stop_token& stop) { k->run(stop, start); });
  }
  return ok_status();
}

Result<RecorderReport> Recorder::stop() {
  Impl& s = *impl_;
  if (!s.running) {
    return make_error(Errc::kInvalidArgument, "recorder not running");
  }
  s.stop_threads();
  s.running = false;
  const double elapsed = std::chrono::duration<double>(Clock::now() - s.started_at).count();
  if (s.collector_error) {
    return *s.collector_error;
  }
  for (const Impl::Stream& st : s.streams) {
    s.writer->set_quality_counters(st.id, s.quality_counters[st.id]);
  }
  auto summary = s.writer->close();
  if (!summary) {
    return std::move(summary).error();
  }

  RecorderReport report;
  report.write = std::move(summary).value();
  report.elapsed_s = elapsed;
  report.collected_records = s.collected.load();
  report.quality_events = s.quality_event_count;
  report.triggers = s.window->triggers();
  report.pre_trigger_overflow = s.window->overflow_evictions();
  for (const Impl::Stream& st : s.streams) {
    StreamReport r;
    r.name = st.setup.name;
    r.id = st.id;
    r.kind = st.setup.kind;
    r.policy = st.setup.policy;
    r.ring = st.ring->counters();
    if (st.producer) {
      r.sensor = st.producer->counters();
    } else if (s.kitti) {
      r.sensor.emitted = s.kitti->emitted();
    }
    report.streams.push_back(std::move(r));
  }
  return report;
}

}  // namespace mr
