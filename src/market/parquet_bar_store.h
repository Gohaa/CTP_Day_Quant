#pragma once

#include "market/bar_builder.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace qsev {

struct KlineBar {
  double open{0};
  double high{0};
  double low{0};
  double close{0};
  double volume{0};
  int64_t time_unix{0};
};

// Writes/reads Quant_Sev/data/klines/{product}/{Nm}/{year}.parquet per consider.md §0.2.
class ParquetBarStore {
 public:
  // data_root: absolute Quant_Sev/data
  // helper_py: path to tools/klines_parquet_write.py (empty => data_root/../tools/...)
  ParquetBarStore(std::string data_root, std::string helper_py = {});

  static std::string PathFor(const std::string& data_root, const Bar& bar);

  void Append(const Bar& bar);
  // Flush buffered rows to parquet (merge+dedupe). Returns files touched.
  std::size_t Flush(bool replace = false);
  std::size_t PendingBars() const;

  // Load recent bars via tools/klines_parquet_read.py (cached by dir mtime).
  // interval_min: 1/5/15/1440. limit<=0 means helper default (600).
  static bool LoadRecent(const std::string& data_root, const std::string& product_key,
                         int interval_min, int limit, std::vector<KlineBar>* out);

 private:
  struct Row {
    int64_t datetime_ns{0};
    double open{0}, high{0}, low{0}, close{0};
    double volume{0};
    int64_t time_unix{0};
  };

  bool FlushPath(const std::string& path, const std::vector<Row>& rows, bool replace);

  std::string data_root_;
  std::string helper_py_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::vector<Row>> pending_;
};

}  // namespace qsev
