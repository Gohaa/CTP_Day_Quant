#include "market/quote_tape.h"

#include "logging/logger.h"
#include "market/parquet_bar_store.h"
#include "util/time_shanghai.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qsev {
namespace {

namespace fs = std::filesystem;

constexpr int kPrioEpTick = 0;
constexpr int kPrioCtp = 1;
constexpr int kPrioEpM1 = 3;
constexpr int kSparkN = 64;

struct MinBar {
  int64_t open_sec{0};
  double open{0}, high{0}, low{0}, close{0}, volume{0};
  int prio{0};
  bool has{false};
};

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> SplitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_q = false;
  for (char c : line) {
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
  if (header.size() >= 3 && static_cast<unsigned char>(header[0]) == 0xEF) header = header.substr(3);
  CsvCols c;
  auto parts = SplitCsv(header);
  for (int i = 0; i < static_cast<int>(parts.size()); ++i) {
    c.idx[Lower(Trim(parts[i]))] = i;
  }
  return c;
}

std::string CellS(const std::vector<std::string>& row, int i) {
  if (i < 0 || i >= static_cast<int>(row.size())) return {};
  return Trim(row[static_cast<size_t>(i)]);
}

double CellD(const std::vector<std::string>& row, int i) {
  const std::string s = CellS(row, i);
  if (s.empty()) return 0;
  try {
    return std::stod(s);
  } catch (...) {
    return 0;
  }
}

int64_t ParseWallSec(const std::string& raw) {
  const std::string s = Trim(raw);
  if (s.size() < 16) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, ms = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &sec, &ms) != 7 &&
      std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 &&
      std::sscanf(s.c_str(), "%d/%d/%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &sec, &ms) != 7 &&
      std::sscanf(s.c_str(), "%d/%d/%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
    return 0;
  }
  if (y < 1990 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  return ShanghaiWallToUnixSec(CivilDate{y, mo, d}, h * 3600 + mi * 60 + sec);
}

CivilDate PrevBusinessDay(CivilDate d) {
  d = AddDays(d, -1);
  while (IsWeekend(d)) d = AddDays(d, -1);
  return d;
}

// 极智 Tick：DateTimeStamp 前 8 位 = CTP TradingDay，后 6 位 = 时钟。
// 周一夜盘 21:00 → 上周五；00:15 → 周六凌晨。不能当成交易日当天的墙钟。
int64_t WallFromTradingDayClock(const CivilDate& td, int hour, int minute, int second) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
  CivilDate cal = td;
  if (hour >= 20) {
    cal = PrevBusinessDay(td);
  } else if (hour < 3) {
    cal = AddDays(PrevBusinessDay(td), 1);
  }
  return ShanghaiWallToUnixSec(cal, hour * 3600 + minute * 60 + second);
}

std::string DigitsOnly(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isdigit(c)) o.push_back(static_cast<char>(c));
  }
  return o;
}

int64_t ParseEpTickWall(const std::string& datetime, const std::string& trade_date,
                        const std::string& stamp) {
  const std::string td = DigitsOnly(trade_date);
  const std::string st = DigitsOnly(stamp);
  if (td.size() >= 8 && st.size() >= 14 && st.compare(0, 8, td, 0, 8) == 0) {
    CivilDate day{};
    if (ParseYyyymmdd(td.substr(0, 8), &day)) {
      int hh = 0, mm = 0, ss = 0;
      if (std::sscanf(st.c_str() + 8, "%2d%2d%2d", &hh, &mm, &ss) == 3) {
        const int64_t w = WallFromTradingDayClock(day, hh, mm, ss);
        if (w > 0) return w;
      }
    }
  }
  return ParseWallSec(datetime);
}

int64_t BarOpenSec(int64_t unix_sec) {
  if (unix_sec <= 0) return 0;
  return AlignBarOpenNsShanghai(unix_sec * 1000000000LL, 1) / 1000000000LL;
}

std::string FmtWall(int64_t unix_sec) {
  if (unix_sec <= 0) return {};
  const auto w = UnixSecToShanghai(unix_sec);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", w.date.year, w.date.month, w.date.day,
                w.hour, w.minute, w.second);
  return buf;
}

