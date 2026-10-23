#pragma once

#include "config/app_config.h"
#include "oms/types.h"

#include <mutex>
#include <string>
#include <vector>

namespace qsev {

class TdGateway;
class InstrumentStore;

struct OmsLimits {
  bool enable_trading{false};
  bool dry_run{true};
  int max_volume{1};
  int max_daily_orders{4};
  std::vector<std::string> whitelist;  // InstrumentID and/or product_key (e.g. CZCE.OI). Empty = deny.
};

class Oms {
 public:
  Oms(std::string data_root, TdGateway* td, InstrumentStore* store);

  void SetLimits(OmsLimits lim);
  const OmsLimits& Limits() const { return lim_; }

  void SeedOrderRef(int login_max_ref, const std::string& trading_day);
  std::string NextOrderRef();

  // Compose only (always allowed). Does not send.
  bool Compose(OrderDraft* d, std::string* err) const;

  // Gate + optional Td send. dry_run logs and returns true without Td.
  bool Submit(OrderDraft& d, std::string* err);

  void OnOrder(const OrderRtn& r);
  void OnTrade(const TradeRtn& t);
  void OnAccount(const AccountSnapshot& a);
  void OnPositions(const std::vector<PositionRow>& rows);
  void OnDepth(const DepthQuote& q);

  AccountSnapshot Account() const;
  std::vector<PositionRow> Positions() const;
  DepthQuote Depth() const;
  int TradedOf(const std::string& order_ref) const;
  double FillPrice(const std::string& order_ref) const;
  int DailyOrderCount() const { return daily_orders_; }
  bool LastSubmitSent() const { return last_submit_sent_; }

  std::mutex& Mutex() { return mu_; }

 private:
  std::string data_root_;
  TdGateway* td_;
  InstrumentStore* store_;
  OmsLimits lim_;
  int max_ref_{0};
  std::string trading_day_;
  int daily_orders_{0};
  bool last_submit_sent_{false};

  mutable std::mutex mu_;
  AccountSnapshot account_;
  std::vector<PositionRow> positions_;
  DepthQuote depth_;
  std::vector<OrderRtn> orders_;
  std::vector<TradeRtn> trades_;

  std::string RefPath() const;
  void PersistRef();
  void LoadRef();
  bool Whitelisted(const std::string& instrument_id) const;
};

int RunTradeSelfTest(const std::string& data_root);
int RunOmsSession(const AppConfig& cfg, bool live_roundtrip, const std::string& product_key);
int RunStrategyLiveVerify(const AppConfig& cfg, const std::string& product_key);

}  // namespace qsev
