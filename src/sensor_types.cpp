#include <cstring>

#include <mr/sensor_types.hpp>

namespace mr {

void encode(const ImuSample& s, MutableByteSpan out) noexcept {
  std::byte* p = out.data();
  store_le_f64(p + 0, s.accel_x);
  store_le_f64(p + 8, s.accel_y);
  store_le_f64(p + 16, s.accel_z);
  store_le_f64(p + 24, s.gyro_x);
  store_le_f64(p + 32, s.gyro_y);
  store_le_f64(p + 40, s.gyro_z);
}

void encode(const GpsFix& s, MutableByteSpan out) noexcept {
  std::byte* p = out.data();
  store_le_f64(p + kGpsLatitudeOffset, s.latitude_deg);
  store_le_f64(p + kGpsLongitudeOffset, s.longitude_deg);
  store_le_f64(p + 16, s.altitude_m);
  store_le_f32(p + 24, s.speed_mps);
  store_le_f32(p + 28, s.heading_deg);
  p[32] = static_cast<std::byte>(s.fix_type);
  p[33] = static_cast<std::byte>(s.satellites);
  std::memset(p + 34, 0, 6);
}

void encode(const WheelSpeedFrame& s, MutableByteSpan out) noexcept {
  std::byte* p = out.data();
  store_le<std::uint32_t>(p + 0, WheelSpeedFrame::kCanId);
  p[4] = std::byte{8};  // DLC
  std::memset(p + 5, 0, 3);
  for (std::size_t i = 0; i < 4; ++i) {
    store_le<std::uint16_t>(p + 8 + 2 * i, s.speed_centi_kph[i]);
  }
}

void encode(const CameraFrameHeader& s, MutableByteSpan out) noexcept {
  std::byte* p = out.data();
  store_le<std::uint16_t>(p + 0, s.width);
  store_le<std::uint16_t>(p + 2, s.height);
  p[4] = static_cast<std::byte>(s.format);
  std::memset(p + 5, 0, 3);
  store_le<std::uint64_t>(p + 8, s.frame_id);
}

std::optional<ImuSample> decode_imu(ByteSpan payload) noexcept {
  if (payload.size() != ImuSample::kSize) {
    return std::nullopt;
  }
  const std::byte* p = payload.data();
  return ImuSample{load_le_f64(p),      load_le_f64(p + 8),  load_le_f64(p + 16),
                   load_le_f64(p + 24), load_le_f64(p + 32), load_le_f64(p + 40)};
}

std::optional<GpsFix> decode_gps(ByteSpan payload) noexcept {
  if (payload.size() != GpsFix::kSize) {
    return std::nullopt;
  }
  const std::byte* p = payload.data();
  GpsFix g;
  g.latitude_deg = load_le_f64(p + kGpsLatitudeOffset);
  g.longitude_deg = load_le_f64(p + kGpsLongitudeOffset);
  g.altitude_m = load_le_f64(p + 16);
  g.speed_mps = load_le_f32(p + 24);
  g.heading_deg = load_le_f32(p + 28);
  g.fix_type = std::to_integer<std::uint8_t>(p[32]);
  g.satellites = std::to_integer<std::uint8_t>(p[33]);
  return g;
}

std::optional<WheelSpeedFrame> decode_wheel(ByteSpan payload) noexcept {
  if (payload.size() != WheelSpeedFrame::kSize ||
      load_le<std::uint32_t>(payload.data()) != WheelSpeedFrame::kCanId) {
    return std::nullopt;
  }
  WheelSpeedFrame f;
  for (std::size_t i = 0; i < 4; ++i) {
    f.speed_centi_kph[i] = load_le<std::uint16_t>(payload.data() + 8 + 2 * i);
  }
  return f;
}

std::optional<CameraFrameHeader> decode_camera_header(ByteSpan payload) noexcept {
  if (payload.size() < CameraFrameHeader::kSize) {
    return std::nullopt;
  }
  const std::byte* p = payload.data();
  CameraFrameHeader h;
  h.width = load_le<std::uint16_t>(p);
  h.height = load_le<std::uint16_t>(p + 2);
  h.format = std::to_integer<std::uint8_t>(p[4]);
  h.frame_id = load_le<std::uint64_t>(p + 8);
  if (payload.size() != CameraFrameHeader::kSize + std::size_t{h.width} * h.height) {
    return std::nullopt;
  }
  return h;
}

}  // namespace mr
