#pragma once

#include "config/app_config.h"
#include "market/tick_bus.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace qsev {

// ThostFtdcMdApi.h: SubscribeMarketData(ppInstrumentID[], nCount) — one call is one batch.
// Do not pass more than 20 ids per call (front buffer / session drop).
// MdApi::Init() already runs the CTP IO thread; we issue 20-sized calls from workers.
constexpr int kMdSubscribeBatch = 20;
constexpr int kMdSubscribeWorkers = 4;

class MdGateway {
 public:
  MdGateway(const AppConfig& cfg, TickBus& bus);
  ~MdGateway();

  MdGateway(const MdGateway&) = delete;
  MdGateway& operator=(const MdGateway&) = delete;

  bool Start();
  void Stop();

  bool LoggedIn() const { return logged_in_.load(); }
  bool Frozen() const { return frozen_.load(); }

  bool SubscribeInstruments(const std::vector<std::string>& instruments);
  bool UnSubscribeInstruments(const std::vector<std::string>& instruments);

  // Splits into kMdSubscribeBatch (≤20) and issues calls on worker threads.
  bool SubscribeBatched(const std::vector<std::string>& instruments,
                        int batch_size = kMdSubscribeBatch, int delay_ms = 0);
  bool UnSubscribeBatched(const std::vector<std::string>& instruments,
                          int batch_size = kMdSubscribeBatch, int delay_ms = 0);

  std::vector<std::string> ActiveSubscriptions() const;

 private:
  friend class MdSpiImpl;

  AppConfig cfg_;
  TickBus& bus_;
  std::atomic<bool> running_{false};
  std::atomic<bool> logged_in_{false};
  std::atomic<bool> frozen_{true};
  std::atomic<int> req_id_{1};

  void* api_{nullptr};
  void* spi_{nullptr};

  mutable std::mutex sub_mu_;
  std::mutex api_mu_;  // one CThostFtdcMdApi: serialize Subscribe/UnSubscribe sends
  std::unordered_set<std::string> active_subs_;

  bool StartCtp();
  void StopCtp();
  void DoSubscribe();
  bool CallSubscribe(const std::vector<std::string>& instruments, bool subscribe);
  bool DispatchBatches(const std::vector<std::string>& instruments, bool subscribe, int batch_size,
                       int delay_ms);
};

}  // namespace qsev
