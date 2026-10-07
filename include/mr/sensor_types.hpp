#pragma once

// Payload layouts of the simulated sensors (all little-endian).
//
//   IMU        48 B : f64 ax, ay, az [m/s^2], f64 gx, gy, gz [rad/s]
//   GPS        40 B : f64 lat, lon [deg], f64 alt [m], f32 speed [m/s],
//                     f32 heading [deg], u8 fix, u8 sats, 6 B reserved
//   Wheel/CAN  16 B : u32 can_id, u8 dlc, 3 B pad, 8 B data =
//                     4 x u16 wheel speed [0.01 km/h] (FL, FR, RL, RR)
//   Camera  16 B + W*H : u16 width, u16 height, u8 format (0 = GRAY8),
//                     3 B pad, u64 frame id, then W*H pixels row-major

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <mr/bytes.hpp>

namespace mr {

struct ImuSample {
  double accel_x = 0, accel_y = 0, accel_z = 0;
  double gyro_x = 0, gyro_y = 0, gyro_z = 0;
  static constexpr std::size_t kSize = 48;
};

struct GpsFix {
  double latitude_deg = 0, longitude_deg = 0, altitude_m = 0;
  float speed_mps = 0, heading_deg = 0;
  std::uint8_t fix_type = 0, satellites = 0;
  static constexpr std::size_t kSize = 40;
};

struct WheelSpeedFrame {
  static constexpr std::uint32_t kCanId = 0x1A0;
  std::array<std::uint16_t, 4> speed_centi_kph{};  // FL, FR, RL, RR
  static constexpr std::size_t kSize = 16;

  [[nodiscard]] double mean_speed_mps() const noexcept {
    double sum = 0;
    for (const std::uint16_t s : speed_centi_kph) {
      sum += s;
    }
    return sum / 4.0 / 100.0 / 3.6;
  }
};

struct CameraFrameHeader {
  std::uint16_t width = 0, height = 0;
  std::uint8_t format = 0;  // 0 = GRAY8
  std::uint64_t frame_id = 0;
  static constexpr std::size_t kSize = 16;
};

void encode(const ImuSample& s, MutableByteSpan out) noexcept;
void encode(const GpsFix& s, MutableByteSpan out) noexcept;
void encode(const WheelSpeedFrame& s, MutableByteSpan out) noexcept;
void encode(const CameraFrameHeader& s, MutableByteSpan out) noexcept;

[[nodiscard]] std::optional<ImuSample> decode_imu(ByteSpan payload) noexcept;
[[nodiscard]] std::optional<GpsFix> decode_gps(ByteSpan payload) noexcept;
[[nodiscard]] std::optional<WheelSpeedFrame> decode_wheel(ByteSpan payload) noexcept;
[[nodiscard]] std::optional<CameraFrameHeader> decode_camera_header(ByteSpan payload) noexcept;

// GPS fields are stored at fixed offsets so the privacy filter can rewrite
// them in place.
inline constexpr std::size_t kGpsLatitudeOffset = 0;
inline constexpr std::size_t kGpsLongitudeOffset = 8;

}  // namespace mr
