#pragma once

// Online data-quality checks, run by the collector on every record.
//
//   * sequence gaps      : seq jumps by more than one
//   * timestamp regression: timestamp lower than the previous one
//   * rate deviation     : records per window of *sensor* time differ from the
//                          nominal rate by more than a tolerance (reported on
//                          entering and leaving the bad state, not per window)
//   * stale stream       : no record has *arrived* for a while (wall clock),
//                          checked periodically, reported once per episode

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include <mr/events.hpp>
#include <mr/record.hpp>

namespace mr {

struct QualityConfig {
  double rate_tolerance = 0.2;
  std::chrono::milliseconds rate_window{1000};
  double stale_periods = 10.0;  // stale after this many nominal periods without data...
  std::chrono::milliseconds stale_min{100};  // ...but never sooner than this
};

struct MonitoredStream {
  StreamId id = 0;
  double nominal_rate_hz = 0.0;  // 0 disables rate and stale checks
};

class QualityMonitor {
 public:
  QualityMonitor(const QualityConfig& config, std::span<const MonitoredStream> streams);

  // Checks one record; appends any resulting events to `out`.
  void observe(const RecordMeta& meta, std::int64_t arrival_ns, std::vector<QualityEvent>& out);
  // Checks all streams for staleness at wall-clock time `now_ns`.
  void check_stale(std::int64_t now_ns, std::vector<QualityEvent>& out);

 private:
  struct State {
    bool monitored = false;
    double nominal_hz = 0.0;
    std::int64_t stale_after_ns = 0;
    bool has_last = false;
    std::uint64_t last_seq = 0;
    std::int64_t last_ts = 0;
    std::int64_t last_arrival_ns = 0;
    std::int64_t window_start_ts = 0;
    std::uint64_t window_count = 0;
    bool rate_bad = false;
    bool stale = false;
  };

  QualityConfig config_;
  std::int64_t window_ns_;
  std::vector<State> states_;  // indexed by stream id
};

}  // namespace mr
