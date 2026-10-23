#pragma once

#include "store/dominant_selector.h"

#include <string>
#include <vector>

namespace qsev {

// Loaded from Quant_Sev/data/live/dominant/{TradingDay}.json (step06 output).
class DominantTable {
 public:
  static DominantTable LoadFromFile(const std::string& path);
  static DominantTable LoadLatest(const std::string& data_root);
  // Prefer data/live/dominant/{trading_day}.json. If missing, copy latest select to that day.
  static DominantTable LoadForTradingDay(const std::string& data_root, const std::string& trading_day);

  bool WriteDayFile(const std::string& data_root, const std::string& trading_day) const;

  const std::string& TradingDay() const { return trading_day_; }
  const std::vector<DominantEntry>& Entries() const { return entries_; }

  // dominant [+ second]; empty product_filter = all; skip_zero_oi drops oi==0 rows.
  std::vector<std::string> InstrumentIds(const std::vector<std::string>& product_filter = {},
                                         bool skip_zero_oi = true,
                                         bool include_second = true) const;

 private:
  std::string trading_day_;
  std::vector<DominantEntry> entries_;
};

}  // namespace qsev
