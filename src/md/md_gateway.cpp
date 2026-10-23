#include "md/md_gateway.h"

#include "logging/logger.h"
#include "market/depth_tick.h"
#include "util/charset.h"
#include "util/path_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(QUANT_SEV_WITH_CTP) && QUANT_SEV_WITH_CTP
#include "ThostFtdcMdApi.h"
#define QSEV_CTP 1
#else
#define QSEV_CTP 0
#endif

namespace qsev {

#if QSEV_CTP

namespace {

double SanitizePrice(double v) {
  if (!std::isfinite(v)) return 0;
  if (v >= std::numeric_limits<double>::max() / 2) return 0;  // CTP empty = DBL_MAX
  return v;
}

DepthTick FromCtp(const CThostFtdcDepthMarketDataField& d) {
  DepthTick t;
  CopyCStr(t.instrument_id, sizeof(t.instrument_id), d.InstrumentID);
  CopyCStr(t.exchange_id, sizeof(t.exchange_id), d.ExchangeID);
  CopyCStr(t.trading_day, sizeof(t.trading_day), d.TradingDay);
  CopyCStr(t.action_day, sizeof(t.action_day), d.ActionDay);
  CopyCStr(t.update_time, sizeof(t.update_time), d.UpdateTime);
  t.update_millisec = d.UpdateMillisec;
  t.last_price = SanitizePrice(d.LastPrice);
  t.open_price = SanitizePrice(d.OpenPrice);
  t.highest_price = SanitizePrice(d.HighestPrice);
  t.lowest_price = SanitizePrice(d.LowestPrice);
  t.pre_close_price = SanitizePrice(d.PreClosePrice);
  t.pre_settlement_price = SanitizePrice(d.PreSettlementPrice);
  t.upper_limit_price = SanitizePrice(d.UpperLimitPrice);
  t.lower_limit_price = SanitizePrice(d.LowerLimitPrice);
  t.average_price = SanitizePrice(d.AveragePrice);
  t.volume = d.Volume;
  t.turnover = d.Turnover;
  t.open_interest = d.OpenInterest;
  t.pre_open_interest = d.PreOpenInterest;
  t.bid_price[0] = SanitizePrice(d.BidPrice1);
  t.bid_volume[0] = d.BidVolume1;
  t.ask_price[0] = SanitizePrice(d.AskPrice1);
  t.ask_volume[0] = d.AskVolume1;
  t.bid_price[1] = SanitizePrice(d.BidPrice2);
  t.bid_volume[1] = d.BidVolume2;
  t.ask_price[1] = SanitizePrice(d.AskPrice2);
  t.ask_volume[1] = d.AskVolume2;
  t.bid_price[2] = SanitizePrice(d.BidPrice3);
  t.bid_volume[2] = d.BidVolume3;
  t.ask_price[2] = SanitizePrice(d.AskPrice3);
  t.ask_volume[2] = d.AskVolume3;
  t.bid_price[3] = SanitizePrice(d.BidPrice4);
  t.bid_volume[3] = d.BidVolume4;
  t.ask_price[3] = SanitizePrice(d.AskPrice4);
  t.ask_volume[3] = d.AskVolume4;
  t.bid_price[4] = SanitizePrice(d.BidPrice5);
  t.bid_volume[4] = d.BidVolume5;
  t.ask_price[4] = SanitizePrice(d.AskPrice5);
  t.ask_volume[4] = d.AskVolume5;
  return t;
}

}  // namespace

class MdSpiImpl : public CThostFtdcMdSpi {
 public:
  explicit MdSpiImpl(MdGateway* gw) : gw_(gw) {}

  void OnFrontConnected() override {
    Logger::Info("md", "OnFrontConnected");
    gw_->frozen_.store(false);
    CThostFtdcReqUserLoginField req{};
    std::strncpy(req.BrokerID, gw_->cfg_.broker_id.c_str(), sizeof(req.BrokerID) - 1);
    std::strncpy(req.UserID, gw_->cfg_.user_id.c_str(), sizeof(req.UserID) - 1);
    std::strncpy(req.Password, gw_->cfg_.password.c_str(), sizeof(req.Password) - 1);
    auto* api = static_cast<CThostFtdcMdApi*>(gw_->api_);
    const int rid = gw_->req_id_.fetch_add(1);
    const int rc = api->ReqUserLogin(&req, rid);
    Logger::Info("md", "ReqUserLogin rc=" + std::to_string(rc) + " req=" + std::to_string(rid));
  }

  void OnFrontDisconnected(int nReason) override {
    Logger::Warn("md", "OnFrontDisconnected reason=" + std::to_string(nReason));
    gw_->logged_in_.store(false);
    gw_->frozen_.store(true);
  }