std::string JsonEsc(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string Num(double x, int digits) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", digits, x);
  return buf;
}

void AccPx(MinBar& b, double px, double qty) {
  if (!(px > 0) || !std::isfinite(px)) return;
  if (!b.has) {
    b.open = b.high = b.low = b.close = px;
    b.has = true;
  } else {
    if (px > b.high) b.high = px;
    if (px < b.low) b.low = px;
    b.close = px;
  }
  if (qty > 0 && std::isfinite(qty)) b.volume += qty;
}

void Upsert(std::map<int64_t, MinBar>& bars, MinBar b) {
  if (!b.has || b.open_sec <= 0) return;
  auto it = bars.find(b.open_sec);
  if (it == bars.end() || b.prio >= it->second.prio) bars[b.open_sec] = b;
}

void MergeMap(std::map<int64_t, MinBar>& dst, const std::map<int64_t, MinBar>& src) {
  for (const auto& kv : src) Upsert(dst, kv.second);
}

struct FileKey {
  std::string path;
  std::uintmax_t size{0};
  std::uintmax_t mtime{0};
  bool operator==(const FileKey& o) const {
    return path == o.path && size == o.size && mtime == o.mtime;
  }
};

FileKey StatFile(const fs::path& p) {
  FileKey k;
  k.path = p.string();
  std::error_code ec;
  k.size = fs::file_size(p, ec);
  const auto t = fs::last_write_time(p, ec);
  if (!ec) k.mtime = static_cast<std::uintmax_t>(t.time_since_epoch().count());
  return k;
}

// Cache 极智 files (they don't grow during --live-ticks).
std::unordered_map<std::string, std::pair<FileKey, std::map<int64_t, MinBar>>> g_file_cache;

bool CacheHit(const FileKey& key, std::map<int64_t, MinBar>* out) {
  auto it = g_file_cache.find(key.path);
  if (it == g_file_cache.end()) return false;
  if (!(it->second.first == key)) return false;
  *out = it->second.second;
  return true;
}

std::map<int64_t, MinBar> LoadM1Csv(const fs::path& path, int prio) {
  std::map<int64_t, MinBar> out;
  FileKey key = StatFile(path);
  if (CacheHit(key, &out)) {
    for (auto& kv : out) kv.second.prio = prio;
    return out;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::string line;
  if (!std::getline(in, line)) return out;
  const CsvCols cols = ParseHeader(line);
  const int i_dt = cols.Get({"datetime"});
  const int i_o = cols.Get({"open"});
  const int i_h = cols.Get({"high"});
  const int i_l = cols.Get({"low"});
  const int i_c = cols.Get({"close"});
  const int i_v = cols.Get({"volume"});
  if (i_dt < 0 || i_c < 0) return out;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto row = SplitCsv(line);
    const int64_t sec = BarOpenSec(ParseWallSec(CellS(row, i_dt)));
    const double c = CellD(row, i_c);
    if (sec <= 0 || !(c > 0)) continue;
    MinBar b;
    b.open_sec = sec;
    b.open = i_o >= 0 ? CellD(row, i_o) : c;
    b.high = i_h >= 0 ? CellD(row, i_h) : c;
    b.low = i_l >= 0 ? CellD(row, i_l) : c;
    b.close = c;
    b.volume = i_v >= 0 ? CellD(row, i_v) : 0;
    if (!(b.open > 0)) b.open = c;
    if (!(b.high > 0)) b.high = c;
    if (!(b.low > 0)) b.low = c;
    b.has = true;
    b.prio = prio;
    Upsert(out, b);
  }
  g_file_cache[key.path] = {key, out};
  return out;
}

