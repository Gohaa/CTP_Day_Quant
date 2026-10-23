#include "store/chip_profile.h"

#include "logging/logger.h"
#include "store/dominant_table.h"
#include "store/instrument_store.h"
#include "store/session_calendar.h"
#include "util/time_shanghai.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace qsev {
namespace {

namespace fs = std::filesystem;

constexpr double kEps = 1e-12;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string ProductCode(std::string id) {
  if (id.size() >= 2 && id[0] == '\xEF') {
    // ignore; callers pass stems
  }
  auto dot = id.find('.');
  if (dot != std::string::npos) id = id.substr(dot + 1);
  while (!id.empty() && std::isdigit(static_cast<unsigned char>(id.back()))) id.pop_back();
  return id;
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_q = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') {
      in_q = !in_q;
    } else if (c == ',' && !in_q) {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

struct CsvCols {
  std::unordered_map<std::string, int> idx;
  int Get(std::initializer_list<const char*> names) const {
    for (const char* n : names) {
      auto it = idx.find(Lower(n));
      if (it != idx.end()) return it->second;
    }
    return -1;
  }
};

CsvCols ParseHeader(std::string header) {
  if (header.size() >= 3 && static_cast<unsigned char>(header[0]) == 0xEF) {
    header = header.substr(3);
  }
  CsvCols c;
  auto parts = SplitCsvLine(header);
  for (int i = 0; i < static_cast<int>(parts.size()); ++i) {
    c.idx[Lower(Trim(parts[i]))] = i;
  }
  return c;
}

double Cell(const std::vector<std::string>& row, int i) {
  if (i < 0 || i >= static_cast<int>(row.size())) return 0;
  const std::string s = Trim(row[i]);
  if (s.empty()) return 0;
  try {
    return std::stod(s);
  } catch (...) {
    return 0;
  }
}

int64_t ParseChipDateTime(const std::string& raw) {
  const std::string s = Trim(raw);
  if (s.size() < 16) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 &&
      std::sscanf(s.c_str(), "%d/%d/%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
    return 0;
  }
  if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  return ShanghaiWallToUnixSec(CivilDate{y, mo, d}, h * 3600 + mi * 60 + sec);
}

std::string FormatShanghaiSec(int64_t unix_sec) {
  if (unix_sec <= 0) return {};
  const auto w = UnixSecToShanghai(unix_sec);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", w.date.year, w.date.month,
                w.date.day, w.hour, w.minute, w.second);
  return buf;
}

SessionCalendar LoadChipSessions(const std::string& data_root) {
  const fs::path latest = fs::path(data_root) / "live" / "sessions" / "latest.json";
  std::error_code ec;
  if (fs::exists(latest, ec)) return SessionCalendar::LoadFromFile(latest.string());
  return {};
}

int64_t LastUnixOfTicks(const std::vector<ChipTick>& ticks) {
  int64_t last = 0;
  for (const auto& t : ticks) {
    if (t.unix_sec > last) last = t.unix_sec;
  }
  return last;
}

int64_t LastUnixOfBars(const std::vector<ChipBar>& bars) {
  int64_t last = 0;
  for (const auto& b : bars) {
    if (b.unix_sec > last) last = b.unix_sec;
  }
  return last;
}

void KeepTicksFrom(std::vector<ChipTick>& ticks, int64_t open_unix) {
  if (open_unix <= 0) return;
  ticks.erase(std::remove_if(ticks.begin(), ticks.end(),
                             [open_unix](const ChipTick& t) {
                               return t.unix_sec > 0 && t.unix_sec < open_unix;
                             }),
              ticks.end());
}

void KeepBarsFrom(std::vector<ChipBar>& bars, int64_t open_unix) {
  if (open_unix <= 0) return;
  bars.erase(std::remove_if(bars.begin(), bars.end(),
                            [open_unix](const ChipBar& b) {
                              return b.unix_sec > 0 && b.unix_sec < open_unix;
                            }),
             bars.end());
}

// Keep the session that contains the last print (openctp /times clocks).
// A new night with only 21:00–23:00 data stays on that night; daytime slots stay empty.
// Do NOT rewind to the previous 21:00→15:00 session.
int64_t ApplyTickSessionWindow(std::vector<ChipTick>& ticks, const SessionCalendar& cal,
                               const std::string& symbol) {
  if (cal.Empty() || ticks.empty()) return 0;
  const int64_t last = LastUnixOfTicks(ticks);
  const int64_t open_unix = cal.SessionOpenUnix(symbol, last);
  KeepTicksFrom(ticks, open_unix);
  ticks.erase(std::remove_if(ticks.begin(), ticks.end(),
                             [&](const ChipTick& t) {
                               if (t.unix_sec <= 0) return false;
                               return !cal.IsInSession(symbol, t.unix_sec * 1000000000LL);
                             }),
              ticks.end());
  return open_unix;
}

int64_t ApplyBarSessionWindow(std::vector<ChipBar>& bars, const SessionCalendar& cal,
                              const std::string& symbol) {
  if (cal.Empty() || bars.empty()) return 0;
  const int64_t last = LastUnixOfBars(bars);
  const int64_t open_unix = cal.SessionOpenUnix(symbol, last);
  KeepBarsFrom(bars, open_unix);
  bars.erase(std::remove_if(bars.begin(), bars.end(),
                            [&](const ChipBar& b) {
                              if (b.unix_sec <= 0) return false;
                              return !cal.IsInSession(symbol, b.unix_sec * 1000000000LL);
                            }),
             bars.end());
  return open_unix;
}

int64_t PriceBin(double px, double tick) {
  if (!(tick > 0) || !std::isfinite(px)) return 0;
  return static_cast<int64_t>(std::llround(px / tick));
}

double BinPrice(int64_t bin, double tick) { return static_cast<double>(bin) * tick; }

double InferTick(const std::vector<double>& px) {
  double dmin = 0;
  for (size_t i = 1; i < px.size(); ++i) {
    const double d = std::fabs(px[i] - px[i - 1]);
    if (d > 1e-9 && (dmin <= 0 || d < dmin)) dmin = d;
  }
  if (dmin <= 0) return 1;
  // snap to 1 / 0.5 / 0.2 / 0.1 / 0.01 ...
  const double mag = std::pow(10.0, std::floor(std::log10(dmin)));
  const double n = dmin / mag;
  if (n < 1.5) return mag;
  if (n < 3) return 2 * mag;
  if (n < 7) return 5 * mag;
  return 10 * mag;
}

void FinalizeProfile(ChipProfile& p) {
  double tot = 0;
  double wsum = 0;
  double poc_vol = -1;
  for (const auto& lv : p.levels) {
    const double v = lv.buy + lv.sell;
    tot += v;
    wsum += v * lv.price;
    if (v > poc_vol) {
      poc_vol = v;
      p.poc = lv.price;
    }
  }
  p.avg_cost = tot > 0 ? wsum / tot : 0;
  double profit = 0;
  for (const auto& lv : p.levels) {
    const double v = lv.buy + lv.sell;
    if (lv.price < p.last - p.price_tick * 0.25) profit += v;
  }
  p.profit_pct = tot > 0 ? 100.0 * profit / tot : 0;

  p.conc90_low = p.poc;
  p.conc90_high = p.poc;
  if (tot <= 0 || p.levels.empty()) return;
  std::vector<ChipLevel> sorted = p.levels;
  std::sort(sorted.begin(), sorted.end(),
            [](const ChipLevel& a, const ChipLevel& b) { return a.price < b.price; });
  int poc_i = 0;
  for (int i = 0; i < static_cast<int>(sorted.size()); ++i) {
    if (std::fabs(sorted[i].price - p.poc) < p.price_tick * 0.25) poc_i = i;
  }
  double acc = sorted[poc_i].buy + sorted[poc_i].sell;
  int lo = poc_i, hi = poc_i;
  const double need = tot * 0.9;
  while (acc < need && (lo > 0 || hi + 1 < static_cast<int>(sorted.size()))) {
    const double left = lo > 0 ? (sorted[lo - 1].buy + sorted[lo - 1].sell) : -1;
    const double right = hi + 1 < static_cast<int>(sorted.size())
                             ? (sorted[hi + 1].buy + sorted[hi + 1].sell)
                             : -1;
    if (right >= left) {
      ++hi;
      acc += sorted[hi].buy + sorted[hi].sell;
    } else {
      --lo;
      acc += sorted[lo].buy + sorted[lo].sell;
    }
  }
  p.conc90_low = sorted[lo].price;
  p.conc90_high = sorted[hi].price;
}

void LevelsFromBuySell(ChipProfile& p, const std::map<int64_t, std::pair<double, double>>& hist,
                       double tick) {
  p.levels.clear();
  if (hist.empty() || !(tick > 0)) return;
  const int64_t lo = hist.begin()->first;
  const int64_t hi = hist.rbegin()->first;
  const int64_t span = hi - lo + 1;
  const bool fill = span > 0 && span <= 2500;
  p.levels.reserve(static_cast<size_t>(fill ? span : hist.size()));
  if (fill) {
    for (int64_t i = hi; i >= lo; --i) {
      ChipLevel lv;
      lv.price = BinPrice(i, tick);
      auto it = hist.find(i);
      if (it != hist.end()) {
        lv.buy = it->second.first;
        lv.sell = it->second.second;
      }
      p.levels.push_back(lv);
    }
    return;
  }
  for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
    ChipLevel lv;
    lv.price = BinPrice(it->first, tick);
    lv.buy = it->second.first;
    lv.sell = it->second.second;
    if (lv.buy + lv.sell <= 0) continue;
    p.levels.push_back(lv);
  }
}

