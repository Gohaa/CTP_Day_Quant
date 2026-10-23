#pragma once



#include "market/tick_snapshot_book.h"

#include "store/instrument_store.h"



#include <string>

#include <vector>



namespace qsev {



struct DominantEntry {

  std::string product_key;

  std::string dominant;

  std::string second;

  double dominant_oi{0};

  double second_oi{0};

  int dominant_volume{0};

  int second_volume{0};

};



struct InstrumentClassStats {

  size_t total{0};

  size_t futures{0};

  size_t futures_trading{0};

  size_t options{0};

  size_t spot_options{0};

  size_t other{0};

};



class DominantSelector {

 public:

  static InstrumentClassStats CountClasses(const InstrumentStore& store);



  // product_filter empty => ALL tradable futures from InstrumentStore (no hardcoded list).

  // Options / SpotOption / Combination are excluded via ProductClass.

  static std::vector<std::string> CollectCandidates(const InstrumentStore& store,

                                                    const std::vector<std::string>& product_filter);



  static std::vector<DominantEntry> Select(const InstrumentStore& store,

                                           const TickSnapshotBook& book,

                                           const std::vector<std::string>& product_filter);



  static bool WriteJson(const std::string& path, const std::string& trading_day,

                        const std::vector<DominantEntry>& entries);

};



}  // namespace qsev