std::map<int64_t, MinBar> LoadTickCsv(const fs::path& path, int prio, bool ctp_cum_volume,
                                      double* last_px, double* bid, double* ask, double* bid_qty,
                                      double* ask_qty) {
  std::map<int64_t, MinBar> out;
  // CTP files grow every few seconds — do not cache.
  const bool use_cache = prio != kPrioCtp;
  FileKey key;
  if (use_cache) {
    key = StatFile(path);
    if (CacheHit(key, &out)) {
      for (auto& kv : out) kv.second.prio = prio;
      return out;
    }
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return out;
  std::string line;
  if (!std::getline(in, line)) return out;
  const CsvCols cols = ParseHeader(line);
  const int i_dt = cols.Get({"datetime"});
  const int i_td = cols.Get({"tradedate"});
  const int i_stamp = cols.Get({"datetimestamp"});
  const int i_px = cols.Get({"lastprice", "last_price"});
  const int i_qty = cols.Get({"lastqty", "last_qty"});
  const int i_vol = cols.Get({"volume", "totalqty"});
  const int i_bid = cols.Get({"bid_price1", "buyprice"});
  const int i_ask = cols.Get({"ask_price1", "sellprice"});
  const int i_bv = cols.Get({"bid_volume1", "buyqty"});
  const int i_av = cols.Get({"ask_volume1", "sellqty"});
  if (i_dt < 0 || i_px < 0) return out;

  std::map<int64_t, MinBar> acc;
  double prev_cum = -1;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto row = SplitCsv(line);
    const int64_t wall = ParseEpTickWall(CellS(row, i_dt),
                                        i_td >= 0 ? CellS(row, i_td) : "",
                                        i_stamp >= 0 ? CellS(row, i_stamp) : "");
    const int64_t sec = BarOpenSec(wall);
    const double px_raw = CellD(row, i_px);
    if (sec <= 0 || !(px_raw > 0) || !std::isfinite(px_raw) || px_raw > 1.0e15) continue;
    double px = px_raw;
    if (i_bid >= 0 && i_ask >= 0) {
      const double bpx = CellD(row, i_bid);
      const double apx = CellD(row, i_ask);
      if (bpx > 0 && apx > 0) {
        const double mid = 0.5 * (bpx + apx);
        if (mid > 0 && (px < mid * 0.5 || px > mid * 2.0)) continue;
      }
    }
    double qty = 0;
    if (i_qty >= 0) {
      qty = CellD(row, i_qty);
    } else if (i_vol >= 0) {
      const double cum = CellD(row, i_vol);
      if (ctp_cum_volume || prev_cum >= 0) {
        if (prev_cum >= 0 && cum >= prev_cum) qty = cum - prev_cum;
      }
      // Mid-session attach: first nonzero Volume is session cumulative, not 1-minute volume.
      if (ctp_cum_volume && prev_cum <= 0 && qty > 10000 && qty >= cum * 0.5) qty = 0;
      prev_cum = cum;
    }
    auto& b = acc[sec];
    b.open_sec = sec;
    b.prio = prio;
    AccPx(b, px, qty);
    if (last_px) *last_px = px;
    if (bid && i_bid >= 0) {
      const double v = CellD(row, i_bid);
      if (v > 0) *bid = v;
    }
    if (ask && i_ask >= 0) {
      const double v = CellD(row, i_ask);
      if (v > 0) *ask = v;
    }
    if (bid_qty && i_bv >= 0) *bid_qty = CellD(row, i_bv);
    if (ask_qty && i_av >= 0) *ask_qty = CellD(row, i_av);
  }
  out = std::move(acc);
  if (use_cache) g_file_cache[key.path] = {key, out};
  return out;
}

std::vector<fs::path> ListDayDirs(const fs::path& root) {
  std::vector<fs::path> days;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return days;
  for (const auto& ent : fs::directory_iterator(root, ec)) {
    if (!ent.is_directory()) continue;
    const std::string name = ent.path().filename().string();
    if (name.size() == 8 && std::all_of(name.begin(), name.end(),
                                        [](unsigned char c) { return std::isdigit(c); })) {
      days.push_back(ent.path());
    }
  }
  std::sort(days.begin(), days.end());
  return days;
}

