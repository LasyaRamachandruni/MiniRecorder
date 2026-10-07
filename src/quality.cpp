#include <algorithm>
#include <cmath>

#include <mr/quality.hpp>

namespace mr {

QualityMonitor::QualityMonitor(const QualityConfig& config,
                               std::span<const MonitoredStream> streams)
    : config_(config),
      window_ns_(std::chrono::duration_cast<std::chrono::nanoseconds>(config.rate_window).count()),
      states_(kMaxStreams) {
  const auto stale_min_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(config.stale_min).count();
  for (const MonitoredStream& s : streams) {
    if (s.id >= kMaxStreams) {
      continue;
    }
    State& st = states_[s.id];
    st.monitored = true;
    st.nominal_hz = s.nominal_rate_hz;
    if (s.nominal_rate_hz > 0) {
      const double period_ns = 1e9 / s.nominal_rate_hz;
      st.stale_after_ns =
          std::max(stale_min_ns, static_cast<std::int64_t>(config.stale_periods * period_ns));
    }
  }
}

void QualityMonitor::observe(const RecordMeta& meta, std::int64_t arrival_ns,
                             std::vector<QualityEvent>& out) {
  if (meta.stream_id >= kMaxStreams || !states_[meta.stream_id].monitored) {
    return;
  }
  State& st = states_[meta.stream_id];
  const std::int64_t ts = meta.timestamp_ns;

  if (st.stale) {
    out.push_back({.stream = meta.stream_id,
                   .kind = EventKind::kStreamResumed,
                   .timestamp_ns = ts,
                   .value = arrival_ns - st.last_arrival_ns,
                   .expected = 0});
    st.stale = false;
  }

  if (st.has_last) {
    if (meta.seq > st.last_seq + 1) {
      out.push_back({.stream = meta.stream_id,
                     .kind = EventKind::kSeqGap,
                     .timestamp_ns = ts,
                     .value = static_cast<std::int64_t>(meta.seq - st.last_seq - 1),
                     .expected = static_cast<std::int64_t>(st.last_seq + 1)});
    }
    if (ts < st.last_ts) {
      out.push_back({.stream = meta.stream_id,
                     .kind = EventKind::kTimestampRegression,
                     .timestamp_ns = ts,
                     .value = st.last_ts - ts,
                     .expected = st.last_ts});
    }
  }

  if (st.nominal_hz > 0 && window_ns_ > 0) {
    if (!st.has_last) {
      st.window_start_ts = ts;
      st.window_count = 0;
    }
    if (ts - st.window_start_ts >= window_ns_) {
      const double measured_hz =
          static_cast<double>(st.window_count) * 1e9 / static_cast<double>(ts - st.window_start_ts);
      const bool bad =
          std::abs(measured_hz - st.nominal_hz) > config_.rate_tolerance * st.nominal_hz;
      if (bad != st.rate_bad) {
        out.push_back({.stream = meta.stream_id,
                       .kind = bad ? EventKind::kRateOutOfBounds : EventKind::kRateRecovered,
                       .timestamp_ns = ts,
                       .value = std::llround(measured_hz * 1000.0),
                       .expected = std::llround(st.nominal_hz * 1000.0)});
        st.rate_bad = bad;
      }
      st.window_start_ts = ts;
      st.window_count = 0;
    }
    ++st.window_count;
  }

  st.has_last = true;
  st.last_seq = meta.seq;
  st.last_ts = ts;
  st.last_arrival_ns = arrival_ns;
}

void QualityMonitor::check_stale(std::int64_t now_ns, std::vector<QualityEvent>& out) {
  for (std::size_t id = 0; id < states_.size(); ++id) {
    State& st = states_[id];
    if (!st.monitored || !st.has_last || st.stale || st.stale_after_ns == 0) {
      continue;
    }
    const std::int64_t silence = now_ns - st.last_arrival_ns;
    if (silence > st.stale_after_ns) {
      out.push_back({.stream = static_cast<StreamId>(id),
                     .kind = EventKind::kStreamStale,
                     .timestamp_ns = st.last_ts + silence,
                     .value = silence,
                     .expected = st.stale_after_ns});
      st.stale = true;
    }
  }
}

}  // namespace mr
