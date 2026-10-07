#pragma once

// Privacy filter applied before anything is buffered or persisted:
//   * GPS latitude/longitude are rounded to a configurable number of decimals
//     (3 decimals is roughly 110 m north-south).
//   * Configurable camera regions of interest (e.g. a cabin-facing area or a
//     fixed region where license plates appear) are zeroed.
// The raw values never leave the collector.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <mr/bytes.hpp>
#include <mr/record.hpp>
#include <mr/result.hpp>

namespace mr {

struct Roi {
  std::uint16_t x = 0, y = 0, width = 0, height = 0;
};

// Parses "x,y,w,h".
[[nodiscard]] Result<Roi> parse_roi(std::string_view text);

struct PrivacyConfig {
  std::optional<int> gps_decimals;  // nullopt: GPS untouched
  std::vector<Roi> camera_rois;
};

class PrivacyFilter {
 public:
  explicit PrivacyFilter(PrivacyConfig config);

  // Rewrites `payload` in place. Returns true if the record was modified.
  bool apply(SensorKind kind, MutableByteSpan payload) const noexcept;

  [[nodiscard]] bool enabled() const noexcept {
    return config_.gps_decimals.has_value() || !config_.camera_rois.empty();
  }

  [[nodiscard]] static double coarsen(double degrees, int decimals) noexcept;

 private:
  PrivacyConfig config_;
};

}  // namespace mr
