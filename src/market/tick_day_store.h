#pragma once

#include "market/depth_tick.h"

#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace qsev {

// Persist full depth ticks under:
//   {data_root}/ticks/{TradingDay}/{InstrumentID}.csv
// UTF-8 CSV with Shanghai wall-clock datetime (human-readable; delete by day folder).
class TickDayStore {
 public:
  explicit TickDayStore(std::string data_root);
  ~TickDayStore();

  TickDayStore(const TickDayStore&) = delete;
  TickDayStore& operator=(const TickDayStore&) = delete;

  void Append(const DepthTick& tick);
  // fflush open files; keep them open for append. Returns ticks since last flush.
  std::size_t Flush();
  std::size_t Pending() const;
  std::size_t TotalWritten() const { return total_written_; }

 private:
  struct FileSink {
    std::string trading_day;
    std::string path;
    std::ofstream out;
    bool header_written{false};
  };

  FileSink* OpenSink(const std::string& trading_day, const std::string& instrument_id);
  void WriteRow(FileSink& sink, const DepthTick& tick);
  void CloseAllUnlocked();

  std::string data_root_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, FileSink> sinks_;  // key = day|id
  std::size_t pending_{0};
  std::size_t total_written_{0};
  std::size_t open_fail_logs_{0};
};

}  // namespace qsev