std::vector<int> SessionSlots(const std::vector<TradingSegment>* segs) {
  std::vector<int> out;
  if (!segs) return out;
  auto add = [&](int b, int e) {
    if (b < 0 || e < 0) return;
    if (e < b) {
      for (int s = b; s < 86400; s += 60) out.push_back(s);
      for (int s = 0; s < e; s += 60) out.push_back(s);
    } else {
      for (int s = b; s < e; s += 60) out.push_back(s);
    }
  };
  std::vector<std::pair<int, int>> night, day;
  for (const auto& s : *segs) {
    const int b = ParseHmsToSec(s.time_begin);
    const int e = ParseHmsToSec(s.time_end);
    if (b < 0 || e < 0) continue;
    if (b >= 20 * 3600) night.emplace_back(b, e);
    else day.emplace_back(b, e);
  }
  for (const auto& p : night) add(p.first, p.second);
  std::sort(day.begin(), day.end());
  for (const auto& p : day) add(p.first, p.second);
  return out;
}

std::vector<std::string> Sparkline(const std::vector<MinBar>& sess, const std::vector<int>& slots) {
  std::vector<std::string> out;
  if (sess.empty()) return out;
  auto down = [&](const std::vector<std::string>& vals) {
    std::vector<std::string> d;
    if (vals.empty()) return d;
    if (static_cast<int>(vals.size()) <= kSparkN) return vals;
    d.reserve(kSparkN);
    const int last = static_cast<int>(vals.size()) - 1;
    for (int i = 0; i < kSparkN; ++i) {
      d.push_back(vals[static_cast<size_t>(std::lround(i * last / static_cast<double>(kSparkN - 1)))]);
    }
    return d;
  };
  if (slots.empty()) {
    std::vector<std::string> raw;
    raw.reserve(sess.size());
    for (const auto& b : sess) raw.push_back(Num(b.close, 4));
    return down(raw);
  }
  std::unordered_map<int, double> by_sod;
  for (const auto& b : sess) {
    const auto w = UnixSecToShanghai(b.open_sec);
    const int sod = w.hour * 3600 + w.minute * 60;
    by_sod[sod] = b.close;
  }
  std::vector<std::string> aligned(slots.size(), "null");
  for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
    auto it = by_sod.find(slots[static_cast<size_t>(i)]);
    if (it == by_sod.end()) continue;
    aligned[static_cast<size_t>(i)] = Num(it->second, 4);
  }
  // Hold at most 2 empty minutes (thin tape). Longer holes stay null so the sparkline breaks.
  constexpr int kMaxHold = 2;
  const int nslot = static_cast<int>(aligned.size());
  for (int i = 0; i < nslot;) {
    if (aligned[static_cast<size_t>(i)] != "null") {
      ++i;
      continue;
    }
    int j = i;
    while (j < nslot && aligned[static_cast<size_t>(j)] == "null") ++j;
    if (i > 0 && (j - i) > 0 && (j - i) <= kMaxHold && j < nslot &&
        aligned[static_cast<size_t>(j)] != "null") {
      const std::string hold = aligned[static_cast<size_t>(i - 1)];
      for (int k = i; k < j; ++k) aligned[static_cast<size_t>(k)] = hold;
    }
    i = j;
  }
  // Keep the full session axis (night → next-day close). Trailing "null" = not yet traded;
  // quote.html draws those as empty, same as the main 分时 chart.
  return down(aligned);
}

std::string LatestYyyymmdd(const fs::path& root) {
  const auto days = ListDayDirs(root);
  if (days.empty()) return {};
  return days.back().filename().string();
}

// Night session belongs to the next CTP TradingDay. Dominant JSON can lag (still yesterday)
// while data/ticks/{MdTradingDay} already exists.
std::string ResolveTapeTradingDay(const std::string& data_root, const std::string& table_day,
                                  const std::string& override_day) {
  std::string day = override_day;
  if (day.empty() || day == "unknown") day = table_day;
  const std::string tick_day = LatestYyyymmdd(fs::path(data_root) / "ticks");
  if (!tick_day.empty() && (day.empty() || day == "unknown" || tick_day > day)) return tick_day;
  return day;
}

int64_t NowUnix() {
  return static_cast<int64_t>(std::time(nullptr));
}

bool WriteFile(const fs::path& path, const std::string& body) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << body;
  return static_cast<bool>(out);
}

