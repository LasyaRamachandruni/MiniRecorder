#include <mr/events.hpp>

#include <format>

namespace mr {

std::string_view to_string(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::kSeqGap:
      return "seq_gap";
    case EventKind::kTimestampRegression:
      return "ts_regression";
    case EventKind::kRateOutOfBounds:
      return "rate_out_of_bounds";
    case EventKind::kRateRecovered:
      return "rate_recovered";
    case EventKind::kStreamStale:
      return "stream_stale";
    case EventKind::kStreamResumed:
      return "stream_resumed";
    case EventKind::kTrigger:
      return "trigger";
    case EventKind::kRecordsDropped:
      return "records_dropped";
  }
  return "unknown";
}

std::string_view to_string(TriggerReason reason) noexcept {
  switch (reason) {
    case TriggerReason::kHardBrakeWheel:
      return "hard_brake_wheel";
    case TriggerReason::kHardBrakeImu:
      return "hard_brake_imu";
    case TriggerReason::kQualityFault:
      return "quality_fault";
  }
  return "unknown";
}

void encode_event(const QualityEvent& e, std::span<std::byte, kEventPayloadSize> out) noexcept {
  std::byte* p = out.data();
  store_le<std::uint16_t>(p + 0, e.stream);
  store_le<std::uint16_t>(p + 2, static_cast<std::uint16_t>(e.kind));
  store_le<std::uint32_t>(p + 4, 0);
  store_le<std::int64_t>(p + 8, e.timestamp_ns);
  store_le<std::int64_t>(p + 16, e.value);
  store_le<std::int64_t>(p + 24, e.expected);
}

std::optional<QualityEvent> decode_event(ByteSpan payload) noexcept {
  if (payload.size() != kEventPayloadSize) {
    return std::nullopt;
  }
  const std::byte* p = payload.data();
  QualityEvent e;
  e.stream = load_le<std::uint16_t>(p + 0);
  const auto kind = load_le<std::uint16_t>(p + 2);
  if (kind < static_cast<std::uint16_t>(EventKind::kSeqGap) ||
      kind > static_cast<std::uint16_t>(EventKind::kRecordsDropped) || e.stream >= kMaxStreams) {
    return std::nullopt;
  }
  e.kind = static_cast<EventKind>(kind);
  e.timestamp_ns = load_le<std::int64_t>(p + 8);
  e.value = load_le<std::int64_t>(p + 16);
  e.expected = load_le<std::int64_t>(p + 24);
  return e;
}

std::string describe(const QualityEvent& e) {
  switch (e.kind) {
    case EventKind::kSeqGap:
      return std::format("{} missing sequence number(s)", e.value);
    case EventKind::kTimestampRegression:
      return std::format("timestamp went back {:.3f} ms", static_cast<double>(e.value) / 1e6);
    case EventKind::kRateOutOfBounds:
      return std::format("rate {:.2f} Hz, expected {:.2f} Hz", static_cast<double>(e.value) / 1e3,
                         static_cast<double>(e.expected) / 1e3);
    case EventKind::kRateRecovered:
      return std::format("rate back to {:.2f} Hz", static_cast<double>(e.value) / 1e3);
    case EventKind::kStreamStale:
      return std::format("no data for {:.1f} ms", static_cast<double>(e.value) / 1e6);
    case EventKind::kStreamResumed:
      return std::format("resumed after {:.1f} ms", static_cast<double>(e.value) / 1e6);
    case EventKind::kTrigger:
      return std::format("trigger: {}", to_string(static_cast<TriggerReason>(e.value)));
    case EventKind::kRecordsDropped:
      return std::format("{} record(s) dropped by overflow policy", e.value);
  }
  return "unknown event";
}

void accumulate_event(const QualityEvent& e,
                      std::array<StreamQualityCounters, kMaxStreams>& counters) noexcept {
  if (e.stream >= kMaxStreams) {
    return;
  }
  StreamQualityCounters& c = counters[e.stream];
  switch (e.kind) {
    case EventKind::kSeqGap:
      ++c.seq_gaps;
      break;
    case EventKind::kTimestampRegression:
      ++c.ts_regressions;
      break;
    case EventKind::kRateOutOfBounds:
      ++c.rate_events;
      break;
    case EventKind::kStreamStale:
      ++c.stale_events;
      break;
    case EventKind::kRecordsDropped:
      c.dropped += e.value > 0 ? static_cast<std::uint64_t>(e.value) : 0;
      break;
    case EventKind::kRateRecovered:
    case EventKind::kStreamResumed:
    case EventKind::kTrigger:
      break;
  }
}

}  // namespace mr