double TriangleY(double p, double lo, double hi, double avg) {
  const double span = hi - lo;
  if (span <= kEps) return 1.0;
  const double h = 2.0 / span;
  if (p + kEps < avg && avg - lo > kEps) return h * (p - lo) / (avg - lo);
  if (p > avg + kEps && hi - avg > kEps) return h * (hi - p) / (hi - avg);
  return h;
}

void AddDayDist(std::map<int64_t, double>& today, const ChipBar& b, double tick, ChipDist dist) {
  if (b.volume <= kEps || !(tick > 0)) return;
  double lo = std::min(b.low, b.high);
  double hi = std::max(b.low, b.high);
  if (!std::isfinite(lo) || !std::isfinite(hi)) return;
  if (hi - lo < tick * 0.25) {
    const double px = std::isfinite(b.close) && b.close > 0 ? b.close : hi;
    today[PriceBin(px, tick)] += 1.0;
    return;
  }
  double avg = (b.high + b.low + b.close) / 3.0;
  if (!std::isfinite(avg) || avg < lo) avg = lo;
  if (avg > hi) avg = hi;
  const int64_t i0 = PriceBin(lo, tick);
  const int64_t i1 = PriceBin(hi, tick);
  std::vector<std::pair<int64_t, double>> w;
  double sum = 0;
  for (int64_t i = i0; i <= i1; ++i) {
    const double p = BinPrice(i, tick);
    double wt = 0;
    if (dist == ChipDist::Uniform) {
      wt = 1.0;
    } else {
      wt = TriangleY(p, lo, hi, avg);
    }
    if (wt > 0) {
      w.push_back({i, wt});
      sum += wt;
    }
  }
  if (sum <= kEps) {
    today[PriceBin(b.close, tick)] += 1.0;
    return;
  }
  for (const auto& kv : w) today[kv.first] += kv.second / sum;
}

