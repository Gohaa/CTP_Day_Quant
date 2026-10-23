#include "market/tick_snapshot_book.h"

#include <algorithm>
#include <cmath>

namespace qsev {

TickSnapshotBook::TickSnapshotBook(size_t ring_capacity)
    : ring_capacity_(ring_capacity == 0 ? 1 : ring_capacity) {}

void TickSnapshotBook::SetRingCapacity(size_t n) {
  std::lock_guard<std::mutex> lock(mu_);
  ring_capacity_ = n == 0 ? 1 : n;
  for (auto& kv : by_id_) {
    while (kv.second.ring.size() > ring_capacity_) kv.second.ring.pop_front();
  }
}

bool TickSnapshotBook::DepthLooksOk(const DepthTick& t) {
  for (int i = 0; i < 5; ++i) {
    if (t.bid_price[i] > 0 || t.ask_price[i] > 0 || t.bid_volume[i] > 0 || t.ask_volume[i] > 0) {
      return true;
    }
  }
  // Some quiet contracts may only have last/oi without book; still accept last>0.
  return t.last_price > 0 || t.open_interest > 0;
}

void TickSnapshotBook::Update(const DepthTick& tick) {
  std::lock_guard<std::mutex> lock(mu_);
  Slot& s = by_id_[tick.instrument_id];
  s.latest = tick;
  s.has_data = true;
  s.tick_count += 1;
  s.last_update = std::chrono::steady_clock::now();
  s.ring.push_back(tick);
  while (s.ring.size() > ring_capacity_) s.ring.pop_front();
}

MarketSnapshot TickSnapshotBook::Get(const std::string& instrument_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = by_id_.find(instrument_id);
  if (it == by_id_.end() || !it->second.has_data) return MarketSnapshot{};
  MarketSnapshot m;
  m.last_price = it->second.latest.last_price;
  m.volume = it->second.latest.volume;
  m.open_interest = it->second.latest.open_interest;
  m.update_time = it->second.latest.update_time;
  m.update_millisec = it->second.latest.update_millisec;
  m.has_data = true;
  return m;
}

bool TickSnapshotBook::TryGetLatest(const std::string& instrument_id, DepthTick* out) const {
  if (!out) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = by_id_.find(instrument_id);
  if (it == by_id_.end() || !it->second.has_data) return false;
  *out = it->second.latest;
  return true;
}

std::vector<DepthTick> TickSnapshotBook::RingCopy(const std::string& instrument_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = by_id_.find(instrument_id);
  if (it == by_id_.end()) return {};
  return {it->second.ring.begin(), it->second.ring.end()};
}

size_t TickSnapshotBook::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return by_id_.size();
}

size_t TickSnapshotBook::RingSize(const std::string& instrument_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = by_id_.find(instrument_id);
  if (it == by_id_.end()) return 0;
  return it->second.ring.size();
}

std::vector<InstrumentTickStats> TickSnapshotBook::Stats(double window_sec, double stale_sec) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = std::chrono::steady_clock::now();
  const double elapsed =
      std::chrono::duration<double>(now - created_).count();
  const double denom = window_sec > 0 ? std::min(window_sec, elapsed) : elapsed;
  std::vector<InstrumentTickStats> out;
  out.reserve(by_id_.size());
  for (const auto& kv : by_id_) {
    InstrumentTickStats st;
    st.instrument_id = kv.first;
    st.tick_count = kv.second.tick_count;
    st.has_data = kv.second.has_data;
    st.depth_ok = kv.second.has_data && DepthLooksOk(kv.second.latest);
    if (kv.second.has_data) {
      st.seconds_since_update =
          std::chrono::duration<double>(now - kv.second.last_update).count();
      st.stale = stale_sec > 0 && st.seconds_since_update > stale_sec;
    } else {
      st.seconds_since_update = elapsed;
      st.stale = true;
    }
    st.ticks_per_sec = denom > 1e-6 ? static_cast<double>(st.tick_count) / denom : 0;
    out.push_back(st);
  }
  std::sort(out.begin(), out.end(),
            [](const InstrumentTickStats& a, const InstrumentTickStats& b) {
              return a.instrument_id < b.instrument_id;
            });
  return out;
}

size_t TickSnapshotBook::CountStale(double stale_sec) const {
  size_t n = 0;
  for (const auto& st : Stats(0, stale_sec)) {
    if (st.stale) ++n;
  }
  return n;
}

}  // namespace qsev