struct ProductTape {
  std::string product_key;
  std::string instrument_id;
  std::string trading_day;
  std::vector<MinBar> klines;
  std::vector<MinBar> history;
  double last{0}, open{0}, high{0}, low{0}, volume{0}, prev_close{0};
  double bid{0}, ask{0}, bid_qty{0}, ask_qty{0};
  std::vector<std::string> sparkline;
  int64_t session_open_unix{0};
  int n_ep_m1{0}, n_ep_tick{0}, n_ctp{0};
};

std::vector<MinBar> CleanBars(std::vector<MinBar> in) {
  std::vector<MinBar> out;
  double ref = 0;
  out.reserve(in.size());
  for (auto b : in) {
    if (!(b.close > 0) || !std::isfinite(b.close) || b.close > 1.0e15) continue;
    if (ref > 0 && (b.close < ref * 0.5 || b.close > ref * 2.0)) continue;
    if (ref > 0) {
      if (b.low > 0 && b.low < ref * 0.5) b.low = std::min(b.open, b.close);
      if (b.high > ref * 2.0) b.high = std::max(b.open, b.close);
    }
    if (!(b.open > 0)) b.open = b.close;
    if (!(b.high > 0)) b.high = b.close;
    if (!(b.low > 0)) b.low = b.close;
    out.push_back(b);
    ref = b.close;
  }
  return out;
}

ProductTape BuildProduct(const std::string& data_root, const SessionCalendar* cal,
                         const DominantEntry& e, const std::string& trading_day) {
  ProductTape t;
  t.product_key = e.product_key;
  t.instrument_id = e.dominant;
  t.trading_day = trading_day;
  if (t.instrument_id.empty()) return t;

  const fs::path root(data_root);
  std::map<int64_t, MinBar> merged;

  for (const auto& day : ListDayDirs(root / "live" / "ep_raw")) {
    const fs::path m1 = day / "m1" / (t.instrument_id + ".csv");
    if (fs::is_regular_file(m1)) {
      auto bars = LoadM1Csv(m1, kPrioEpM1);
      t.n_ep_m1 += static_cast<int>(bars.size());
      MergeMap(merged, bars);
    }
    const fs::path tick = day / "tick" / (t.instrument_id + ".csv");
    if (fs::is_regular_file(tick)) {
      auto bars = LoadTickCsv(tick, kPrioEpTick, false, nullptr, nullptr, nullptr, nullptr, nullptr);
      t.n_ep_tick += static_cast<int>(bars.size());
      MergeMap(merged, bars);
    }
  }

  double live_px = 0, bid = 0, ask = 0, bq = 0, aq = 0;
  for (const auto& day : ListDayDirs(root / "ticks")) {
    const fs::path tick = day / (t.instrument_id + ".csv");
    if (!fs::is_regular_file(tick)) continue;
    auto bars = LoadTickCsv(tick, kPrioCtp, true, &live_px, &bid, &ask, &bq, &aq);
    t.n_ctp += static_cast<int>(bars.size());
    MergeMap(merged, bars);
  }
  t.bid = bid;
  t.ask = ask;
  t.bid_qty = bq;
  t.ask_qty = aq;

  SessionBound bounds{};
  if (cal) bounds = cal->DayBounds(t.product_key, trading_day);
  int64_t open_sec = bounds.ok ? bounds.open_unix_ns / 1000000000LL : 0;
  int64_t close_sec = bounds.ok ? bounds.close_unix_sec : 0;
  t.session_open_unix = open_sec;

  std::vector<MinBar> hist;
  hist.reserve(merged.size());
  for (const auto& kv : merged) {
    if (!cal || cal->Empty() || cal->IsInSession(t.product_key, kv.first * 1000000000LL)) {
      hist.push_back(kv.second);
    }
  }
  hist = CleanBars(std::move(hist));
  t.history = hist;

  for (const auto& b : hist) {
    if (open_sec > 0 && b.open_sec < open_sec) t.prev_close = b.close;
  }

  for (const auto& b : hist) {
    if (open_sec > 0 && b.open_sec < open_sec) continue;
    if (close_sec > 0 && b.open_sec > close_sec) continue;
    t.klines.push_back(b);
  }

  if (!t.klines.empty()) {
    t.open = t.klines.front().open;
    t.high = t.klines.front().high;
    t.low = t.klines.front().low;
    t.volume = 0;
    for (const auto& b : t.klines) {
      if (b.high > t.high) t.high = b.high;
      if (b.low < t.low) t.low = b.low;
      t.volume += b.volume;
    }
    t.last = t.klines.back().close;
    if (live_px > 0 && (t.last <= 0 || (live_px >= t.last * 0.5 && live_px <= t.last * 2.0))) {
      t.last = live_px;
    }
    if (!(t.prev_close > 0)) t.prev_close = t.open;
  } else if (live_px > 0) {
    t.last = live_px;
  }

  const auto* segs = cal ? cal->Find(t.product_key) : nullptr;
  t.sparkline = Sparkline(t.klines, SessionSlots(segs));
  return t;
}