std::vector<ChipTick> LoadTicksCsv(const std::string& path) {
  std::ifstream in(path);
  std::vector<ChipTick> out;
  if (!in) return out;
  std::string line;
  if (!std::getline(in, line)) return out;
  const CsvCols cols = ParseHeader(line);
  const int i_last = cols.Get({"LastPrice", "last_price"});
  const int i_qty = cols.Get({"LastQty", "last_qty"});
  const int i_vol = cols.Get({"TotalQty", "volume"});
  const int i_bid = cols.Get({"BuyPrice", "bid_price1", "bid"});
  const int i_ask = cols.Get({"SellPrice", "ask_price1", "ask"});
  const int i_bq = cols.Get({"BuyQty", "bid_volume1", "bid_qty"});
  const int i_aq = cols.Get({"SellQty", "ask_volume1", "ask_qty"});
  const int i_dt = cols.Get({"datetime", "DateTime", "time"});
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto row = SplitCsvLine(line);
    ChipTick t;
    t.last = Cell(row, i_last);
    t.last_qty = Cell(row, i_qty);
    t.volume = Cell(row, i_vol);
    t.bid = Cell(row, i_bid);
    t.ask = Cell(row, i_ask);
    t.bid_qty = Cell(row, i_bq);
    t.ask_qty = Cell(row, i_aq);
    t.unix_sec = ParseChipDateTime(i_dt >= 0 && i_dt < static_cast<int>(row.size()) ? row[i_dt] : "");
    if (!(t.last > 0)) continue;
    out.push_back(t);
  }
  return out;
}

std::vector<ChipBar> LoadBarsCsv(const std::string& path) {
  std::ifstream in(path);
  std::vector<ChipBar> out;
  if (!in) return out;
  std::string line;
  if (!std::getline(in, line)) return out;
  const CsvCols cols = ParseHeader(line);
  const int i_o = cols.Get({"open", "OpeningPrice"});
  const int i_h = cols.Get({"high", "HighPrice"});
  const int i_l = cols.Get({"low", "LowPrice"});
  const int i_c = cols.Get({"close", "LastPrice"});
  const int i_v = cols.Get({"volume", "KLineQty"});
  const int i_oi = cols.Get({"open_interest", "PositionQty", "oi"});
  const int i_dt = cols.Get({"datetime", "DateTime", "time"});
  const int i_unix = cols.Get({"time_unix", "unix"});
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto row = SplitCsvLine(line);
    ChipBar b;
    b.open = Cell(row, i_o);
    b.high = Cell(row, i_h);
    b.low = Cell(row, i_l);
    b.close = Cell(row, i_c);
    b.volume = Cell(row, i_v);
    b.open_interest = Cell(row, i_oi);
    if (i_unix >= 0) {
      const double u = Cell(row, i_unix);
      if (u > 1e9) b.unix_sec = static_cast<int64_t>(u);
    }
    if (b.unix_sec <= 0) {
      b.unix_sec = ParseChipDateTime(i_dt >= 0 && i_dt < static_cast<int>(row.size()) ? row[i_dt] : "");
    }
    if (!(b.high > 0 || b.close > 0 || b.low > 0)) continue;
    if (b.high <= 0) b.high = b.close;
    if (b.low <= 0) b.low = b.close;
    if (b.open <= 0) b.open = b.close;
    out.push_back(b);
  }
  return out;
}

std::string LatestDayDir(const fs::path& root) {
  std::string best;
  std::error_code ec;
  if (!fs::exists(root, ec)) return {};
  for (const auto& ent : fs::directory_iterator(root, ec)) {
    if (!ent.is_directory()) continue;
    const std::string name = ent.path().filename().string();
    if (name.size() == 8 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
          return std::isdigit(c);
        })) {
      if (name > best) best = name;
    }
  }
  return best;
}

