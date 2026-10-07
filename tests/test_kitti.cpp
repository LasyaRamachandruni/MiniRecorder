#include <gtest/gtest.h>

#include <format>
#include <fstream>
#include <thread>

#include <mr/kitti.hpp>
#include <mr/reader.hpp>
#include <mr/recorder.hpp>

#include "test_util.hpp"

namespace {

using mr::test::TempDir;

// A SYNTHETIC OXTS line in the KITTI layout (30 fields). Values are made up
// but in realistic ranges: Karlsruhe coordinates, 10 m/s forward.
std::string oxts_line(double lat, double af) {
  return std::format(
      "{:.9f} 8.4318 116.4 0.03 -0.01 1.57 0.1 9.9 10.0 0.01 0.0 {:.4f} 0.1 9.81 {:.4f} 0.05 9.8 "
      "0.001 0.002 0.01 0.0011 0.0021 0.0101 0.5 0.03 4 10 4 4 0",
      lat, af, af);
}

void write_fixture(const std::filesystem::path& dir, int n) {
  std::filesystem::create_directories(dir / "data");
  std::ofstream ts(dir / "timestamps.txt");
  for (int i = 0; i < n; ++i) {
    ts << std::format("2011-09-26 13:02:{:02}.{:09}\n", 25 + i / 10, (i % 10) * 100'000'000);
    std::ofstream data(dir / "data" / std::format("{:010}.txt", i));
    data << oxts_line(49.0 + i * 1e-5, -0.5 * i) << "\n";
  }
}

TEST(Kitti, ParsesTimestamp) {
  auto t = mr::parse_kitti_timestamp("2011-09-26 13:02:25.964389445");
  ASSERT_TRUE(t);
  EXPECT_EQ(*t, ((13LL * 60 + 2) * 60 + 25) * 1'000'000'000LL + 964'389'445LL);
  auto short_frac = mr::parse_kitti_timestamp("2011-09-26 00:00:01.5");
  ASSERT_TRUE(short_frac);
  EXPECT_EQ(*short_frac, 1'500'000'000LL);
  EXPECT_FALSE(mr::parse_kitti_timestamp("13:02:25"));
  EXPECT_FALSE(mr::parse_kitti_timestamp("2011-09-26 aa:02:25.1"));
}

TEST(Kitti, ParsesOxtsLine) {
  auto s = mr::parse_oxts_line(oxts_line(49.015, -2.5));
  ASSERT_TRUE(s) << s.error().describe();
  EXPECT_DOUBLE_EQ(s->gps.latitude_deg, 49.015);
  EXPECT_DOUBLE_EQ(s->gps.longitude_deg, 8.4318);
  EXPECT_NEAR(s->gps.heading_deg, 89.954, 0.01);
  EXPECT_FLOAT_EQ(s->gps.speed_mps, 10.0F);
  EXPECT_EQ(s->gps.satellites, 10);
  EXPECT_DOUBLE_EQ(s->imu.accel_x, -2.5);
  EXPECT_DOUBLE_EQ(s->imu.gyro_z, 0.0101);
  EXPECT_FALSE(mr::parse_oxts_line("1 2 3"));
  EXPECT_FALSE(mr::parse_oxts_line(oxts_line(1, 1) + " 99"));
  EXPECT_FALSE(mr::parse_oxts_line("x " + oxts_line(1, 1).substr(2)));
}

TEST(Kitti, LoadsDirectoryAndRecordsIt) {
  TempDir dir;
  const auto oxts = dir.file("oxts");
  write_fixture(oxts, 20);
  auto samples = mr::load_oxts_dir(oxts);
  ASSERT_TRUE(samples) << samples.error().describe();
  ASSERT_EQ(samples->size(), 20U);
  EXPECT_EQ(samples->front().timestamp_ns, 0);
  EXPECT_EQ(samples->back().timestamp_ns, 1'900'000'000);

  mr::RecorderConfig cfg;
  cfg.output = dir.file("kitti.mrlog");
  cfg.streams = mr::default_streams();
  cfg.streams.resize(2);  // imu + gps only
  cfg.streams[0].rate_hz = 10.0;
  cfg.kitti_oxts_dir = oxts;
  cfg.kitti_loop = false;
  cfg.pacing = mr::Pacing::kUnpaced;
  auto rec = mr::Recorder::create(cfg);
  ASSERT_TRUE(rec) << rec.error().describe();
  ASSERT_TRUE((*rec)->start());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto report = (*rec)->stop();
  ASSERT_TRUE(report) << report.error().describe();

  auto reader = mr::Reader::open(cfg.output);
  ASSERT_TRUE(reader);
  EXPECT_EQ(reader->stream_stats(0)->records, 20U);
  EXPECT_EQ(reader->stream_stats(1)->records, 20U);
  mr::ReadOptions only_gps;
  only_gps.stream_mask = 1U << 1;
  auto it = reader->messages(only_gps);
  mr::Message m;
  ASSERT_TRUE(it.next(m).value());
  const auto fix = mr::decode_gps(m.payload);
  ASSERT_TRUE(fix);
  EXPECT_DOUBLE_EQ(fix->latitude_deg, 49.0);
}

TEST(Kitti, MissingDirectoryIsAnError) {
  EXPECT_EQ(mr::load_oxts_dir("/nonexistent/oxts").error().code, mr::Errc::kNotFound);
}

}  // namespace
