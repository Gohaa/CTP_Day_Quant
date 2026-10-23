#pragma once

#include "config/app_config.h"
#include "market/tick_snapshot_book.h"
#include "store/dominant_table.h"
#include "store/instrument_store.h"
#include "util/time_shanghai.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qsev {

// Latest quote used by the option screen (tick / snapshot / 昨结 fallback).
struct OptionQuote {
  double last{0};
  double bid{0};
  double ask{0};
  double open_interest{0};
  double volume{0};
  bool has_last{false};
  bool has_book{false};
  std::string source;  // tick | snapshot | pre_settlement
};

struct OptionScreenConfig {
  int dte_min{1};   // keep DTE >= this (step14: 0 < DTE)
  int dte_max{29};  // keep DTE <= this (step14: DTE < 30)
  bool include_spot_option{true};
  bool require_trading{true};
  bool restrict_underlying_dominant{true};
  bool include_second{true};
  std::string dte_mode{"calendar"};  // calendar | trading (weekdays only)
  std::string as_of;                 // YYYYMMDD; empty = Shanghai today
  double hv{0.22};                   // Black-76 σ
  double rate{0.02};
  double cheap_ratio{0.5};           // MarketTV / TheoTV
  double min_theo_tv{1.0};
  double min_gap{0.0};               // 0 = ratio only; >0 also allows Diff > min_gap
  int max_spread_ticks{8};           // 0 = skip bid/ask filter
  double min_oi{0};
  double min_volume{0};
  int max_subscribe{200};
};

struct OptionScreenRow {
  std::string instrument_id;
  std::string exchange_id;
  std::string product_id;
  std::string underlying_id;
  std::string expire_date;
  char options_type{0};  // '1' Call / '2' Put
  int dte{0};
  double strike{0};
  double price_tick{0};
  double spot{0};
  double opt_px{0};
  double bid{0};
  double ask{0};
  double spread_ticks{-1};
  double oi{0};
  double volume{0};
  double intrinsic{0};
  double market_tv{0};
  double bs_price{0};
  double theo_tv{0};
  double ratio{-1};
  double diff{0};
  bool cheap{false};
  bool liquid{false};
  std::string quote_source;
  std::string reject_reason;
  std::string explain;
};

struct OptionScreenResult {
  std::string as_of;
  std::string trading_day;
  OptionScreenConfig cfg;
  std::vector<OptionScreenRow> near;
  std::vector<OptionScreenRow> cheap;
  std::vector<std::string> subscribe;
  size_t options_seen{0};
  size_t dte_kept{0};
  size_t underlying_kept{0};
  bool orders_placed{false};  // always false
};

struct OptionScreenCliOptions {
  bool enabled{false};
  bool selftest{false};
  bool live{false};
  bool include_second{true};
  bool include_spot{true};
  std::string as_of;
  std::string dte_mode{"calendar"};
  std::string instruments_path;
  std::string dominant_path;
  std::string out_dir;
  int dte_min{-1};
  int dte_max{-1};
  double hv{-1};
  double rate{-1};
  double cheap_ratio{-1};
  double min_theo_tv{-1};
  double min_gap{-1};
  int max_spread_ticks{-1};
  double min_oi{-1};
  double min_volume{-1};
  int max_subscribe{-1};
  int live_seconds{15};
};

int CalendarDaysBetween(const CivilDate& from, const CivilDate& to);
int WeekdayTradingDaysBetween(const CivilDate& from, const CivilDate& to);

double IntrinsicValue(char options_type, double spot, double strike);
double Black76Price(bool is_call, double forward, double strike, double t_years, double sigma,
                    double rate);
double NormCdf(double x);

std::unordered_set<std::string> DominantUnderlyingIds(const DominantTable& table,
                                                      bool include_second);
bool UnderlyingAllowed(const std::string& underlying_instr_id,
                       const std::unordered_set<std::string>& allowed_ids);

void MergeQuotesFromPreSettlement(const InstrumentStore& store,
                                  std::unordered_map<std::string, OptionQuote>* quotes);
void MergeQuotesFromBook(const TickSnapshotBook& book,
                         std::unordered_map<std::string, OptionQuote>* quotes);
void MergeQuotesFromTickFiles(const std::string& data_root, const std::string& day,
                              const std::vector<std::string>& ids,
                              std::unordered_map<std::string, OptionQuote>* quotes);

OptionScreenResult ScreenOptions(const InstrumentStore& store, const DominantTable* dominant,
                                 const std::unordered_map<std::string, OptionQuote>& quotes,
                                 const OptionScreenConfig& cfg);

bool WriteOptionScreen(const std::string& json_path, const std::string& csv_path,
                       const OptionScreenResult& result);

std::string FindLatestFullInstruments(const std::string& data_root);

int RunOptionScreenSelfTest();
int RunOptionScreen(const OptionScreenCliOptions& opt, const std::string& data_root,
                    const AppConfig* cfg_for_live);

}  // namespace qsev