std::string FattestCsv(const fs::path& dir, const std::string& product, const std::string& prefer) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  fs::path best;
  uintmax_t best_sz = 0;
  const std::string prod = Lower(product);
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() != ".csv") continue;
    const std::string stem = ent.path().stem().string();
    if (Lower(ProductCode(stem)) != prod) continue;
    const auto sz = fs::file_size(ent.path(), ec);
    if (sz > best_sz) {
      best_sz = sz;
      best = ent.path();
    }
  }
  if (!prefer.empty()) {
    const fs::path p = dir / (prefer + ".csv");
    if (fs::exists(p, ec)) {
      const auto sz = fs::file_size(p, ec);
      if (best.empty() || sz >= best_sz) return p.string();
    }
  }
  return best.string();
}

double TickFromStore(const InstrumentStore& store, const std::string& instrument_id) {
  if (auto t = store.TickSize(instrument_id)) {
    if (*t > 0) return *t;
  }
  const std::string want = Lower(instrument_id);
  const std::string prod = Lower(ProductCode(instrument_id));
  for (const auto& info : store.List()) {
    if (Lower(info.instrument_id) == want && info.price_tick > 0) return info.price_tick;
  }
  for (const auto& info : store.List()) {
    if (Lower(info.product_id) == prod && info.price_tick > 0) return info.price_tick;
  }
  return 0;
}

double LoadPriceTick(const std::string& data_root, const std::string& instrument_id) {
  const fs::path live = fs::path(data_root) / "live" / "instruments";
  std::error_code ec;
  std::vector<fs::path> files;
  auto add_dated = [&](const fs::path& dir) {
    if (!fs::exists(dir, ec)) return;
    std::string best_name;
    fs::path best_path;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
      if (!ent.is_regular_file() || ent.path().extension() != ".json") continue;
      const std::string name = ent.path().stem().string();
      if (name == "latest") continue;
      if (name.size() == 8 && name > best_name) {
        best_name = name;
        best_path = ent.path();
      }
    }
    if (!best_path.empty()) files.push_back(best_path);
    const fs::path latest = dir / "latest.json";
    if (fs::exists(latest, ec)) files.push_back(latest);
  };
  add_dated(live);            // 全市场合约，优先（含非主力月份）
  add_dated(live / "dominant");

  for (const auto& path : files) {
    InstrumentStore store;
    if (!store.LoadJson(path.string())) continue;
    const double t = TickFromStore(store, instrument_id);
    if (t > 0) {
      Logger::Info("chip", "price_tick=" + std::to_string(t) + " id=" + instrument_id +
                               " from " + path.string());
      return t;
    }
  }
  Logger::Warn("chip", "price_tick missing in data/live/instruments for " + instrument_id);
  return 0;
}

void AppendNum(std::string& s, double x) {
  if (!std::isfinite(x)) {
    s += "0";
    return;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", x);
  s += buf;
}

void AppendStr(std::string& s, const std::string& v) {
  s += '"';
  for (char c : v) {
    if (c == '"' || c == '\\') s += '\\';
    s += c;
  }
  s += '"';
}

}  // namespace

ChipAggressor TickAggressor(const ChipTick* prev, const ChipTick& cur) {
  ChipAggressor z;
  if (!prev) return z;
  double qty = cur.last_qty;
  const double dvol = cur.volume - prev->volume;
  if (qty <= 0) qty = dvol > 0 ? dvol : 0;
  if (qty <= 0) return z;
  const double last = cur.last;
  const double p_last = prev->last;
  const double p_bid = prev->bid;
  const double p_ask = prev->ask;
  const double p_bq = prev->bid_qty;
  const double p_aq = prev->ask_qty;
  const double bid = cur.bid;
  const double ask = cur.ask;
  const double bq = cur.bid_qty;
  const double aq = cur.ask_qty;
  if (p_ask > 0 && last + kEps >= p_ask) {
    z.buy = qty;
    return z;
  }
  if (p_bid > 0 && last <= p_bid + kEps) {
    z.sell = qty;
    return z;
  }
  const double eat_ask =
      (p_ask > 0 && std::fabs(ask - p_ask) <= 1e-8 * std::max(1.0, std::fabs(p_ask))) ? (p_aq - aq)
                                                                                      : 0;
  const double eat_bid =
      (p_bid > 0 && std::fabs(bid - p_bid) <= 1e-8 * std::max(1.0, std::fabs(p_bid))) ? (p_bq - bq)
                                                                                      : 0;
  if (eat_ask > 0 && eat_bid <= 0) {
    z.buy = qty;
    return z;
  }
  if (eat_bid > 0 && eat_ask <= 0) {
    z.sell = qty;
    return z;
  }
  if (eat_ask > 0 && eat_bid > 0) {
    const double s = eat_ask + eat_bid;
    z.buy = qty * eat_ask / s;
    z.sell = qty * eat_bid / s;
    return z;
  }
  if (p_last > 0 && last > p_last) {
    z.buy = qty;
    return z;
  }
  if (p_last > 0 && last < p_last) {
    z.sell = qty;
    return z;
  }
  if (p_ask > 0 && p_bid > 0) {
    if (std::fabs(last - p_ask) <= std::fabs(last - p_bid))
      z.buy = qty;
    else
      z.sell = qty;
  }
  return z;
}