std::string QuoteObject(const ProductTape& t) {
  const double chg = t.last - t.prev_close;
  const double pct = t.prev_close > 0 ? chg / t.prev_close * 100.0 : 0;
  std::ostringstream o;
  o << "{";
  o << "\"last\":" << Num(t.last, 4);
  o << ",\"open\":" << Num(t.open, 4);
  o << ",\"high\":" << Num(t.high, 4);
  o << ",\"low\":" << Num(t.low, 4);
  o << ",\"close\":" << Num(t.last, 4);
  o << ",\"volume\":" << Num(t.volume, 2);
  o << ",\"prev_close\":" << Num(t.prev_close, 4);
  o << ",\"change\":" << Num(chg, 4);
  o << ",\"change_pct\":" << Num(pct, 4);
  o << ",\"interval_min\":1";
  if (t.bid > 0) o << ",\"bid\":" << Num(t.bid, 4);
  if (t.ask > 0) o << ",\"ask\":" << Num(t.ask, 4);
  if (t.bid_qty > 0) o << ",\"bid_qty\":" << Num(t.bid_qty, 0);
  if (t.ask_qty > 0) o << ",\"ask_qty\":" << Num(t.ask_qty, 0);
  o << ",\"sparkline\":[";
  for (size_t i = 0; i < t.sparkline.size(); ++i) {
    if (i) o << ",";
    o << t.sparkline[i];
  }
  o << "]}";
  return o.str();
}

std::string ProductJson(const ProductTape& t) {
  std::ostringstream o;
  o << "{\n";
  o << "  \"symbol\":\"" << JsonEsc(t.product_key) << "\",\n";
  o << "  \"instrument_id\":\"" << JsonEsc(t.instrument_id) << "\",\n";
  o << "  \"trading_day\":\"" << JsonEsc(t.trading_day) << "\",\n";
  o << "  \"interval_min\":1,\n";
  o << "  \"source\":\"quote_tape\",\n";
  o << "  \"session_sliced\":true,\n";
  o << "  \"session_open_unix\":" << t.session_open_unix << ",\n";
  o << "  \"updated_unix\":" << NowUnix() << ",\n";
  o << "  \"quote\":" << QuoteObject(t) << ",\n";
  o << "  \"klines\":[";
  for (size_t i = 0; i < t.klines.size(); ++i) {
    const auto& b = t.klines[i];
    if (i) o << ",";
    const int64_t close_unix = b.open_sec + 60;
    o << "{\"datetime_str\":\"" << FmtWall(b.open_sec) << "\"";
    o << ",\"time_unix\":" << close_unix;
    o << ",\"time\":" << close_unix;
    o << ",\"open\":" << Num(b.open, 4);
    o << ",\"high\":" << Num(b.high, 4);
    o << ",\"low\":" << Num(b.low, 4);
    o << ",\"close\":" << Num(b.close, 4);
    o << ",\"volume\":" << Num(b.volume, 2);
    o << "}";
  }
  o << "]\n}\n";
  return o.str();
}

}  // namespace

QuoteTapeBuilder::QuoteTapeBuilder(std::string data_root, const SessionCalendar* calendar)
    : data_root_(std::move(data_root)), calendar_(calendar) {}

std::string QuoteTapeBuilder::DirFor(const std::string& data_root, const std::string& trading_day) {
  return (fs::path(data_root) / "live" / "quote" / trading_day).string();
}

