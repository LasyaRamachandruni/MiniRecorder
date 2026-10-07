#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include <mr/chunk.hpp>
#include <mr/sensors.hpp>

namespace {

using mr::ByteSpan;
using mr::MutableByteSpan;

TEST(VehicleModel, HardBrakePhaseHasStrongDeceleration) {
  const mr::VehicleProfile p;  // brake from 20 s to 22 s
  EXPECT_NEAR(mr::vehicle_state(p, 21.0).accel_mps2, -7.0, 1e-9);
  EXPECT_NEAR(mr::vehicle_state(p, 21.0).speed_mps, 8.0, 1e-9);
  EXPECT_GT(mr::vehicle_state(p, 10.0).accel_mps2, -1.0);
  EXPECT_GT(mr::vehicle_state(p, 25.0).accel_mps2, 0.0);
  // Periodic.
  EXPECT_NEAR(mr::vehicle_state(p, 51.0).speed_mps, mr::vehicle_state(p, 21.0).speed_mps, 1e-9);
  // Speed is continuous across phase boundaries.
  for (const double t : {20.0, 22.0, 29.0, 30.0}) {
    EXPECT_NEAR(mr::vehicle_state(p, t - 1e-6).speed_mps, mr::vehicle_state(p, t + 1e-6).speed_mps, 1e-3) << t;
  }
}

TEST(Generators, PayloadsDecode) {
  const mr::VehicleProfile p;
  auto imu = mr::make_imu_generator(p, 1);
  auto gps = mr::make_gps_generator(p, 2);
  auto wheel = mr::make_wheel_generator(p, 3);
  auto cam = mr::make_camera_generator(64, 48, 4);

  std::vector<std::byte> buf(imu->payload_size());
  imu->generate(21.0, 0, buf);
  auto s = mr::decode_imu(buf);
  ASSERT_TRUE(s);
  EXPECT_NEAR(s->accel_x, -7.0, 0.5);
  EXPECT_NEAR(s->accel_z, 9.81, 0.2);

  buf.resize(gps->payload_size());
  gps->generate(5.0, 0, buf);
  auto g = mr::decode_gps(buf);
  ASSERT_TRUE(g);
  EXPECT_NEAR(g->latitude_deg, 37.3352, 0.01);
  EXPECT_NEAR(g->longitude_deg, -121.8811, 0.01);
  EXPECT_EQ(g->fix_type, 3);

  buf.resize(wheel->payload_size());
  wheel->generate(10.0, 0, buf);
  auto w = mr::decode_wheel(buf);
  ASSERT_TRUE(w);
  EXPECT_NEAR(w->mean_speed_mps(), mr::vehicle_state(p, 10.0).speed_mps, 0.05);

  buf.resize(cam->payload_size());
  EXPECT_EQ(buf.size(), 16U + 64 * 48);
  cam->generate(0.0, 7, buf);
  auto h = mr::decode_camera_header(buf);
  ASSERT_TRUE(h);
  EXPECT_EQ(h->width, 64);
  EXPECT_EQ(h->height, 48);
  EXPECT_EQ(h->frame_id, 7U);
}

TEST(Generators, CameraFramesAreNeitherConstantNorRandom) {
  auto cam = mr::make_camera_generator(320, 240, 9);
  std::vector<std::byte> frames;
  std::vector<std::byte> one(cam->payload_size());
  for (std::uint64_t i = 0; i < 4; ++i) {
    cam->generate(0.0, i, one);
    frames.insert(frames.end(), one.begin(), one.end());
  }
  mr::ZstdCompressor c(3);
  std::vector<std::byte> out;
  ASSERT_TRUE(c.compress(frames, out));
  const double ratio = static_cast<double>(frames.size()) / static_cast<double>(out.size());
  // Real raw grayscale images typically compress 1.3-3x losslessly; a constant
  // fill would be >100x and random bytes ~1.0x.
  EXPECT_GT(ratio, 1.2);
  EXPECT_LT(ratio, 6.0);
}

TEST(SensorProducer, UnpacedProducerEmitsSequentialRecordsWithInjectedGaps) {
  mr::RecordRing ring(1024, 64, mr::OverflowPolicy::kBlock);
  mr::SensorConfig cfg;
  cfg.stream = 2;
  cfg.rate_hz = 100.0;
  cfg.pacing = mr::Pacing::kUnpaced;
  cfg.faults.gap_probability = 0.05;
  cfg.faults.regression_probability = 0.05;
  mr::SensorProducer producer(cfg, mr::make_imu_generator({}, 1), ring);

  std::jthread t([&](const std::stop_token& st) { producer.run(st, std::chrono::steady_clock::now()); });
  std::uint64_t popped = 0;
  std::uint64_t gaps = 0;
  std::uint64_t regressions = 0;
  std::uint64_t last_seq = 0;
  std::int64_t last_ts = 0;
  while (popped < 2000) {
    ring.try_pop([&](const mr::RingHeader& h, ByteSpan p) noexcept {
      EXPECT_EQ(h.meta.stream_id, 2);
      EXPECT_EQ(p.size(), mr::ImuSample::kSize);
      if (popped > 0 && h.meta.seq != last_seq + 1) {
        ++gaps;
      }
      if (popped > 0 && h.meta.timestamp_ns < last_ts) {
        ++regressions;
      }
      last_seq = h.meta.seq;
      last_ts = h.meta.timestamp_ns;
      ++popped;
    });
  }
  t.request_stop();
  ring.close();
  t.join();
  const auto c = producer.counters();
  EXPECT_GT(c.gaps, 0U);
  EXPECT_GT(gaps, 0U);
  EXPECT_GT(regressions, 0U);
  EXPECT_GE(c.emitted, 2000U);
}

TEST(SensorProducer, RealTimePacingHonoursRate) {
  mr::RecordRing ring(1024, 64, mr::OverflowPolicy::kDropNewest);
  mr::SensorConfig cfg;
  cfg.rate_hz = 200.0;
  mr::SensorProducer producer(cfg, mr::make_imu_generator({}, 1), ring);
  {
    std::jthread t([&](const std::stop_token& st) { producer.run(st, std::chrono::steady_clock::now()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  // ~100 samples in 0.5 s; generous bounds for a shared CI machine.
  EXPECT_GT(producer.counters().emitted, 70U);
  EXPECT_LT(producer.counters().emitted, 130U);
}

}  // namespace