ChipProfile BuildTickChip(const std::vector<ChipTick>& ticks, double price_tick) {
  ChipProfile p;
  p.mode = "tick";
  p.tick_count = static_cast<int>(ticks.size());
  if (ticks.empty()) return p;
  if (!(price_tick > 0)) {
    std::vector<double> px;
    px.reserve(ticks.size());
    for (const auto& t : ticks) {
      if (t.last > 0) px.push_back(t.last);
    }
    price_tick = InferTick(px);
  }
  p.price_tick = price_tick;
  p.last = ticks.back().last;
  double tmin = 0, tmax = 0;
  bool have_px = false;
  for (const auto& t : ticks) {
    if (!(t.last > 0)) continue;
    if (!have_px) {
      tmin = tmax = t.last;
      have_px = true;
    } else {
      tmin = std::min(tmin, t.last);
      tmax = std::max(tmax, t.last);
    }
  }
  p.tick_low = tmin;
  p.tick_high = tmax;
  std::map<int64_t, std::pair<double, double>> hist;
  for (size_t i = 1; i < ticks.size(); ++i) {
    const auto a = TickAggressor(&ticks[i - 1], ticks[i]);
    if (a.buy <= 0 && a.sell <= 0) continue;
    if (!(ticks[i].last > 0)) continue;
    auto& bin = hist[PriceBin(ticks[i].last, price_tick)];
    bin.first += a.buy;
    bin.second += a.sell;
  }
  if (have_px && price_tick > 0) {
    hist[PriceBin(tmin, price_tick)];
    hist[PriceBin(tmax, price_tick)];
  }
  LevelsFromBuySell(p, hist, price_tick);
  FinalizeProfile(p);
  return p;
}

ChipProfile BuildKlineChip(const std::vector<ChipBar>& bars, double price_tick, ChipDist dist,
                           double decay) {
  ChipProfile p;
  p.mode = "kline";
  p.bar_count = static_cast<int>(bars.size());
  if (bars.empty()) return p;
  if (!(price_tick > 0)) {
    std::vector<double> px;
    for (const auto& b : bars) {
      if (b.close > 0) px.push_back(b.close);
      if (b.high > 0) px.push_back(b.high);
      if (b.low > 0) px.push_back(b.low);
    }
    price_tick = InferTick(px);
    Logger::Warn("chip", "kline inferred price_tick=" + std::to_string(price_tick));
  }
  p.price_tick = price_tick;
  p.last = bars.back().close > 0 ? bars.back().close : bars.back().high;
  if (decay < 0) decay = 0;

  std::map<int64_t, std::pair<double, double>> hist;  // long, short
  double acc_vol = 0;
  for (const auto& b : bars) {
    if (b.volume <= kEps) continue;
    acc_vol += std::max(0.0, b.volume);
    std::map<int64_t, double> today;
    AddDayDist(today, b, price_tick, dist);
    double turn = 1.0;
    if (b.open_interest > kEps) {
      turn = std::min(1.0, (b.volume / b.open_interest) * decay);
    } else if (acc_vol > kEps) {
      turn = std::min(1.0, (b.volume / acc_vol) * decay);
    }
    if (!std::isfinite(turn) || turn < 0) turn = 0;
    if (turn > 1) turn = 1;
    const bool is_long = b.close >= b.open;
    if (!hist.empty()) {
      for (auto& kv : hist) {
        kv.second.first *= (1.0 - turn);
        kv.second.second *= (1.0 - turn);
      }
    }
    const double add = (hist.empty() ? 1.0 : turn) * b.volume;
    for (const auto& kv : today) {
      if (is_long)
        hist[kv.first].first += add * kv.second;
      else
        hist[kv.first].second += add * kv.second;
    }
  }
  LevelsFromBuySell(p, hist, price_tick);
  FinalizeProfile(p);
  return p;
}

