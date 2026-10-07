#include <mr/record.hpp>

#include <array>
#include <utility>

namespace mr {
namespace {

constexpr std::array<std::pair<SensorKind, std::string_view>, 6> kKindNames{{
    {SensorKind::kUnknown, "unknown"},
    {SensorKind::kImu, "imu"},
    {SensorKind::kGps, "gps"},
    {SensorKind::kWheelCan, "wheel_can"},
    {SensorKind::kCamera, "camera"},
    {SensorKind::kEvents, "events"},
}};

}  // namespace

std::string_view to_string(SensorKind kind) noexcept {
  for (const auto& [k, name] : kKindNames) {
    if (k == kind) {
      return name;
    }
  }
  return "unknown";
}

std::optional<SensorKind> parse_sensor_kind(std::string_view text) noexcept {
  for (const auto& [k, name] : kKindNames) {
    if (name == text) {
      return k;
    }
  }
  return std::nullopt;
}

}  // namespace mr
