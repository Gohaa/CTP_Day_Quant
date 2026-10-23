#include "market/bar_builder.h"

#include "util/time_shanghai.h"

#include <algorithm>

namespace qsev {

BarBuilder::BarBuilder(int interval_min, const SessionCalendar* calendar)
    : interval_min_(interval_min),
      daily_(interval_min == kDailyIntervalMin),
      calendar_(calendar) {}

void BarBuilder::Emit(State& st, const BarClosedFn& on_closed) {
  if (!st.active) return;
  if (on_closed) on_closed(st.bar);
  st.active = false;
  st.bar = Bar{};
  st.last_segment_key = -1;
  // keep last_cum_volume across bars within same day for continuous volume series
}

void BarBuilder::OnMinuteTick(const std::string& product_key, const DepthTick& tick, int64_t ts_ns,
                              const BarClosedFn& on_closed) {
  if (calendar_ && !calendar_->IsInSession(product_key, ts_ns)) {
    // Left session: close open bar if any.
    auto it = by_product_.find(product_key);
    if (it != by_product_.end() && it->second.active) {
      Emit(it->second, on_closed);
    }
    return;
  }

  const int64_t open_ns = AlignBarOpenNsShanghai(ts_ns, interval_min_);
  const int64_t close_sec = open_ns / 1000000000LL + static_cast<int64_t>(interval_min_) * 60;
  const int seg_key = static_cast<int>(open_ns / 1000000000LL);

  auto& st = by_product_[product_key];
  if (st.active && st.last_segment_key != seg_key) {
    Emit(st, on_closed);
  }

  const int cum = tick.volume;
  double dvol = 0;
  if (st.last_cum_volume >= 0 && cum >= st.last_cum_volume) {
    dvol = static_cast<double>(cum - st.last_cum_volume);
  } else if (st.last_cum_volume >= 0 && cum < st.last_cum_volume) {
    // New CTP trading day: Volume resets. Do not dump previous session into this bar.
    dvol = static_cast<double>(cum);
  }
  // Mid-session attach: first nonzero Volume is the session cumulative, not this minute.
  if (dvol > 10000 && st.last_cum_volume <= 0 && dvol >= static_cast<double>(cum) * 0.5) {
    dvol = 0;
  }
  st.last_cum_volume = cum;

  if (!st.active) {
    st.active = true;
    st.last_segment_key = seg_key;
    st.bar.product_key = product_key;
    st.bar.interval_min = interval_min_;
    st.bar.datetime_ns = open_ns;
    st.bar.time_unix = close_sec;
    st.bar.open = tick.last_price;
    st.bar.high = tick.last_price;
    st.bar.low = tick.last_price;
    st.bar.close = tick.last_price;
    st.bar.volume = dvol;
    st.bar.open_interest = tick.open_interest;
    return;
  }

  st.bar.high = std::max(st.bar.high, tick.last_price);
  st.bar.low = std::min(st.bar.low, tick.last_price);
  st.bar.close = tick.last_price;
  st.bar.volume += dvol;
  st.bar.open_interest = tick.open_interest;
}

void BarBuilder::OnDailyTick(const std::string& product_key, const DepthTick& tick, int64_t ts_ns,
                             const BarClosedFn& on_closed) {
  const std::string td = tick.trading_day[0] != '\0' ? std::string(tick.trading_day) : std::string();
  if (td.empty()) return;

  auto& st = by_product_[product_key];

  // Lunch / night gaps: do NOT close the daily bar. Only close after final session end.
  if (calendar_ && !calendar_->IsInSession(product_key, ts_ns)) {
    if (st.active && st.bar.time_unix > 0 && ts_ns / 1000000000LL >= st.bar.time_unix) {
      Emit(st, on_closed);
      st.last_cum_volume = -1;
    }
    return;
  }

  if (st.active && st.bar.trading_day != td) {
    Emit(st, on_closed);
    st.last_cum_volume = -1;
  }

  const int cum = tick.volume;
  double dvol = 0;
  if (st.last_cum_volume >= 0 && cum >= st.last_cum_volume) {
    dvol = static_cast<double>(cum - st.last_cum_volume);
  } else if (st.last_cum_volume >= 0 && cum < st.last_cum_volume) {
    dvol = static_cast<double>(cum);
  }
  st.last_cum_volume = cum;

  if (!st.active) {
    st.active = true;
    st.bar.product_key = product_key;
    st.bar.interval_min = kDailyIntervalMin;
    st.bar.trading_day = td;
    SessionBound bound;
    if (calendar_) bound = calendar_->DayBounds(product_key, td);
    if (bound.ok) {
      st.bar.datetime_ns = bound.open_unix_ns;
      st.bar.time_unix = bound.close_unix_sec;
    } else {
      st.bar.datetime_ns = ts_ns;
      st.bar.time_unix = ts_ns / 1000000000LL;
    }
    st.bar.open = tick.last_price;
    st.bar.high = tick.last_price;
    st.bar.low = tick.last_price;
    st.bar.close = tick.last_price;
    st.bar.volume = dvol;
    st.bar.open_interest = tick.open_interest;
    return;
  }

  st.bar.high = std::max(st.bar.high, tick.last_price);
  st.bar.low = std::min(st.bar.low, tick.last_price);
  st.bar.close = tick.last_price;
  st.bar.volume += dvol;
  st.bar.open_interest = tick.open_interest;
}

void BarBuilder::OnTick(const std::string& product_key, const DepthTick& tick,
                        const BarClosedFn& on_closed) {
  if (product_key.empty()) return;
  if (!IsValidLastPrice(tick.last_price)) return;
  const int64_t ts_ns =
      TickToUnixNs(tick.action_day, tick.trading_day, tick.update_time, tick.update_millisec);
  if (ts_ns <= 0) return;

  if (daily_) {
    OnDailyTick(product_key, tick, ts_ns, on_closed);
  } else {
    OnMinuteTick(product_key, tick, ts_ns, on_closed);
  }
}

void BarBuilder::Flush(const BarClosedFn& on_closed) {
  for (auto& kv : by_product_) {
    if (kv.second.active) Emit(kv.second, on_closed);
  }
}

MultiBarBuilder::MultiBarBuilder(const SessionCalendar* calendar)
    : b1_(1, calendar), b5_(5, calendar), b15_(15, calendar), b1d_(BarBuilder::kDailyIntervalMin, calendar) {}

void MultiBarBuilder::OnTick(const std::string& product_key, const DepthTick& tick,
                             const BarClosedFn& on_closed) {
  b1_.OnTick(product_key, tick, on_closed);
  b5_.OnTick(product_key, tick, on_closed);
  b15_.OnTick(product_key, tick, on_closed);
  b1d_.OnTick(product_key, tick, on_closed);
}

void MultiBarBuilder::Flush(const BarClosedFn& on_closed) {
  b1_.Flush(on_closed);
  b5_.Flush(on_closed);
  b15_.Flush(on_closed);
  b1d_.Flush(on_closed);
}

}  // namespace qsev