std::string ChipProfileToJson(const ChipProfile& p) {
  std::string s = "{";
  s += "\"ok\":true,\"mode\":";
  AppendStr(s, p.mode);
  s += ",\"symbol\":";
  AppendStr(s, p.symbol);
  s += ",\"instrument_id\":";
  AppendStr(s, p.instrument_id);
  s += ",\"source\":";
  AppendStr(s, p.source);
  s += ",\"price_tick\":";
  AppendNum(s, p.price_tick);
  s += ",\"last\":";
  AppendNum(s, p.last);
  s += ",\"avg_cost\":";
  AppendNum(s, p.avg_cost);
  s += ",\"poc\":";
  AppendNum(s, p.poc);
  s += ",\"profit_pct\":";
  AppendNum(s, p.profit_pct);
  s += ",\"conc90_low\":";
  AppendNum(s, p.conc90_low);
  s += ",\"conc90_high\":";
  AppendNum(s, p.conc90_high);
  s += ",\"tick_count\":";
  s += std::to_string(p.tick_count);
  s += ",\"bar_count\":";
  s += std::to_string(p.bar_count);
  s += ",\"session_open\":";
  AppendStr(s, p.session_open);
  s += ",\"session_open_unix\":";
  s += std::to_string(p.session_open_unix);
  s += ",\"tick_high\":";
  AppendNum(s, p.tick_high);
  s += ",\"tick_low\":";
  AppendNum(s, p.tick_low);
  s += ",\"levels\":[";
  for (size_t i = 0; i < p.levels.size(); ++i) {
    if (i) s += ',';
    s += "{\"p\":";
    AppendNum(s, p.levels[i].price);
    s += ",\"buy\":";
    AppendNum(s, p.levels[i].buy);
    s += ",\"sell\":";
    AppendNum(s, p.levels[i].sell);
    s += '}';
  }
  s += "]}";
  return s;
}

int PriceDecimals(double tick) {
  if (!(tick > 0) || tick >= 1) return 0;
  int d = 0;
  double t = tick;
  while (t + 1e-12 < 1.0 && d < 8) {
    t *= 10.0;
    ++d;
  }
  return d;
}

std::string ChipProfileToCsv(const ChipProfile& p) {
  double tot = 0;
  for (const auto& lv : p.levels) tot += lv.buy + lv.sell;
  const int dec = PriceDecimals(p.price_tick);
  std::string s;
  s += "price,long_qty,short_qty,volume,pct\n";
  char row[256];
  for (const auto& lv : p.levels) {
    const double vol = lv.buy + lv.sell;
    const double pct = tot > 0 ? 100.0 * vol / tot : 0;
    std::snprintf(row, sizeof(row), "%.*f,%.10g,%.10g,%.10g,%.4f\n", dec, lv.price, lv.buy, lv.sell,
                  vol, pct);
    s += row;
  }
  return s;
}

int RunChipSelfTest() {
  int fail = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) {
      std::cerr << "chip selftest FAIL " << name << "\n";
      ++fail;
    }
  };

  ChipTick a;
  a.last = 100;
  a.bid = 99;
  a.ask = 101;
  a.bid_qty = 10;
  a.ask_qty = 8;
  a.volume = 1000;
  ChipTick b = a;
  b.last = 101;
  b.last_qty = 5;
  b.volume = 1005;
  b.ask_qty = 3;
  auto hit_ask = TickAggressor(&a, b);
  check(std::fabs(hit_ask.buy - 5) < 1e-9 && hit_ask.sell == 0, "hit_prev_ask");

  ChipTick c = a;
  c.last = 99;
  c.last_qty = 4;
  c.volume = 1004;
  auto hit_bid = TickAggressor(&a, c);
  check(std::fabs(hit_bid.sell - 4) < 1e-9 && hit_bid.buy == 0, "hit_prev_bid");

  ChipTick quote = b;
  quote.last_qty = 0;
  quote.volume = a.volume;
  auto q = TickAggressor(&a, quote);
  check(q.buy == 0 && q.sell == 0, "quote_only");

  std::vector<ChipTick> ticks = {a, b, c};
  auto tp = BuildTickChip(ticks, 1);
  check(tp.levels.size() >= 2, "tick_levels");

  ChipBar bar;
  bar.open = 10;
  bar.high = 10;
  bar.low = 10;
  bar.close = 10;
  bar.volume = 100;
  auto kp = BuildKlineChip({bar}, 1, ChipDist::Triangle, 1.0);
  check(kp.levels.size() == 1, "kline_flat");
  check(std::fabs(kp.levels[0].price - 10) < 1e-9, "kline_flat_px");
  check(kp.levels[0].buy > 0 && kp.levels[0].sell == 0, "kline_flat_long");
  const double mass0 = kp.levels[0].buy + kp.levels[0].sell;
  check(std::fabs(mass0 - 100.0) < 1e-6, "kline_mass_vol");

  ChipBar down;
  down.open = 11;
  down.high = 11;
  down.low = 9;
  down.close = 9;
  down.volume = 50;
  ChipBar up;
  up.open = 9;
  up.high = 11;
  up.low = 9;
  up.close = 11;
  up.volume = 50;
  auto mix = BuildKlineChip({down, up}, 1, ChipDist::Uniform, 1.0);
  bool both = false;
  for (const auto& lv : mix.levels) {
    if (lv.buy > 0 && lv.sell > 0) both = true;
  }
  check(both, "kline_long_and_short");

  ChipBar b2 = bar;
  b2.high = 12;
  b2.low = 8;
  b2.close = 12;
  b2.volume = 100;
  b2.open_interest = 100;
  auto kp2 = BuildKlineChip({bar, b2}, 1, ChipDist::Triangle, 1.0);
  check(!kp2.levels.empty(), "kline_move");
  check(kp2.last == 12, "kline_last");

  {
    const std::vector<TradingSegment> day_only = {{1, "09:30:00", "11:30:00"},
                                                  {2, "13:00:00", "15:00:00"}};
    const int64_t last_day = ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 14 * 3600);
    check(SessionOpenUnixFromLast(day_only, last_day) ==
              ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 9 * 3600 + 30 * 60),
          "session_day_open");
    const std::vector<TradingSegment> night = {{1, "21:00:00", "01:00:00"},
                                               {2, "09:00:00", "10:15:00"},
                                               {3, "10:30:00", "11:30:00"},
                                               {4, "13:30:00", "15:00:00"}};
    const int64_t last_aft = ShanghaiWallToUnixSec(CivilDate{2026, 8, 22}, 14 * 3600);
    check(SessionOpenUnixFromLast(night, last_aft) ==
              ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 21 * 3600),
          "session_night_from_day");
    const int64_t last_ow = ShanghaiWallToUnixSec(CivilDate{2026, 8, 22}, 30 * 60);
    check(SessionOpenUnixFromLast(night, last_ow) ==
              ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 21 * 3600),
          "session_night_overnight");
    const int64_t last_n = ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 22 * 3600);
    check(SessionOpenUnixFromLast(night, last_n) ==
              ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 21 * 3600),
          "session_night_same_evening");
    const int64_t last_early = ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 21 * 3600 + 60);
    check(SessionOpenUnixFromLast(night, last_early) ==
              ShanghaiWallToUnixSec(CivilDate{2026, 8, 21}, 21 * 3600),
          "session_night_just_opened");
  }

  if (fail == 0) {
    std::cout << "{\"ok\":true,\"selftest\":true}\n";
    Logger::Info("chip", "selftest ok");
  }
  return fail == 0 ? 0 : 11;
}

