#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>

#include <mr/privacy.hpp>
#include <mr/sensor_types.hpp>

namespace mr {

Result<Roi> parse_roi(std::string_view text) {
  std::array<std::uint16_t, 4> v{};
  std::size_t i = 0;
  const char* p = text.data();
  const char* end = text.data() + text.size();
  while (i < v.size()) {
    const auto [next, ec] = std::from_chars(p, end, v[i]);
    if (ec != std::errc()) {
      break;
    }
    ++i;
    p = next;
    if (i < v.size()) {
      if (p == end || *p != ',') {
        break;
      }
      ++p;
    }
  }
  if (i != v.size() || p != end) {
    return make_error(Errc::kInvalidArgument,
                      "ROI must be 'x,y,w,h', got '" + std::string(text) + "'");
  }
  return Roi{v[0], v[1], v[2], v[3]};
}

PrivacyFilter::PrivacyFilter(PrivacyConfig config) : config_(std::move(config)) {
  if (config_.gps_decimals) {
    config_.gps_decimals = std::clamp(*config_.gps_decimals, 0, 9);
  }
}

double PrivacyFilter::coarsen(double degrees, int decimals) noexcept {
  const double scale = std::pow(10.0, decimals);
  return std::round(degrees * scale) / scale;
}

bool PrivacyFilter::apply(SensorKind kind, MutableByteSpan payload) const noexcept {
  if (kind == SensorKind::kGps && config_.gps_decimals && payload.size() == GpsFix::kSize) {
    std::byte* p = payload.data();
    store_le_f64(p + kGpsLatitudeOffset,
                 coarsen(load_le_f64(p + kGpsLatitudeOffset), *config_.gps_decimals));
    store_le_f64(p + kGpsLongitudeOffset,
                 coarsen(load_le_f64(p + kGpsLongitudeOffset), *config_.gps_decimals));
    return true;
  }
  if (kind == SensorKind::kCamera && !config_.camera_rois.empty()) {
    const auto header = decode_camera_header(payload);
    if (!header || header->format != 0) {
      return false;
    }
    std::byte* pixels = payload.data() + CameraFrameHeader::kSize;
    bool changed = false;
    for (const Roi& roi : config_.camera_rois) {
      const std::size_t x0 = std::min<std::size_t>(roi.x, header->width);
      const std::size_t y0 = std::min<std::size_t>(roi.y, header->height);
      const std::size_t x1 = std::min<std::size_t>(std::size_t{roi.x} + roi.width, header->width);
      const std::size_t y1 = std::min<std::size_t>(std::size_t{roi.y} + roi.height, header->height);
      if (x1 <= x0 || y1 <= y0) {
        continue;
      }
      for (std::size_t y = y0; y < y1; ++y) {
        std::memset(pixels + y * header->width + x0, 0, x1 - x0);
      }
      changed = true;
    }
    return changed;
  }
  return false;
}

}  // namespace mr
