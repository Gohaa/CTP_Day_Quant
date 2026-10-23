#pragma once

#include "market/depth_tick.h"
#include "store/session_calendar.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qsev {

struct Bar {
  std::string product_key;
  int interval_min{5};             // 1/5/15; daily uses 1440
  int64_t datetime_ns{0};          // open, unix ns
  double open{0}, high{0}, low{0}, close{0};
  double volume{0};
  double open_interest{0};         // last OI on this bar (memory; not a parquet column)
  int64_t time_unix{0};            // close, unix sec
  std::string trading_day;         // for daily; empty for minute bars
};

using BarClosedFn = std::function<void(const Bar&)>;

// Aggregate ticks into one interval series (wall-clock minutes or session day).
class BarBuilder {
 public:
  static constexpr int kDailyIntervalMin = 1440;

  BarBuilder(int interval_min, const SessionCalendar* calendar);

  int IntervalMin() const { return interval_min_; }

  // product_key must be set by caller (e.g. SHFE.rb).
  void OnTick(const std::string& product_key, const DepthTick& tick, const BarClosedFn& on_closed);

  // Force-close open bars (shutdown / day change).
  void Flush(const BarClosedFn& on_closed);

 private:
  struct State {
    bool active{false};
    Bar bar;
    int64_t last_cum_volume{-1};
    int last_segment_key{-1};  // for minute: aligned open sec; for day: unused
  };

  void Emit(State& st, const BarClosedFn& on_closed);
  void OnMinuteTick(const std::string& product_key, const DepthTick& tick, int64_t ts_ns,
                    const BarClosedFn& on_closed);
  void OnDailyTick(const std::string& product_key, const DepthTick& tick, int64_t ts_ns,
                   const BarClosedFn& on_closed);

  int interval_min_;
  bool daily_;
  const SessionCalendar* calendar_;  // nullable
  std::unordered_map<std::string, State> by_product_;
};

// Fans each tick to 1m / 5m / 15m / 1d builders.
class MultiBarBuilder {
 public:
  explicit MultiBarBuilder(const SessionCalendar* calendar);

  void OnTick(const std::string& product_key, const DepthTick& tick, const BarClosedFn& on_closed);
  void Flush(const BarClosedFn& on_closed);

 private:
  BarBuilder b1_;
  BarBuilder b5_;
  BarBuilder b15_;
  BarBuilder b1d_;
};

}  // namespace qsev