int RunChip(const ChipCliOptions& opt, const std::string& data_root) {
  if (opt.selftest) return RunChipSelfTest();

  ChipCliOptions o = opt;
  if (o.mode != "kline") o.mode = "tick";
  const ChipDist dist = (o.dist == "uniform") ? ChipDist::Uniform : ChipDist::Triangle;

  std::string inst;
  std::string symbol = o.symbol;
  try {
    const DominantTable table = DominantTable::LoadLatest(data_root);
    for (const auto& e : table.Entries()) {
      if (e.product_key == symbol || e.dominant == symbol || e.second == symbol) {
        if (!e.dominant.empty()) inst = e.dominant;
        if (!e.product_key.empty() && (e.product_key == symbol || e.dominant == symbol)) {
          symbol = e.product_key;
        }
      }
    }
    if (inst.empty()) {
      for (const auto& e : table.Entries()) {
        if (Lower(ProductCode(e.dominant)) == Lower(ProductCode(symbol)) ||
            Lower(e.product_key) == Lower(symbol)) {
          inst = e.dominant;
          symbol = e.product_key.empty() ? symbol : e.product_key;
          break;
        }
      }
    }
  } catch (const std::exception& e) {
    Logger::Warn("chip", std::string("dominant table: ") + e.what());
  }
  if (inst.empty()) inst = symbol;

  std::string day = o.day;
  if (day.empty()) {
    // Live chip = current CTP TradingDay under data/ticks/. 极智 ep_raw folder date can lag
    // (Monday night is ticks/20260825 while ep_raw still 20260824).
    const std::string tick_day = LatestDayDir(fs::path(data_root) / "ticks");
    const std::string ep_day = LatestDayDir(fs::path(data_root) / "live" / "ep_raw");
    if (!tick_day.empty() && (ep_day.empty() || tick_day >= ep_day))
      day = tick_day;
    else
      day = ep_day;
  }

  ChipProfile profile;
  const std::string prod = ProductCode(symbol.empty() ? inst : symbol);

  if (o.mode == "tick") {
    std::string path = o.ticks_csv;
    if (path.empty()) {
      path = FattestCsv(fs::path(data_root) / "ticks" / day, prod, inst);
      if (path.empty()) {
        path = FattestCsv(fs::path(data_root) / "live" / "ep_raw" / day / "tick", prod, inst);
      }
    }
    if (path.empty()) {
      Logger::Error("chip", "no tick csv for " + symbol + " day=" + day);
      std::cout << "{\"ok\":false,\"error\":\"no tick csv\"}\n";
      return 12;
    }
    {
      const std::string file_id = fs::path(path).stem().string();
      double tick = LoadPriceTick(data_root, file_id);
      if (!(tick > 0) && !inst.empty()) tick = LoadPriceTick(data_root, inst);
      inst = file_id;
      auto ticks = LoadTicksCsv(path);
      auto cal = LoadChipSessions(data_root);
      int64_t open_unix = ApplyTickSessionWindow(ticks, cal, symbol);
      profile = BuildTickChip(ticks, tick);
      profile.session_open_unix = open_unix;
      profile.session_open = FormatShanghaiSec(open_unix);
    }
    profile.source = path;
  } else {
    std::string path = o.bars_csv;
    std::vector<ChipBar> bars;
    if (!path.empty()) {
      bars = LoadBarsCsv(path);
    } else {
      std::error_code ec;
      const fs::path ep = fs::path(data_root) / "live" / "ep_raw";
      if (fs::exists(ep, ec)) {
        std::vector<std::string> days;
        for (const auto& ent : fs::directory_iterator(ep, ec)) {
          if (!ent.is_directory()) continue;
          const std::string name = ent.path().filename().string();
          if (name.size() == 8 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isdigit(c);
              })) {
            days.push_back(name);
          }
        }
        std::sort(days.begin(), days.end());
        for (const auto& d : days) {
          const std::string p = FattestCsv(ep / d / "m1", prod, inst);
          if (p.empty()) continue;
          auto part = LoadBarsCsv(p);
          bars.insert(bars.end(), part.begin(), part.end());
          path = p;
          if (inst.empty()) inst = fs::path(p).stem().string();
        }
      }
    }
    if (o.lookback > 0 && static_cast<int>(bars.size()) > o.lookback) {
      bars.erase(bars.begin(), bars.end() - o.lookback);
    }
    int64_t open_unix = 0;
    if (o.session_window) {
      auto cal = LoadChipSessions(data_root);
      open_unix = ApplyBarSessionWindow(bars, cal, symbol);
    }
    if (bars.empty()) {
      Logger::Error("chip", "no kline bars for " + symbol);
      std::cout << "{\"ok\":false,\"error\":\"no kline bars\"}\n";
      return 12;
    }
    if (o.bars_csv.empty() && !path.empty()) {
      inst = fs::path(path).stem().string();
    } else if (inst.empty() && !path.empty()) {
      inst = fs::path(path).stem().string();
    }
    double tick = LoadPriceTick(data_root, inst);
    profile = BuildKlineChip(bars, tick, dist, o.decay);
    profile.source = path.empty() ? "bars-csv" : path;
    profile.session_open_unix = open_unix;
    profile.session_open = FormatShanghaiSec(open_unix);
  }

  profile.symbol = symbol;
  profile.instrument_id = inst;
  const std::string json = ChipProfileToJson(profile);
  const std::string csv = ChipProfileToCsv(profile);

  std::string out = o.out_path;
  if (out.empty()) {
    const fs::path dir = fs::path(data_root) / "live" / "chip";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = inst.empty() ? ProductCode(symbol) : inst;
    out = (dir / (stem + "_" + o.mode + ".csv")).string();
  }
  {
    std::ofstream f(out, std::ios::binary);
    if (f) {
      if (out.size() >= 4 && Lower(out.substr(out.size() - 4)) == ".csv")
        f << csv;
      else
        f << json;
    }
    Logger::Info("chip", "wrote " + out + " levels=" + std::to_string(profile.levels.size()) +
                             " price_tick=" + std::to_string(profile.price_tick));
  }
  std::cout << json << "\n";
  return 0;
}

