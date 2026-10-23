#pragma once

#include "market/depth_tick.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace qsev {

// SPI callback pushes; engine thread pops. No heavy work in Push.
class TickBus {
 public:
  void Push(const DepthTick& tick);
  std::optional<DepthTick> TryPop();
  DepthTick PopWait();
  void Close();
  bool Closed() const;
  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<DepthTick> q_;
  bool closed_{false};
};

}  // namespace qsev
