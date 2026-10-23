#include "market/tick_bus.h"

namespace qsev {

void TickBus::Push(const DepthTick& tick) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) return;
    q_.push_back(tick);
  }
  cv_.notify_one();
}

std::optional<DepthTick> TickBus::TryPop() {
  std::lock_guard<std::mutex> lock(mu_);
  if (q_.empty()) return std::nullopt;
  DepthTick t = q_.front();
  q_.pop_front();
  return t;
}

DepthTick TickBus::PopWait() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [&] { return closed_ || !q_.empty(); });
  if (q_.empty()) return DepthTick{};
  DepthTick t = q_.front();
  q_.pop_front();
  return t;
}

void TickBus::Close() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    closed_ = true;
  }
  cv_.notify_all();
}

bool TickBus::Closed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return closed_;
}

size_t TickBus::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return q_.size();
}

}  // namespace qsev
