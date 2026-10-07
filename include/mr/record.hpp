#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace mr {

using StreamId = std::uint16_t;

// Stream ids index a 64-bit mask in chunk headers, so at most 64 streams.
inline constexpr std::size_t kMaxStreams = 64;
// Reserved stream carrying data-quality and trigger events.
inline constexpr StreamId kEventStreamId = 63;

enum class SensorKind : std::uint16_t {
  kUnknown = 0,
  kImu = 1,
  kGps = 2,
  kWheelCan = 3,
  kCamera = 4,
  kEvents = 5,
};

[[nodiscard]] std::string_view to_string(SensorKind kind) noexcept;
[[nodiscard]] std::optional<SensorKind> parse_sensor_kind(std::string_view text) noexcept;

// Per-record flags persisted with each record.
namespace record_flags {
inline constexpr std::uint16_t kPrivacyFiltered = 1U << 0;
inline constexpr std::uint16_t kPreTrigger = 1U << 1;
inline constexpr std::uint16_t kInjectedFault = 1U << 2;
}  // namespace record_flags

// Metadata stored with every record. timestamp_ns is the sensor timestamp on
// the vehicle's monotonic time base (CLOCK_MONOTONIC in this simulator).
struct RecordMeta {
  std::int64_t timestamp_ns = 0;
  std::uint64_t seq = 0;
  StreamId stream_id = 0;
  std::uint16_t flags = 0;
};

}  // namespace mr