  void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin, CThostFtdcRspInfoField* pRspInfo,
                      int /*nRequestID*/, bool /*bIsLast*/) override {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
      Logger::Error("md", "login failed ErrorID=" + std::to_string(pRspInfo->ErrorID) + " " +
                             ToUtf8FromCtpText(pRspInfo->ErrorMsg));
      return;
    }
    gw_->logged_in_.store(true);
    Logger::Info("md", std::string("login ok trading_day=") +
                           (pRspUserLogin ? pRspUserLogin->TradingDay : ""));
    gw_->DoSubscribe();
  }

  void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                          CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) override {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
      Logger::Error("md", std::string("subscribe error: ") + pRspInfo->ErrorMsg +
                              (pSpecificInstrument ? std::string(" id=") + pSpecificInstrument->InstrumentID
                                                   : std::string()));
      return;
    }
    // Avoid per-instrument spam when batch-subscribing thousands of futures.
    if (bIsLast) Logger::Info("md", "OnRspSubMarketData batch ack (bIsLast)");
  }

  void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField* /*pSpecificInstrument*/,
                            CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool bIsLast) override {
    if (pRspInfo && pRspInfo->ErrorID != 0) {
      Logger::Error("md", std::string("unsubscribe error: ") + pRspInfo->ErrorMsg);
      return;
    }
    if (bIsLast) Logger::Info("md", "OnRspUnSubMarketData batch ack (bIsLast)");
  }

  void OnRspError(CThostFtdcRspInfoField* pRspInfo, int /*nRequestID*/, bool /*bIsLast*/) override {
    if (pRspInfo) {
      Logger::Error("md", std::string("OnRspError ") + std::to_string(pRspInfo->ErrorID) + " " +
                              pRspInfo->ErrorMsg);
    }
  }

  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) override {
    if (!pDepthMarketData) return;
    gw_->bus_.Push(FromCtp(*pDepthMarketData));
  }

 private:
  MdGateway* gw_;
};

#endif  // QSEV_CTP

MdGateway::MdGateway(const AppConfig& cfg, TickBus& bus) : cfg_(cfg), bus_(bus) {}

MdGateway::~MdGateway() { Stop(); }

bool MdGateway::Start() {
  if (running_.load()) return true;
#if QSEV_CTP
  return StartCtp();
#else
  Logger::Warn("md", "built without CTP - use --demo-ticks to exercise TickBus");
  Logger::Info("md", "configured md_front=" + cfg_.md_front);
  running_ = true;
  frozen_ = false;
  return true;
#endif
}

void MdGateway::Stop() {
  const bool was = running_.exchange(false);
#if QSEV_CTP
  StopCtp();
#else
  (void)was;
#endif
  logged_in_ = false;
  frozen_ = true;
  if (was) Logger::Info("md", "stopped");
}

void MdGateway::DoSubscribe() {
  // Reconnect must restore what live already Sync'd (dominants), not MD_INSTRUMENTS.
  std::vector<std::string> ids;
  {
    std::lock_guard<std::mutex> lock(sub_mu_);
    ids.assign(active_subs_.begin(), active_subs_.end());
  }
  if (ids.empty()) ids = cfg_.md_instruments;  // debug/demo first login only
  if (ids.empty()) return;
  Logger::Info("md", "DoSubscribe n=" + std::to_string(ids.size()));
  SubscribeBatched(ids, kMdSubscribeBatch, 0);
}

bool MdGateway::CallSubscribe(const std::vector<std::string>& instruments, bool subscribe) {
  if (instruments.empty()) return false;
  if (instruments.size() > static_cast<size_t>(kMdSubscribeBatch)) {
    return DispatchBatches(instruments, subscribe, kMdSubscribeBatch, 0);
  }
#if QSEV_CTP
  if (!api_ || !logged_in_.load()) {
    Logger::Warn("md", std::string(subscribe ? "Subscribe" : "UnSubscribe") + ": not logged in yet");
    return false;
  }
  auto* api = static_cast<CThostFtdcMdApi*>(api_);
  std::vector<std::string> hold = instruments;
  std::vector<char*> ptrs;
  ptrs.reserve(hold.size());
  for (auto& s : hold) ptrs.push_back(s.data());

  int rc = 0;
  for (int attempt = 0; attempt < 5; ++attempt) {
    {
      std::lock_guard<std::mutex> api_lock(api_mu_);
      rc = subscribe ? api->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()))
                     : api->UnSubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    }
    // CTP: -2 pending overflow, -3 per-second limit → sleep and retry.
    if (rc == 0) break;
    Logger::Warn("md", std::string(subscribe ? "Subscribe" : "UnSubscribe") +
                           " rc=" + std::to_string(rc) + " n=" + std::to_string(ptrs.size()) +
                           " retry");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  Logger::Info("md", std::string(subscribe ? "SubscribeMarketData" : "UnSubscribeMarketData") +
                         " n=" + std::to_string(ptrs.size()) + " rc=" + std::to_string(rc));
  if (rc == 0) {
    std::lock_guard<std::mutex> lock(sub_mu_);
    if (subscribe) {
      for (const auto& id : instruments) active_subs_.insert(id);
    } else {
      for (const auto& id : instruments) active_subs_.erase(id);
    }
  }
  return rc == 0;