std::size_t QuoteTapeBuilder::RebuildAll(const DominantTable& table, bool write_klines,
                                         const std::string& trading_day) {
  const std::string day = ResolveTapeTradingDay(data_root_, table.TradingDay(), trading_day);
  if (day.empty() || day == "unknown") {
    Logger::Error("quote_tape", "no trading_day (dominant JSON and data/ticks)");
    return 0;
  }
  const fs::path dir = fs::path(data_root_) / "live" / "quote" / day;
  std::error_code ec;
  fs::create_directories(dir, ec);

  std::ostringstream index;
  index << "{\n  \"trading_day\":\"" << JsonEsc(day) << "\",\n  \"source\":\"quote_tape\",\n"
        << "  \"updated_unix\":" << NowUnix() << ",\n  \"products\":{\n";
  bool first = true;
  std::size_t n = 0;
  std::size_t n_bars = 0;
  ParquetBarStore kstore(data_root_);
  for (const auto& e : table.Entries()) {
    if (e.dominant.empty()) continue;
    ProductTape t = BuildProduct(data_root_, calendar_, e, day);
    const fs::path out = dir / (e.product_key + ".json");
    if (!WriteFile(out, ProductJson(t))) {
      Logger::Error("quote_tape", "write failed " + out.string());
      continue;
    }
    if (write_klines) {
      for (const auto& b : t.history) {
        Bar bar;
        bar.product_key = t.product_key;
        bar.interval_min = 1;
        bar.datetime_ns = b.open_sec * 1000000000LL;
        bar.open = b.open;
        bar.high = b.high;
        bar.low = b.low;
        bar.close = b.close;
        bar.volume = b.volume;
        bar.time_unix = b.open_sec + 60;
        kstore.Append(bar);
      }
    }
    if (!first) index << ",\n";
    first = false;
    index << "    \"" << JsonEsc(e.product_key) << "\":{"
          << "\"instrument_id\":\"" << JsonEsc(t.instrument_id) << "\","
          << "\"n_bars\":" << t.klines.size() << ","
          << "\"quote\":" << QuoteObject(t) << "}";
    ++n;
    n_bars += t.klines.size();
  }
  index << "\n  }\n}\n";
  WriteFile(dir / "index.json", index.str());

  std::ostringstream latest;
  latest << "{\n  \"trading_day\":\"" << JsonEsc(day) << "\",\n  \"updated_unix\":" << NowUnix()
         << "\n}\n";
  WriteFile(fs::path(data_root_) / "live" / "quote" / "latest.json", latest.str());

  {
    static std::string last_key;
    static auto last_log = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto key = day + ":" + std::to_string(n) + ":" + std::to_string(n_bars);
    const auto now = std::chrono::steady_clock::now();
    if (key != last_key || now - last_log >= std::chrono::seconds(60)) {
      Logger::Info("quote_tape", "wrote day=" + day + " products=" + std::to_string(n) +
                                     " bars=" + std::to_string(n_bars) + " dir=" + dir.string());
      last_key = key;
      last_log = now;
    }
  }
  if (write_klines) {
    const auto nf = kstore.Flush(true);
    Logger::Info("quote_tape", "klines 1m replace files=" + std::to_string(nf) +
                                   " (dominant instrument only)");
  }
  return n;
}

int RunQuoteTape(const std::string& data_root, const std::string& dominant_file) {
  DominantTable table;
  try {
    if (!dominant_file.empty()) table = DominantTable::LoadFromFile(dominant_file);
    else table = DominantTable::LoadLatest(data_root);
  } catch (const std::exception& e) {
    Logger::Error("quote_tape", e.what());
    return 6;
  }
  SessionCalendar cal;
  const fs::path sess = fs::path(data_root) / "live" / "sessions" / "latest.json";
  if (fs::exists(sess)) cal = SessionCalendar::LoadFromFile(sess.string());
  QuoteTapeBuilder b(data_root, cal.Empty() ? nullptr : &cal);
  const auto n = b.RebuildAll(table, /*write_klines=*/false);
  return n > 0 ? 0 : 7;
}

}  // namespace qsev
