#include <gtest/gtest.h>

#include <vector>

#include <mr/privacy.hpp>
#include <mr/sensor_types.hpp>

namespace {

TEST(Privacy, CoarsensGpsInPlace) {
  mr::GpsFix fix{.latitude_deg = 37.335187, .longitude_deg = -121.881072, .altitude_m = 30.5,
                 .speed_mps = 12.5F, .heading_deg = 90.0F, .fix_type = 3, .satellites = 10};
  std::vector<std::byte> buf(mr::GpsFix::kSize);
  mr::encode(fix, buf);
  const mr::PrivacyFilter filter({.gps_decimals = 3, .camera_rois = {}});
  EXPECT_TRUE(filter.apply(mr::SensorKind::kGps, buf));
  const auto out = mr::decode_gps(buf);
  ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(out->latitude_deg, 37.335);
  EXPECT_DOUBLE_EQ(out->longitude_deg, -121.881);
  // Other fields untouched.
  EXPECT_DOUBLE_EQ(out->altitude_m, 30.5);
  EXPECT_FLOAT_EQ(out->speed_mps, 12.5F);
  EXPECT_EQ(out->satellites, 10);
}

TEST(Privacy, CoarsenRounding) {
  EXPECT_DOUBLE_EQ(mr::PrivacyFilter::coarsen(1.23456, 2), 1.23);
  EXPECT_DOUBLE_EQ(mr::PrivacyFilter::coarsen(1.235001, 2), 1.24);
  EXPECT_DOUBLE_EQ(mr::PrivacyFilter::coarsen(-0.5, 0), -1.0);
}

TEST(Privacy, ZeroesCameraRoiAndClampsToImage) {
  constexpr std::uint16_t kW = 16;
  constexpr std::uint16_t kH = 8;
  std::vector<std::byte> frame(mr::CameraFrameHeader::kSize + kW * kH, std::byte{200});
  mr::encode(mr::CameraFrameHeader{.width = kW, .height = kH, .format = 0, .frame_id = 1}, frame);
  // Second ROI extends past the right/bottom edge and must be clamped.
  const mr::PrivacyFilter filter({.gps_decimals = std::nullopt,
                                  .camera_rois = {{2, 1, 3, 2}, {14, 6, 100, 100}}});
  EXPECT_TRUE(filter.apply(mr::SensorKind::kCamera, frame));
  const std::byte* px = frame.data() + mr::CameraFrameHeader::kSize;
  int zeros = 0;
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const bool in_a = x >= 2 && x < 5 && y >= 1 && y < 3;
      const bool in_b = x >= 14 && y >= 6;
      const bool zero = px[y * kW + x] == std::byte{0};
      EXPECT_EQ(zero, in_a || in_b) << x << "," << y;
      zeros += zero ? 1 : 0;
    }
  }
  EXPECT_EQ(zeros, 6 + 4);
  // Header untouched.
  EXPECT_EQ(mr::decode_camera_header(frame)->frame_id, 1U);
}

TEST(Privacy, IgnoresMalformedPayloadsAndOtherKinds) {
  const mr::PrivacyFilter filter({.gps_decimals = 2, .camera_rois = {{0, 0, 4, 4}}});
  std::vector<std::byte> short_gps(10);
  EXPECT_FALSE(filter.apply(mr::SensorKind::kGps, short_gps));
  std::vector<std::byte> bad_camera(mr::CameraFrameHeader::kSize + 3);
  mr::encode(mr::CameraFrameHeader{.width = 4, .height = 4}, bad_camera);
  EXPECT_FALSE(filter.apply(mr::SensorKind::kCamera, bad_camera));
  std::vector<std::byte> imu(mr::ImuSample::kSize, std::byte{7});
  EXPECT_FALSE(filter.apply(mr::SensorKind::kImu, imu));
  EXPECT_EQ(imu[0], std::byte{7});
}

TEST(Privacy, ParseRoi) {
  auto r = mr::parse_roi("10,20,300,40");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->x, 10);
  EXPECT_EQ(r->height, 40);
  EXPECT_FALSE(mr::parse_roi("1,2,3"));
  EXPECT_FALSE(mr::parse_roi("1,2,3,4,5"));
  EXPECT_FALSE(mr::parse_roi("a,b,c,d"));
  EXPECT_FALSE(mr::parse_roi("1,2,3,70000"));
}

}  // namespace
