#include "market/tick_day_store.h"

#include "logging/logger.h"
#include "util/time_shanghai.h"

#include <cstdio>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <share.h>
#include <stdio.h>
#endif

namespace qsev {
namespace {

namespace fs = std::filesystem;

std::string SafeInstrumentFileName(const std::string& id) {
  std::string o;
  o.reserve(id.size());
  for (unsigned char c : id) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
        c == '-' || c == '.') {
      o.push_back(static_cast<char>(c));
    } else {
      o.push_back('_');
    }
  }
  return o.empty() ? "unknown" : o;
}

std::string ShanghaiDateTimeCsv(const DepthTick& tick) {
  const int64_t ns =
      TickToUnixNs(tick.action_day, tick.trading_day, tick.update_time, tick.update_millisec);
  if (ns <= 0) {
    return std::string(tick.action_day) + " " + tick.update_time;
  }
  const ShanghaiWall w = UnixSecToShanghai(ns / 1000000000LL);
  const int ms = static_cast<int>((ns / 1000000LL) % 1000LL);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", w.date.year, w.date.month,
                w.date.day, w.hour, w.minute, w.second, ms);
  return buf;
}

}  // namespace

namespace {

constexpr std::size_t kMaxOpenSinks = 128;

}  // namespace

TickDayStore::TickDayStore(std::string data_root) : data_root_(std::move(data_root)) {
#ifdef _WIN32
  // CRT default is 512; quote_tape + parquet jobs share the same process limit.
  _setmaxstdio(2048);
#endif
}

TickDayStore::~TickDayStore() {
  std::lock_guard<std::mutex> lock(mu_);
  CloseAllUnlocked();
}

void TickDayStore::CloseAllUnlocked() {
  for (auto& kv : sinks_) {
    if (kv.second.out.is_open()) {
      kv.second.out.flush();
      kv.second.out.close();
    }
  }
  sinks_.clear();
}

TickDayStore::FileSink* TickDayStore::OpenSink(const std::string& trading_day,
                                               const std::string& instrument_id) {
  const std::string key = trading_day + "|" + instrument_id;
  auto it = sinks_.find(key);
  if (it != sinks_.end()) {
    if (it->second.out.is_open()) return &it->second;
    sinks_.erase(it);
  }
  if (sinks_.size() >= kMaxOpenSinks) {
    CloseAllUnlocked();
  }
  FileSink sink;
  sink.trading_day = trading_day;
  const fs::path dir = fs::path(data_root_) / "ticks" / trading_day;
  std::error_code ec;
  fs::create_directories(dir, ec);
  sink.path = (dir / (SafeInstrumentFileName(instrument_id) + ".csv")).string();
  const bool existed = fs::exists(sink.path, ec);
#ifdef _WIN32
  sink.out.open(sink.path.c_str(), std::ios::out | std::ios::app | std::ios::binary, _SH_DENYNO);
#else
  sink.out.open(sink.path, std::ios::out | std::ios::app | std::ios::binary);
#endif
  if (!sink.out) {
    CloseAllUnlocked();
#ifdef _WIN32
    sink.out.open(sink.path.c_str(), std::ios::out | std::ios::app | std::ios::binary, _SH_DENYNO);
#else
    sink.out.open(sink.path, std::ios::out | std::ios::app | std::ios::binary);
#endif
  }
  if (!sink.out) {
    ++open_fail_logs_;
    if (open_fail_logs_ <= 8 || open_fail_logs_ % 200 == 0) {
      Logger::Error("ticks", "cannot open " + sink.path +
                                 " n=" + std::to_string(open_fail_logs_) +
                                 " (handle/sharing limit)");
    }
    return nullptr;
  }
  sink.header_written = existed && fs::file_size(sink.path, ec) > 0;
  if (!sink.header_written) {
    // UTF-8 BOM for Excel
    sink.out << "\xEF\xBB\xBF";
    sink.out
        << "datetime,exchange_id,instrument_id,trading_day,action_day,update_time,update_millisec,"
           "last_price,volume,turnover,open_interest,pre_open_interest,"
           "bid_price1,bid_volume1,bid_price2,bid_volume2,bid_price3,bid_volume3,bid_price4,"
           "bid_volume4,bid_price5,bid_volume5,"
           "ask_price1,ask_volume1,ask_price2,ask_volume2,ask_price3,ask_volume3,ask_price4,"
           "ask_volume4,ask_price5,ask_volume5\n";
    sink.header_written = true;
  }
  auto [ins, ok] = sinks_.emplace(key, std::move(sink));
  (void)ok;
  return &ins->second;
}

void TickDayStore::WriteRow(FileSink& sink, const DepthTick& tick) {
  sink.out << ShanghaiDateTimeCsv(tick) << ',' << tick.exchange_id << ',' << tick.instrument_id
           << ',' << tick.trading_day << ',' << tick.action_day << ',' << tick.update_time << ','
           << tick.update_millisec << ',' << tick.last_price << ',' << tick.volume << ','
           << tick.turnover << ',' << tick.open_interest << ',' << tick.pre_open_interest;
  for (int i = 0; i < 5; ++i) {
    sink.out << ',' << tick.bid_price[i] << ',' << tick.bid_volume[i];
  }
  for (int i = 0; i < 5; ++i) {
    sink.out << ',' << tick.ask_price[i] << ',' << tick.ask_volume[i];
  }
  sink.out << '\n';
  ++pending_;
  ++total_written_;
}

void TickDayStore::Append(const DepthTick& tick) {
  std::string day = tick.trading_day;
  if (day.empty() || day[0] == '\0') {
    day = "unknown";
  }
  const std::string id = tick.InstrumentId();
  if (id.empty()) return;
  std::lock_guard<std::mutex> lock(mu_);
  FileSink* sink = OpenSink(day, id);
  if (!sink) return;
  WriteRow(*sink, tick);
}

std::size_t TickDayStore::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  const std::size_t n = pending_;
  for (auto& kv : sinks_) {
    if (kv.second.out.is_open()) kv.second.out.flush();
  }
  pending_ = 0;
  return n;
}

std::size_t TickDayStore::Pending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return pending_;
}

}  // namespace qsev
