#include <gtest/gtest.h>

#include <vector>

#include <mr/sensor_types.hpp>
#include <mr/sensors.hpp>
#include <mr/trigger.hpp>

namespace {

constexpr std::int64_t kMs = 1'000'000;

std::vector<std::byte> wheel_payload(double speed_mps) {
  mr::WheelSpeedFrame f;
  for (auto& s : f.speed_centi_kph) {
    s = static_cast<std::uint16_t>(speed_mps * 360.0);
  }
  std::vector<std::byte> out(mr::WheelSpeedFrame::kSize);
  mr::encode(f, out);
  return out;
}

std::vector<std::byte> imu_payload(double ax) {
  std::vector<std::byte> out(mr::ImuSample::kSize);
  mr::encode(mr::ImuSample{.accel_x = ax, .accel_z = 9.81}, out);
  return out;
}

TEST(TriggerDetector, FiresOnWheelSpeedHardBrakeOnce) {
  mr::TriggerConfig cfg;
  mr::TriggerDetector det(cfg);
  const mr::VehicleProfile profile;  // brake 20..22 s
  std::vector<std::int64_t> fired;
  for (int i = 0; i < 3000; ++i) {  // 30 s at 100 Hz
    const double t = i * 0.01;
    const auto p = wheel_payload(mr::vehicle_state(profile, t).speed_mps);
    const mr::RecordMeta meta{.timestamp_ns = static_cast<std::int64_t>(i) * 10 * kMs};
    if (det.observe(mr::SensorKind::kWheelCan, meta, p)) {
      fired.push_back(meta.timestamp_ns);
    }
  }
  ASSERT_EQ(fired.size(), 1U) << "refractory period should suppress repeats";
  EXPECT_GE(fired[0], 20'000 * kMs);
  EXPECT_LE(fired[0], 20'200 * kMs);
}

TEST(TriggerDetector, ImuDetectionIsSmoothed) {
  mr::TriggerConfig cfg;
  mr::TriggerDetector det(cfg);
  // A single noisy spike must not trigger.
  EXPECT_FALSE(det.observe(mr::SensorKind::kImu, {.timestamp_ns = 0}, imu_payload(0.0)));
  EXPECT_FALSE(det.observe(mr::SensorKind::kImu, {.timestamp_ns = 5 * kMs}, imu_payload(-9.0)));
  EXPECT_FALSE(det.observe(mr::SensorKind::kImu, {.timestamp_ns = 10 * kMs}, imu_payload(0.0)));
  // Sustained braking does.
  std::optional<mr::TriggerReason> r;
  for (int i = 3; i < 40 && !r; ++i) {
    r = det.observe(mr::SensorKind::kImu, {.timestamp_ns = i * 5 * kMs}, imu_payload(-7.0));
  }
  ASSERT_TRUE(r);
  EXPECT_EQ(*r, mr::TriggerReason::kHardBrakeImu);
}

TEST(TriggerDetector, QualityFaultTriggerIsOptIn) {
  mr::TriggerConfig cfg;
  const mr::QualityEvent gap{.stream = 0, .kind = mr::EventKind::kSeqGap, .timestamp_ns = 1, .value = 2};
  EXPECT_FALSE(mr::TriggerDetector(cfg).observe(gap));
  cfg.on_quality_fault = true;
  EXPECT_EQ(mr::TriggerDetector(cfg).observe(gap), mr::TriggerReason::kQualityFault);
}

class EventWindowTest : public ::testing::Test {
 protected:
  EventWindowTest() {
    cfg_.pre_window = std::chrono::milliseconds(1000);
    cfg_.post_window = std::chrono::milliseconds(500);
  }
  mr::EventWindow make() {
    return mr::EventWindow(cfg_, [this](const mr::RecordMeta& m, mr::ByteSpan p, std::int64_t) {
      persisted_.push_back(m);
      EXPECT_EQ(p.size(), 8U);
      return mr::ok_status();
    });
  }
  // One record every 10 ms from `from_ms` (inclusive) to `to_ms` (exclusive).
  void feed(mr::EventWindow& w, int from_ms, int to_ms) {
    const std::vector<std::byte> payload(8);
    for (int t = from_ms; t < to_ms; t += 10) {
      ASSERT_TRUE(w.on_record({.timestamp_ns = t * kMs, .seq = static_cast<std::uint64_t>(t)}, payload, 0));
    }
  }
  mr::TriggerConfig cfg_;
  std::vector<mr::RecordMeta> persisted_;
};

TEST_F(EventWindowTest, NothingPersistedWithoutTrigger) {
  auto w = make();
  feed(w, 0, 5000);
  EXPECT_TRUE(persisted_.empty());
  // Rolling buffer only holds about the pre-window.
  EXPECT_LE(w.buffered_records(), 101U);
}

TEST_F(EventWindowTest, PersistsPreAndPostWindowAroundTrigger) {
  auto w = make();
  feed(w, 0, 3000);
  ASSERT_TRUE(w.on_trigger(3000 * kMs));
  feed(w, 3000, 5000);
  ASSERT_FALSE(persisted_.empty());
  // Pre window: [2000, 3000) flagged as pre-trigger; post: [3000, 3500].
  EXPECT_EQ(persisted_.front().timestamp_ns, 2000 * kMs);
  EXPECT_EQ(persisted_.back().timestamp_ns, 3500 * kMs);
  EXPECT_EQ(persisted_.size(), 100U + 51U);
  for (const auto& m : persisted_) {
    const bool pre = (m.flags & mr::record_flags::kPreTrigger) != 0;
    EXPECT_EQ(pre, m.timestamp_ns < 3000 * kMs);
  }
}

TEST_F(EventWindowTest, RetriggerExtendsWindow) {
  auto w = make();
  feed(w, 0, 1000);
  ASSERT_TRUE(w.on_trigger(1000 * kMs));
  feed(w, 1000, 1300);
  ASSERT_TRUE(w.on_trigger(1300 * kMs));  // extends to 1800
  feed(w, 1300, 3000);
  EXPECT_EQ(persisted_.back().timestamp_ns, 1800 * kMs);
  EXPECT_EQ(w.triggers(), 2U);
}

TEST_F(EventWindowTest, BufferIsBoundedInBytes) {
  cfg_.max_buffer_bytes = 80;  // ten 8-byte records
  auto w = make();
  feed(w, 0, 500);
  EXPECT_EQ(w.buffered_records(), 10U);
  EXPECT_EQ(w.overflow_evictions(), 40U);
}

}  // namespace
