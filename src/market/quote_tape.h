#pragma once

#include "store/dominant_table.h"
#include "store/session_calendar.h"

#include <cstddef>
#include <string>

namespace qsev {

// Splices 极智 ep_raw (all folder days, one InstrumentID) + CTP data/ticks
// into the current CTP TradingDay session tape for quote.html.
//
// Output:
//   data/live/quote/{TradingDay}/{EXCHANGE.product}.json
//   data/live/quote/{TradingDay}/index.json
//   data/live/quote/latest.json
//
// Product parquet is NOT used (it mixes dominant months).
class QuoteTapeBuilder {
 public:
  QuoteTapeBuilder(std::string data_root, const SessionCalendar* calendar);

  // Rebuild quote JSON for 分时/盘口. write_klines defaults false:
  // historical OHLC lives in data/klines from 极智 import; CTP only merge-appends live bars.
  // trading_day: CTP Md/tick TradingDay (e.g. Monday 21:00 → next calendar day). Empty =
  // max(dominant JSON day, latest data/ticks/{YYYYMMDD}).
  std::size_t RebuildAll(const DominantTable& table, bool write_klines = false,
                         const std::string& trading_day = {});

  static std::string DirFor(const std::string& data_root, const std::string& trading_day);

 private:
  std::string data_root_;
  const SessionCalendar* calendar_;  // nullable
};

// Offline: load latest dominant + sessions, write tape, no CTP.
int RunQuoteTape(const std::string& data_root, const std::string& dominant_file = {});

}  // namespace qsev
