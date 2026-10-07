#include <gtest/gtest.h>

#include <cmath>
#include <thread>

#include <mr/reader.hpp>
#include <mr/recorder.hpp>

#include "test_util.hpp"

namespace {

using mr::test::TempDir;

mr::RecorderConfig small_config(const std::filesystem::path& out) {
  mr::RecorderConfig cfg;
  cfg.output = out;
  cfg.streams = mr::default_streams();
  for (auto& s : cfg.streams) {
    if (s.kind == mr::SensorKind::kCamera) {
      s.camera_width = 160;
      s.camera_height = 120;
    }
  }
  cfg.writer.chunk_target_bytes = 64 * 1024;
  cfg.writer.chunk_max_age = std::chrono::milliseconds(100);
  cfg.writer.fsync = {mr::FsyncMode::kOnClose, std::chrono::milliseconds(0)};
  return cfg;
}

mr::RecorderReport run_for(mr::RecorderConfig cfg, std::chrono::milliseconds d) {
  auto rec = mr::Recorder::create(std::move(cfg));
  EXPECT_TRUE(rec) << rec.error().describe();
  EXPECT_TRUE((*rec)->start());
  std::this_thread::sleep_for(d);
  auto report = (*rec)->stop();
  EXPECT_TRUE(report) << report.error().describe();
  return std::move(report).value();
}

TEST(Recorder, ContinuousModeRecordsAllStreamsAtNominalRates) {
  TempDir dir;
  const auto path = dir.file("cont.mrlog");
  const auto report = run_for(small_config(path), std::chrono::milliseconds(1500));

  auto reader = mr::Reader::open(path);
  ASSERT_TRUE(reader) << reader.error().describe();
  const std::array<double, 4> rates{200, 10, 100, 30};
  for (mr::StreamId id = 0; id < 4; ++id) {
    const auto* st = reader->stream_stats(id);
    ASSERT_NE(st, nullptr);
    const double expected = rates[id] * report.elapsed_s;
    // Generous tolerance: shared CI machines schedule threads late.
    EXPECT_GT(static_cast<double>(st->records), expected * 0.7) << "stream " << id;
    EXPECT_LT(static_cast<double>(st->records), expected * 1.1 + 2) << "stream " << id;
    EXPECT_EQ(st->dropped, 0U);
  }
  // Everything the collector saw was written, and the file agrees.
  EXPECT_EQ(report.write.metrics.records_written,
            report.collected_records + reader->stream_stats(mr::kEventStreamId)->records);
}

TEST(Recorder, InjectedFaultsAreDetectedAndCounted) {
  TempDir dir;
  const auto path = dir.file("faults.mrlog");
  auto cfg = small_config(path);
  for (auto& s : cfg.streams) {
    if (s.kind == mr::SensorKind::kImu) {
      s.faults.gap_probability = 0.02;
      s.faults.regression_probability = 0.02;
    }
  }
  const auto report = run_for(cfg, std::chrono::milliseconds(1500));
  auto reader = mr::Reader::open(path);
  ASSERT_TRUE(reader);
  const auto* imu = reader->stream_stats(0);
  ASSERT_NE(imu, nullptr);
  EXPECT_GT(imu->seq_gaps, 0U);
  EXPECT_GT(imu->ts_regressions, 0U);
  EXPECT_EQ(imu->seq_gaps, report.streams[0].sensor.gaps);
  EXPECT_EQ(imu->ts_regressions, report.streams[0].sensor.regressions);
  // Clean streams stay clean.
  EXPECT_EQ(reader->stream_stats(1)->seq_gaps, 0U);
  EXPECT_GT(reader->stream_stats(mr::kEventStreamId)->records, 0U);
}

TEST(Recorder, StallIsReportedAsStaleStream) {
  TempDir dir;
  const auto path = dir.file("stall.mrlog");
  auto cfg = small_config(path);
  for (auto& s : cfg.streams) {
    if (s.kind == mr::SensorKind::kWheelCan) {
      s.faults.stall_probability = 0.01;
      s.faults.stall_duration = std::chrono::milliseconds(300);
    }
  }
  run_for(cfg, std::chrono::milliseconds(2000));
  auto reader = mr::Reader::open(path);
  ASSERT_TRUE(reader);
  EXPECT_GT(reader->stream_stats(2)->stale_events, 0U);
}

TEST(Recorder, TriggeredModeOnlyKeepsWindowsAroundHardBrakes) {
  TempDir dir;
  const auto path = dir.file("trig.mrlog");
  auto cfg = small_config(path);
  cfg.mode = mr::RecordMode::kTriggered;
  cfg.vehicle.cycle_s = 12.0;
  cfg.vehicle.brake_start_s = 1.0;  // brake at 1.0-3.0 s
  cfg.trigger.pre_window = std::chrono::milliseconds(500);
  cfg.trigger.post_window = std::chrono::milliseconds(300);
  const auto report = run_for(cfg, std::chrono::milliseconds(2500));
  EXPECT_EQ(report.triggers, 1U);

  auto reader = mr::Reader::open(path);
  ASSERT_TRUE(reader);
  // Find the trigger event and check every sensor record is inside its window.
  std::optional<std::int64_t> trigger_ts;
  auto it = reader->messages();
  mr::Message m;
  std::vector<mr::RecordMeta> sensor_records;
  while (it.next(m).value()) {
    if (m.meta.stream_id == mr::kEventStreamId) {
      const auto e = mr::decode_event(m.payload);
      ASSERT_TRUE(e);
      if (e->kind == mr::EventKind::kTrigger) {
        trigger_ts = e->timestamp_ns;
      }
    } else {
      sensor_records.push_back(m.meta);
    }
  }
  ASSERT_TRUE(trigger_ts);
  ASSERT_FALSE(sensor_records.empty());
  bool saw_pre = false;
  for (const auto& r : sensor_records) {
    EXPECT_GE(r.timestamp_ns, *trigger_ts - 500'000'000);
    EXPECT_LE(r.timestamp_ns, *trigger_ts + 300'000'000);
    saw_pre = saw_pre || (r.flags & mr::record_flags::kPreTrigger) != 0;
  }
  EXPECT_TRUE(saw_pre);
  // Far fewer records than continuous recording of the same 2.5 s.
  EXPECT_LT(sensor_records.size(), report.collected_records / 2);
}

TEST(Recorder, PrivacyFilterAppliedToPersistedData) {
  TempDir dir;
  const auto path = dir.file("priv.mrlog");
  auto cfg = small_config(path);
  cfg.privacy.gps_decimals = 3;
  cfg.privacy.camera_rois.push_back({0, 0, 160, 30});
  run_for(cfg, std::chrono::milliseconds(800));
  auto reader = mr::Reader::open(path);
  ASSERT_TRUE(reader);
  auto it = reader->messages();
  mr::Message m;
  int gps = 0;
  int frames = 0;
  while (it.next(m).value()) {
    if (m.meta.stream_id == 1) {
      const auto fix = mr::decode_gps(m.payload);
      ASSERT_TRUE(fix);
      EXPECT_DOUBLE_EQ(fix->latitude_deg, std::round(fix->latitude_deg * 1000) / 1000);
      EXPECT_NE(m.meta.flags & mr::record_flags::kPrivacyFiltered, 0);
      ++gps;
    } else if (m.meta.stream_id == 3) {
      const mr::ByteSpan pixels = m.payload.subspan(mr::CameraFrameHeader::kSize);
      for (std::size_t i = 0; i < 160 * 30; ++i) {
        ASSERT_EQ(pixels[i], std::byte{0});
      }
      ++frames;
    }
  }
  EXPECT_GT(gps, 3);
  EXPECT_GT(frames, 10);
}

TEST(Recorder, OverloadWithDropPoliciesCountsLosses) {
  TempDir dir;
  const auto path = dir.file("overload.mrlog");
  auto cfg = small_config(path);
  cfg.pacing = mr::Pacing::kUnpaced;
  cfg.writer.zstd_level = 9;
  cfg.writer.max_inflight_chunks = 2;
  cfg.writer.compression_threads = 1;
  for (auto& s : cfg.streams) {
    s.ring_capacity = 8;
    s.policy = s.kind == mr::SensorKind::kCamera ? mr::OverflowPolicy::kDropOldest
                                                 : mr::OverflowPolicy::kDropNewest;
  }
  const auto report = run_for(cfg, std::chrono::milliseconds(700));
  std::uint64_t lost = 0;
  for (const auto& s : report.streams) {
    lost += s.ring.lost();
    // Produced = persisted + lost, exactly.
    const auto& st = report.write.footer.streams[s.id];
    EXPECT_EQ(s.sensor.emitted, st.records + s.ring.lost()) << s.name;
    EXPECT_EQ(st.dropped, s.ring.lost()) << s.name;
  }
  EXPECT_GT(lost, 0U) << "unpaced producers into 8-slot rings must overflow";
}

}  // namespace
