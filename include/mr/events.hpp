#pragma once

// Data-quality and trigger events. They are recorded as ordinary records on
// the reserved stream kEventStreamId, so they are ordered, compressed and
// crash-protected like sensor data, and the per-stream quality counters in the
// footer can be rebuilt from them by recovery.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <mr/bytes.hpp>
#include <mr/record.hpp>
#include <mr/writer.hpp>

namespace mr {

enum class EventKind : std::uint16_t {
  kSeqGap = 1,              // value = number of missing sequence numbers
  kTimestampRegression = 2, // value = how far back the timestamp jumped (ns)
  kRateOutOfBounds = 3,     // value = measured rate (mHz), expected = nominal (mHz)
  kRateRecovered = 4,       // value = measured rate (mHz)
  kStreamStale = 5,         // value = silence so far (ns)
  kStreamResumed = 6,       // value = total silence (ns)
  kTrigger = 7,             // value = TriggerReason, expected = window end (ns)
  kRecordsDropped = 8,      // value = records lost in the ring since last report
};

enum class TriggerReason : std::uint16_t {
  kHardBrakeWheel = 1,
  kHardBrakeImu = 2,
  kQualityFault = 3,
};

[[nodiscard]] std::string_view to_string(EventKind kind) noexcept;
[[nodiscard]] std::string_view to_string(TriggerReason reason) noexcept;

struct QualityEvent {
  StreamId stream = 0;  // stream the event is about
  EventKind kind = EventKind::kSeqGap;
  std::int64_t timestamp_ns = 0;
  std::int64_t value = 0;
  std::int64_t expected = 0;
};

inline constexpr std::size_t kEventPayloadSize = 32;

void encode_event(const QualityEvent& event, std::span<std::byte, kEventPayloadSize> out) noexcept;
[[nodiscard]] std::optional<QualityEvent> decode_event(ByteSpan payload) noexcept;
[[nodiscard]] std::string describe(const QualityEvent& event);

// Folds an event into per-stream quality counters. Used both live by the
// recorder and by recovery, so the two always agree.
void accumulate_event(const QualityEvent& event,
                      std::array<StreamQualityCounters, kMaxStreams>& counters) noexcept;

}  // namespace mr
