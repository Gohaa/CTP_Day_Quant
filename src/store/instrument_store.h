#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "store/session_calendar.h"

namespace qsev {

// Contract basics assembled from multiple CTP queries (step05):
// - CThostFtdcInstrumentField          via ReqQryInstrument
// - CThostFtdcDepthMarketDataField     via ReqQryDepthMarketData (limits)
// - CThostFtdcInstrumentMarginRateField via ReqQryInstrumentMarginRate
// - CThostFtdcExchangeMarginRateField   via ReqQryExchangeMarginRate
// - Trading sessions: NOT in CTP — openctp /times HTTP → SessionCalendar (+ local cache)
struct InstrumentInfo {
  std::string exchange_id;
  std::string instrument_id;
  std::string exchange_inst_id;
  std::string product_id;
  std::string instrument_name;
  char product_class{0};
  char inst_life_phase{0};
  char options_type{0};
  char position_type{0};
  char position_date_type{0};
  char combination_type{0};
  char max_margin_side_algorithm{0};

  int delivery_year{0};
  int delivery_month{0};
  int volume_multiple{0};
  int max_market_order_volume{0};
  int min_market_order_volume{0};  // 市价最少开仓手数
  int max_limit_order_volume{0};
  int min_limit_order_volume{0};  // 限价最少开仓手数

  double price_tick{0};
  double strike_price{0};
  double underlying_multiple{0};

  // From InstrumentField Long/ShortMarginRatio (often empty=DBL_MAX on some fronts).
  double long_margin_ratio{0};
  double short_margin_ratio{0};
  bool instrument_margin_valid{false};

  std::string create_date;
  std::string open_date;    // CTP 上市日（不是每日开盘钟点）
  std::string expire_date;  // 合约到期日
  std::string start_deliv_date;
  std::string end_deliv_date;
  std::string underlying_instr_id;

  bool is_trading{false};

  // From DepthMarketData (涨跌停板价 / 昨结 → 幅度).
  double upper_limit_price{0};
  double lower_limit_price{0};
  double pre_settlement_price{0};
  double limit_up_ratio{0};    // (upper - pre) / pre
  double limit_down_ratio{0};  // (pre - lower) / pre
  bool limits_valid{false};

  // Investor margin (ReqQryInstrumentMarginRate, HedgeFlag=Speculation).
  double long_margin_ratio_by_money{0};
  double long_margin_ratio_by_volume{0};
  double short_margin_ratio_by_money{0};
  double short_margin_ratio_by_volume{0};
  bool margin_is_relative{false};
  char margin_hedge_flag{0};
  bool investor_margin_valid{false};

  // Exchange margin (ReqQryExchangeMarginRate).
  double exch_long_margin_ratio_by_money{0};
  double exch_long_margin_ratio_by_volume{0};
  double exch_short_margin_ratio_by_money{0};
  double exch_short_margin_ratio_by_volume{0};
  bool exchange_margin_valid{false};

  // Exchange trading clock sessions (open/close segments). Not from CTP InstrumentField.
  std::vector<TradingSegment> sessions;
  bool sessions_valid{false};
  std::string session_day_open;    // first daytime open HH:MM:SS
  std::string session_day_close;   // last daytime close
  std::string session_night_open;  // night open if any
  std::string session_night_close;
};

class InstrumentStore {
 public:
  void Clear();
  void Upsert(const InstrumentInfo& info);
  void PatchLimits(const std::string& instrument_id, double upper, double lower, double pre_settlement);
  void PatchInvestorMargin(const std::string& instrument_id, double long_by_money, double long_by_volume,
                           double short_by_money, double short_by_volume, bool is_relative, char hedge_flag);
  void PatchExchangeMargin(const std::string& instrument_id, double long_by_money, double long_by_volume,
                           double short_by_money, double short_by_volume);
  void PatchSessions(const std::string& instrument_id, const std::vector<TradingSegment>& sessions);

  void MarkReady();
  void MarkEnriched();
  bool Ready() const { return ready_.load(); }
  bool Enriched() const { return enriched_.load(); }

  void SetTradingDay(const std::string& day);
  std::string TradingDay() const;

  const InstrumentInfo* Find(const std::string& instrument_id) const;
  std::vector<InstrumentInfo> List() const;
  size_t Size() const;

  // Derived APIs (replace _TICK_VALUE / _BUILTIN_SYMBOLS on live path).
  static std::string ProductKey(const std::string& exchange, const std::string& product_id);
  static double TickValue(const InstrumentInfo& info);  // PriceTick * VolumeMultiple

  std::optional<double> TickSize(const std::string& instrument_id) const;
  std::optional<int> Multiplier(const std::string& instrument_id) const;
  std::optional<double> TickValueOf(const std::string& instrument_id) const;
  // Open 1+ lots: Price × VolumeMultiple × lots × money_ratio + lots × volume_ratio.
  // Investor margin first, then exchange, then instrument. 0 if unknown.
  static double EstimateOpenMargin(const InstrumentInfo& info, bool is_short, double price, int lots);
  // Same, but if CTP ratio is empty use 12% of notional so a margin cap cannot be skipped.
  static double EstimateOpenMarginOrHaircut(const InstrumentInfo& info, bool is_short, double price,
                                            int lots);
  // product_class==0 => all classes; else filter exact CTP ProductClass char.
  std::vector<std::string> ListProductKeys(char product_class = 0) const;

  // Persist / reload: Quant_Sev/data/live/instruments/... (DATA_ROOT=./data)
  bool WriteJson(const std::string& path) const;
  static bool WriteJson(const std::string& path, const std::string& trading_day,
                        const std::vector<InstrumentInfo>& rows);
  bool LoadJson(const std::string& path);

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, InstrumentInfo> by_id_;
  std::string trading_day_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> enriched_{false};
};

}  // namespace qsev