bool TryLoadSessionTickChip(const std::string& data_root, const std::string& symbol_in, ChipProfile* out,
                            const std::string& instrument_id) {
  if (!out) return false;
  ChipCliOptions o;
  o.symbol = symbol_in;
  o.mode = "tick";
  std::string inst = instrument_id;
  std::string symbol = o.symbol;
  if (inst.empty()) {
    try {
      const DominantTable table = DominantTable::LoadLatest(data_root);
      for (const auto& e : table.Entries()) {
        if (e.product_key == symbol || e.dominant == symbol || e.second == symbol) {
          if (!e.dominant.empty()) inst = e.dominant;
          if (!e.product_key.empty() && (e.product_key == symbol || e.dominant == symbol)) {
            symbol = e.product_key;
          }
        }
      }
    } catch (...) {
    }
  }
  if (inst.empty()) inst = symbol;
  const std::string tick_day = LatestDayDir(fs::path(data_root) / "ticks");
  const std::string ep_day = LatestDayDir(fs::path(data_root) / "live" / "ep_raw");
  std::string day;
  if (!tick_day.empty() && (ep_day.empty() || tick_day >= ep_day))
    day = tick_day;
  else
    day = ep_day;
  const std::string prod = ProductCode(symbol.empty() ? inst : symbol);
  std::string path = FattestCsv(fs::path(data_root) / "ticks" / day, prod, inst);
  if (path.empty()) path = FattestCsv(fs::path(data_root) / "live" / "ep_raw" / day / "tick", prod, inst);
  if (path.empty()) return false;
  const std::string file_id = fs::path(path).stem().string();
  double tick = LoadPriceTick(data_root, file_id);
  if (!(tick > 0) && !inst.empty()) tick = LoadPriceTick(data_root, inst);
  auto ticks = LoadTicksCsv(path);
  auto cal = LoadChipSessions(data_root);
  const int64_t open_unix = ApplyTickSessionWindow(ticks, cal, symbol);
  *out = BuildTickChip(ticks, tick);
  out->session_open_unix = open_unix;
  out->session_open = FormatShanghaiSec(open_unix);
  out->source = path;
  out->symbol = symbol;
  out->instrument_id = file_id;
  return out->tick_count > 0 || !out->levels.empty();
}

}  // namespace qsev
