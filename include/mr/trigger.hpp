#pragma once

// Event-triggered recording.
//
// TriggerDetector looks for a hard brake in wheel-speed (CAN) or IMU data,
// or optionally reacts to data-quality faults. EventWindow implements the
// recording policy around triggers: it keeps a rolling pre-trigger buffer of
// the last N seconds (bounded in bytes), and on a trigger flushes that buffer
// and passes everything through until trigger + post window. A trigger inside
// an active window extends it.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include <mr/bytes.hpp>
#include <mr/events.hpp>
#include <mr/record.hpp>
#include <mr/result.hpp>

namespace mr {

struct TriggerConfig {
  double hard_brake_mps2 = 5.0;                 // deceleration threshold
  std::chrono::milliseconds wheel_window{100};  // speed derivative baseline
  double imu_smoothing = 0.2;                   // EMA factor for longitudinal accel
  bool on_quality_fault = false;                // seq gaps / regressions / stale trigger too
  std::chrono::milliseconds refractory{2000};   // minimum spacing between triggers
  std::chrono::milliseconds pre_window{3000};
  std::chrono::milliseconds post_window{2000};
  std::size_t max_buffer_bytes = 64U << 20;  // cap for the pre-trigger buffer
};

class TriggerDetector {
 public:
  explicit TriggerDetector(const TriggerConfig& config);

  // Feed a record; returns a reason if it fires a trigger.
  [[nodiscard]] std::optional<TriggerReason> observe(SensorKind kind, const RecordMeta& meta,
                                                     ByteSpan payload);
  [[nodiscard]] std::optional<TriggerReason> observe(const QualityEvent& event);

 private:
  [[nodiscard]] std::optional<TriggerReason> fire(TriggerReason reason, std::int64_t ts);

  TriggerConfig config_;
  std::int64_t refractory_ns_;
  std::int64_t wheel_window_ns_;
  std::optional<std::int64_t> last_trigger_ns_;
  struct SpeedSample {
    std::int64_t ts;
    double speed;
  };
  std::deque<SpeedSample> wheel_history_;
  std::optional<double> imu_ema_;
};

using RecordSink = std::function<Status(const RecordMeta&, ByteSpan, std::int64_t enqueue_ns)>;

class EventWindow {
 public:
  EventWindow(const TriggerConfig& config, RecordSink sink);

  // Buffers or forwards one record.
  [[nodiscard]] Status on_record(const RecordMeta& meta, ByteSpan payload, std::int64_t enqueue_ns);
  // Flushes the pre-trigger window and opens/extends the post window.
  [[nodiscard]] Status on_trigger(std::int64_t trigger_ts);

  [[nodiscard]] bool recording_at(std::int64_t ts) const noexcept {
    return record_until_ && ts <= *record_until_;
  }
  [[nodiscard]] std::size_t buffered_records() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffered_bytes_; }
  [[nodiscard]] std::uint64_t overflow_evictions() const noexcept { return overflow_evictions_; }
  [[nodiscard]] std::uint64_t triggers() const noexcept { return triggers_; }

 private:
  struct Buffered {
    RecordMeta meta;
    std::int64_t enqueue_ns;
    std::vector<std::byte> payload;
  };
  void evict_front();

  std::int64_t pre_ns_;
  std::int64_t post_ns_;
  std::size_t max_bytes_;
  RecordSink sink_;
  std::deque<Buffered> buffer_;
  std::vector<std::vector<std::byte>> spare_;  // recycled payload buffers
  std::size_t buffered_bytes_ = 0;
  std::int64_t newest_ts_ = 0;
  std::optional<std::int64_t> record_until_;
  std::uint64_t overflow_evictions_ = 0;
  std::uint64_t triggers_ = 0;
};

}  // namespace mr
