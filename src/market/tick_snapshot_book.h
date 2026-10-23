#pragma once

#include "market/depth_tick.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qsev {

struct MarketSnapshot {
  double last_price{0};
  int volume{0};
  double open_interest{0};
  std::string update_time;
  int update_millisec{0};
  bool has_data{false};
};

struct InstrumentTickStats {
  std::string instrument_id;
  uint64_t tick_count{0};
  double ticks_per_sec{0};
  double seconds_since_update{0};
  bool has_data{false};
  bool stale{false};
  // Five-level sanity: at least one side level present when has_data.
  bool depth_ok{false};
};

// Latest full DepthTick per InstrumentID + ring buffer (for bars / screeners) + rate stats.
class TickSnapshotBook {
 public:
  explicit TickSnapshotBook(size_t ring_capacity = 256);

  void SetRingCapacity(size_t n);
  void Update(const DepthTick& tick);

  MarketSnapshot Get(const std::string& instrument_id) const;
  bool TryGetLatest(const std::string& instrument_id, DepthTick* out) const;
  std::vector<DepthTick> RingCopy(const std::string& instrument_id) const;
  size_t Size() const;
  size_t RingSize(const std::string& instrument_id) const;

  std::vector<InstrumentTickStats> Stats(double window_sec, double stale_sec) const;
  size_t CountStale(double stale_sec) const;

 private:
  struct Slot {
    DepthTick latest{};
    std::deque<DepthTick> ring;
    uint64_t tick_count{0};
    std::chrono::steady_clock::time_point last_update{};
    bool has_data{false};
  };

  static bool DepthLooksOk(const DepthTick& t);

  mutable std::mutex mu_;
  size_t ring_capacity_;
  std::unordered_map<std::string, Slot> by_id_;
  std::chrono::steady_clock::time_point created_{std::chrono::steady_clock::now()};
};

}  // namespace qsev