#else
  (void)subscribe;
  Logger::Info("md", "CallSubscribe stub n=" + std::to_string(instruments.size()));
  return true;
#endif
}

bool MdGateway::SubscribeInstruments(const std::vector<std::string>& instruments) {
  return CallSubscribe(instruments, true);
}

bool MdGateway::UnSubscribeInstruments(const std::vector<std::string>& instruments) {
  return CallSubscribe(instruments, false);
}

bool MdGateway::DispatchBatches(const std::vector<std::string>& instruments, bool subscribe,
                                int batch_size, int delay_ms) {
  if (batch_size <= 0 || batch_size > kMdSubscribeBatch) batch_size = kMdSubscribeBatch;
  if (delay_ms < 0) delay_ms = 0;

  std::vector<std::vector<std::string>> batches;
  for (size_t i = 0; i < instruments.size(); i += static_cast<size_t>(batch_size)) {
    const size_t n = std::min(static_cast<size_t>(batch_size), instruments.size() - i);
    batches.emplace_back(instruments.begin() + static_cast<std::ptrdiff_t>(i),
                         instruments.begin() + static_cast<std::ptrdiff_t>(i + n));
  }
  if (batches.empty()) return true;

  Logger::Info("md", std::string(subscribe ? "subscribe" : "unsubscribe") +
                         " batches=" + std::to_string(batches.size()) +
                         " per_call<=" + std::to_string(batch_size) +
                         " workers=" + std::to_string(kMdSubscribeWorkers) +
                         " total=" + std::to_string(instruments.size()));

  std::atomic<size_t> next{0};
  std::atomic<int> fails{0};
  const int workers = std::max(1, std::min(kMdSubscribeWorkers, static_cast<int>(batches.size())));
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(workers));
  for (int w = 0; w < workers; ++w) {
    pool.emplace_back([&]() {
      while (true) {
        const size_t i = next.fetch_add(1);
        if (i >= batches.size()) break;
        if (!CallSubscribe(batches[i], subscribe)) fails.fetch_add(1);
        if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
    });
  }
  for (auto& t : pool) t.join();
  return fails.load() == 0;
}

bool MdGateway::SubscribeBatched(const std::vector<std::string>& instruments, int batch_size,
                                 int delay_ms) {
  return DispatchBatches(instruments, true, batch_size, delay_ms);
}

bool MdGateway::UnSubscribeBatched(const std::vector<std::string>& instruments, int batch_size,
                                   int delay_ms) {
  return DispatchBatches(instruments, false, batch_size, delay_ms);
}

std::vector<std::string> MdGateway::ActiveSubscriptions() const {
  std::lock_guard<std::mutex> lock(sub_mu_);
  return {active_subs_.begin(), active_subs_.end()};
}

bool MdGateway::StartCtp() {
#if QSEV_CTP
  if (cfg_.md_front.empty()) {
    Logger::Error("md", "MD_FRONT required");
    return false;
  }
  if (!cfg_.production_mode) {
    Logger::Error("md", "production_mode=false forbidden");
    return false;
  }
  spi_ = new MdSpiImpl(this);
  const std::string flow =
      cfg_.md_flow_path.empty() ? PrepareCtpFlowDir(cfg_.flow_path) : cfg_.md_flow_path;
  auto* api = CThostFtdcMdApi::CreateFtdcMdApi(flow.c_str(), false, false, true);
  if (!api) {
    delete static_cast<MdSpiImpl*>(spi_);
    spi_ = nullptr;
    Logger::Error("md", "CreateFtdcMdApi failed");
    return false;
  }
  api_ = api;
  api->RegisterSpi(static_cast<MdSpiImpl*>(spi_));
  char front[256];
  std::snprintf(front, sizeof(front), "%s", cfg_.md_front.c_str());
  api->RegisterFront(front);
  api->Init();
  running_ = true;
  Logger::Info("md", "CTP MdApi Init done front=" + cfg_.md_front);
  return true;
#else
  return false;
#endif
}

void MdGateway::StopCtp() {
#if QSEV_CTP
  if (api_) {
    auto* api = static_cast<CThostFtdcMdApi*>(api_);
    api->RegisterSpi(nullptr);
    api->Release();
    api_ = nullptr;
  }
  if (spi_) {
    delete static_cast<MdSpiImpl*>(spi_);
    spi_ = nullptr;
  }
#endif
}

}  // namespace qsev
