#include <mr/trigger.hpp>

#include <algorithm>

#include <mr/sensor_types.hpp>

namespace mr {

namespace {
std::int64_t to_ns(std::chrono::milliseconds ms) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(ms).count();
}
}  // namespace

TriggerDetector::TriggerDetector(const TriggerConfig& config)
    : config_(config),
      refractory_ns_(to_ns(config.refractory)),
      wheel_window_ns_(std::max<std::int64_t>(1, to_ns(config.wheel_window))) {}

std::optional<TriggerReason> TriggerDetector::fire(TriggerReason reason, std::int64_t ts) {
  if (last_trigger_ns_ && ts - *last_trigger_ns_ < refractory_ns_ && ts >= *last_trigger_ns_) {
    return std::nullopt;
  }
  last_trigger_ns_ = ts;
  return reason;
}

std::optional<TriggerReason> TriggerDetector::observe(SensorKind kind, const RecordMeta& meta,
                                                      ByteSpan payload) {
  const std::int64_t ts = meta.timestamp_ns;
  if (kind == SensorKind::kWheelCan) {
    const auto frame = decode_wheel(payload);
    if (!frame) {
      return std::nullopt;
    }
    const double v = frame->mean_speed_mps();
    // Drop out-of-order samples (timestamp regressions) from the derivative.
    if (!wheel_history_.empty() && ts <= wheel_history_.back().ts) {
      return std::nullopt;
    }
    wheel_history_.push_back({ts, v});
    // Keep exactly one sample at least `wheel_window` old as the baseline.
    while (wheel_history_.size() > 2 && ts - wheel_history_[1].ts >= wheel_window_ns_) {
      wheel_history_.pop_front();
    }
    const SpeedSample& base = wheel_history_.front();
    const std::int64_t dt = ts - base.ts;
    if (dt >= wheel_window_ns_ / 2) {
      const double decel = (base.speed - v) / (static_cast<double>(dt) / 1e9);
      if (decel >= config_.hard_brake_mps2) {
        return fire(TriggerReason::kHardBrakeWheel, ts);
      }
    }
    return std::nullopt;
  }
  if (kind == SensorKind::kImu) {
    const auto sample = decode_imu(payload);
    if (!sample) {
      return std::nullopt;
    }
    imu_ema_ = imu_ema_ ? *imu_ema_ + config_.imu_smoothing * (sample->accel_x - *imu_ema_)
                        : sample->accel_x;
    if (*imu_ema_ <= -config_.hard_brake_mps2) {
      return fire(TriggerReason::kHardBrakeImu, ts);
    }
  }
  return std::nullopt;
}

std::optional<TriggerReason> TriggerDetector::observe(const QualityEvent& event) {
  if (!config_.on_quality_fault) {
    return std::nullopt;
  }
  switch (event.kind) {
    case EventKind::kSeqGap:
    case EventKind::kTimestampRegression:
    case EventKind::kStreamStale:
      return fire(TriggerReason::kQualityFault, event.timestamp_ns);
    default:
      return std::nullopt;
  }
}

// ---------------------------------------------------------------- EventWindow

EventWindow::EventWindow(const TriggerConfig& config, RecordSink sink)
    : pre_ns_(to_ns(config.pre_window)),
      post_ns_(to_ns(config.post_window)),
      max_bytes_(config.max_buffer_bytes),
      sink_(std::move(sink)) {}

void EventWindow::evict_front() {
  buffered_bytes_ -= buffer_.front().payload.size();
  spare_.push_back(std::move(buffer_.front().payload));
  buffer_.pop_front();
}

Status EventWindow::on_record(const RecordMeta& meta, ByteSpan payload, std::int64_t enqueue_ns) {
  newest_ts_ = std::max(newest_ts_, meta.timestamp_ns);
  if (recording_at(meta.timestamp_ns)) {
    return sink_(meta, payload, enqueue_ns);
  }
  std::vector<std::byte> copy;
  if (!spare_.empty()) {
    copy = std::move(spare_.back());
    spare_.pop_back();
  }
  copy.assign(payload.begin(), payload.end());
  buffered_bytes_ += copy.size();
  buffer_.push_back({meta, enqueue_ns, std::move(copy)});

  while (!buffer_.empty() && buffer_.front().meta.timestamp_ns < newest_ts_ - pre_ns_) {
    evict_front();
  }
  while (buffered_bytes_ > max_bytes_ && !buffer_.empty()) {
    evict_front();
    ++overflow_evictions_;
  }
  return ok_status();
}

Status EventWindow::on_trigger(std::int64_t trigger_ts) {
  ++triggers_;
  const std::int64_t from = trigger_ts - pre_ns_;
  Status status;
  while (!buffer_.empty()) {
    Buffered& b = buffer_.front();
    if (b.meta.timestamp_ns >= from && status) {
      RecordMeta meta = b.meta;
      meta.flags |= record_flags::kPreTrigger;
      status = sink_(meta, b.payload, b.enqueue_ns);
    }
    evict_front();
  }
  const std::int64_t until = trigger_ts + post_ns_;
  record_until_ = record_until_ ? std::max(*record_until_, until) : until;
  return status;
}

}  // namespace mr
