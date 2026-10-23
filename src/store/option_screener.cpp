#include "store/option_screener.h"

#include "logging/logger.h"
#include "md/md_gateway.h"
#include "md/subscription_manager.h"
#include "market/tick_bus.h"
#include "util/ctp_product_class.h"
#include "util/time_shanghai.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace qsev {
namespace {

namespace fs = std::filesystem;

constexpr double kEps = 1e-12;

std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else o += static_cast<char>(c);
  }
  return o;
}

std::string FmtNum(double x) {
  if (!std::isfinite(x)) return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", x);
  return buf;
}

std::string Yyyymmdd(const CivilDate& d) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d", d.year, d.month, d.day);
  return buf;
}

CivilDate ShanghaiToday() {
  return UnixSecToShanghai(static_cast<int64_t>(std::time(nullptr))).date;
}

int WeekdaySun0(const CivilDate& d) {
  const int64_t unix = ShanghaiWallToUnixSec(d, 12 * 3600);
  const int64_t days = unix / 86400;
  int w = static_cast<int>((days + 4) % 7);
  if (w < 0) w += 7;
  return w;  // 0=Sun
}

bool LooksLikeContractId(const std::string& id) {
  for (unsigned char c : id) {
    if (std::isdigit(c)) return true;
  }
  return false;
}

char OptClass(const InstrumentInfo& e) {
  return e.product_class;
}

bool IsCall(char options_type) { return options_type == '1'; }
bool IsPut(char options_type) { return options_type == '2'; }

double MidOrLast(const OptionQuote& q) {
  if (q.has_book && q.bid > 0 && q.ask > 0 && q.ask >= q.bid) return 0.5 * (q.bid + q.ask);
  if (q.has_last && IsValidLastPrice(q.last)) return q.last;
  return 0;
}

void ApplyQuote(OptionQuote* dst, const OptionQuote& src, bool overwrite_source) {
  if (!dst) return;
  if (src.has_last) {
    dst->last = src.last;
    dst->has_last = true;
  }
  if (src.has_book) {
    dst->bid = src.bid;
    dst->ask = src.ask;
    dst->has_book = true;
  }
  if (src.open_interest > 0) dst->open_interest = src.open_interest;
  if (src.volume > 0) dst->volume = src.volume;
  if (overwrite_source || dst->source.empty()) dst->source = src.source;
}

std::vector<std::string> SplitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ',') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

bool ParseLastTickLine(const std::string& line, OptionQuote* q) {
  if (!q || line.empty() || line[0] == 'd' || static_cast<unsigned char>(line[0]) == 0xEF) {
    return false;
  }
  const auto cols = SplitCsv(line);
  // TickDayStore: last_price=7 volume=8 oi=10 bid1=12 ask1=22
  if (cols.size() < 23) return false;
  try {
    const double last = std::stod(cols[7]);
    const double vol = std::stod(cols[8]);
    const double oi = std::stod(cols[10]);
    const double bid = std::stod(cols[12]);
    const double ask = std::stod(cols[22]);
    if (IsValidLastPrice(last)) {
      q->last = last;
      q->has_last = true;
    }
    q->volume = vol;
    q->open_interest = oi;
    if (IsValidLastPrice(bid) && IsValidLastPrice(ask)) {
      q->bid = bid;
      q->ask = ask;
      q->has_book = true;
    }
    q->source = "tick";
    return q->has_last || q->has_book;
  } catch (...) {
    return false;
  }
}

bool ReadLastTickQuote(const fs::path& path, OptionQuote* q) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const auto sz = in.tellg();
  if (sz <= 0) return false;
  const auto back = std::min<std::streamoff>(static_cast<std::streamoff>(sz), 16384);
  in.seekg(-back, std::ios::end);
  std::string buf(static_cast<size_t>(back), '\0');
  in.read(buf.data(), back);
  const auto n = static_cast<size_t>(in.gcount());
  buf.resize(n);
  size_t end = buf.size();
  while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r')) --end;
  if (end == 0) return false;
  const auto nl = buf.find_last_of('\n', end - 1);
  const std::string line = (nl == std::string::npos) ? buf.substr(0, end) : buf.substr(nl + 1, end - nl - 1);
  return ParseLastTickLine(line, q);
}

std::string SafeFileStem(const std::string& id) {
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

std::string CpName(char options_type) {
  if (IsCall(options_type)) return "Call";
  if (IsPut(options_type)) return "Put";
  return "?";
}

void ScoreRow(OptionScreenRow* row, const OptionQuote& oq, const OptionQuote& uq,
              const OptionScreenConfig& cfg) {
  if (uq.has_last && IsValidLastPrice(uq.last)) row->spot = uq.last;
  row->oi = oq.open_interest;
  row->volume = oq.volume;
  row->bid = oq.bid;
  row->ask = oq.ask;
  row->quote_source = oq.source;
  if (oq.has_last || oq.has_book) {
    row->opt_px = MidOrLast(oq);
  }
  if (row->price_tick > 0 && oq.has_book && oq.ask > 0 && oq.bid > 0 && oq.ask >= oq.bid) {
    row->spread_ticks = (oq.ask - oq.bid) / row->price_tick;
  }

  if (row->spot > 0 && row->strike > 0) {
    row->intrinsic = IntrinsicValue(row->options_type, row->spot, row->strike);
  }
  if (row->opt_px > 0) {
    row->market_tv = std::max(row->opt_px - row->intrinsic, 0.0);
  }

  const double t_years = static_cast<double>(std::max(row->dte, 0)) / 365.0;
  if (row->spot > 0 && row->strike > 0 && t_years > 0 && cfg.hv > 0) {
    row->bs_price = Black76Price(IsCall(row->options_type), row->spot, row->strike, t_years, cfg.hv,
                                 cfg.rate);
    row->theo_tv = std::max(row->bs_price - row->intrinsic, 0.0);
  }
  if (row->theo_tv > kEps) {
    row->ratio = row->market_tv / row->theo_tv;
  }
  row->diff = row->theo_tv - row->market_tv;

  bool spread_ok = true;
  if (cfg.max_spread_ticks > 0) {
    spread_ok = oq.has_book && row->spread_ticks >= 0 &&
                row->spread_ticks <= static_cast<double>(cfg.max_spread_ticks);
  }
  const bool oi_ok = row->oi + kEps >= cfg.min_oi;
  const bool vol_ok = row->volume + kEps >= cfg.min_volume;
  row->liquid = spread_ok && oi_ok && vol_ok;

  const bool theo_ok = row->theo_tv > cfg.min_theo_tv;
  const bool ratio_ok = row->theo_tv > kEps && row->ratio >= 0 && row->ratio < cfg.cheap_ratio;
  const bool gap_ok = cfg.min_gap > 0 && row->diff > cfg.min_gap;
  const bool cheap_rule = theo_ok && (ratio_ok || gap_ok);
  row->cheap = cheap_rule && row->liquid && row->opt_px > 0 && row->spot > 0;

  if (!row->liquid) {
    if (!spread_ok) row->reject_reason = "spread";
    else if (!oi_ok) row->reject_reason = "oi";
    else if (!vol_ok) row->reject_reason = "volume";
  } else if (cheap_rule && !row->cheap) {
    row->reject_reason = "no_quote";
  } else if (!theo_ok) {
    row->reject_reason = "theo_tv";
  } else if (!ratio_ok && !gap_ok) {
    row->reject_reason = "not_cheap";
  }

  std::ostringstream ex;
  ex << CpName(row->options_type) << " DTE=" << row->dte << " expire=" << row->expire_date
     << " K=" << FmtNum(row->strike) << " S=" << FmtNum(row->spot) << " px=" << FmtNum(row->opt_px)
     << " Intrinsic=" << FmtNum(row->intrinsic) << " MarketTV=" << FmtNum(row->market_tv)
     << " TheoTV=" << FmtNum(row->theo_tv) << " Ratio=" << FmtNum(row->ratio)
     << " Diff=" << FmtNum(row->diff);
  if (row->cheap) ex << " cheap";
  if (!row->reject_reason.empty()) ex << " reject=" << row->reject_reason;
  row->explain = ex.str();
}

std::vector<std::string> BuildSubscribe(const OptionScreenResult& r, int cap) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto add = [&](const std::string& id) {
    if (id.empty() || seen.count(id)) return;
    if (cap > 0 && static_cast<int>(out.size()) >= cap) return;
    seen.insert(id);
    out.push_back(id);
  };
  for (const auto& row : r.near) add(row.underlying_id);
  for (const auto& row : r.cheap) add(row.instrument_id);
  std::vector<OptionScreenRow> rest = r.near;
  std::sort(rest.begin(), rest.end(), [](const OptionScreenRow& a, const OptionScreenRow& b) {
    if (a.dte != b.dte) return a.dte < b.dte;
    if (a.oi != b.oi) return a.oi > b.oi;
    return a.instrument_id < b.instrument_id;
  });
  for (const auto& row : rest) add(row.instrument_id);
  return out;
}

void WriteRowJson(std::ostream& out, const OptionScreenRow& r) {
  out << "{";
  out << "\"instrument_id\":\"" << JsonEscape(r.instrument_id) << "\",";
  out << "\"exchange_id\":\"" << JsonEscape(r.exchange_id) << "\",";
  out << "\"product_id\":\"" << JsonEscape(r.product_id) << "\",";
  out << "\"underlying_id\":\"" << JsonEscape(r.underlying_id) << "\",";
  out << "\"expire_date\":\"" << JsonEscape(r.expire_date) << "\",";
  out << "\"options_type\":\"" << (r.options_type ? std::string(1, r.options_type) : "") << "\",";
  out << "\"cp\":\"" << CpName(r.options_type) << "\",";
  out << "\"dte\":" << r.dte << ",";
  out << "\"strike\":" << FmtNum(r.strike) << ",";
  out << "\"spot\":" << FmtNum(r.spot) << ",";
  out << "\"opt_px\":" << FmtNum(r.opt_px) << ",";
  out << "\"bid\":" << FmtNum(r.bid) << ",";
  out << "\"ask\":" << FmtNum(r.ask) << ",";
  out << "\"spread_ticks\":" << FmtNum(r.spread_ticks) << ",";
  out << "\"oi\":" << FmtNum(r.oi) << ",";
  out << "\"volume\":" << FmtNum(r.volume) << ",";
  out << "\"intrinsic\":" << FmtNum(r.intrinsic) << ",";
  out << "\"market_tv\":" << FmtNum(r.market_tv) << ",";
  out << "\"bs_price\":" << FmtNum(r.bs_price) << ",";
  out << "\"theo_tv\":" << FmtNum(r.theo_tv) << ",";
  out << "\"ratio\":" << FmtNum(r.ratio) << ",";
  out << "\"diff\":" << FmtNum(r.diff) << ",";
  out << "\"cheap\":" << (r.cheap ? "true" : "false") << ",";
  out << "\"liquid\":" << (r.liquid ? "true" : "false") << ",";
  out << "\"quote_source\":\"" << JsonEscape(r.quote_source) << "\",";
  out << "\"reject_reason\":\"" << JsonEscape(r.reject_reason) << "\",";
  out << "\"explain\":\"" << JsonEscape(r.explain) << "\"";
  out << "}";
}

bool WaitMd(MdGateway& md, int timeout_ms) {
  const int steps = std::max(1, timeout_ms / 100);
  for (int i = 0; i < steps && !md.LoggedIn(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return md.LoggedIn();
}

void ApplyCliToConfig(const OptionScreenCliOptions& opt, OptionScreenConfig* cfg) {
  if (opt.dte_min >= 0) cfg->dte_min = opt.dte_min;
  if (opt.dte_max >= 0) cfg->dte_max = opt.dte_max;
  if (!opt.dte_mode.empty()) cfg->dte_mode = opt.dte_mode;
  if (!opt.as_of.empty()) cfg->as_of = opt.as_of;
  cfg->include_spot_option = opt.include_spot;
  cfg->include_second = opt.include_second;
  if (opt.hv >= 0) cfg->hv = opt.hv;
  if (opt.rate >= 0) cfg->rate = opt.rate;
  if (opt.cheap_ratio >= 0) cfg->cheap_ratio = opt.cheap_ratio;
  if (opt.min_theo_tv >= 0) cfg->min_theo_tv = opt.min_theo_tv;
  if (opt.min_gap >= 0) cfg->min_gap = opt.min_gap;
  if (opt.max_spread_ticks >= 0) cfg->max_spread_ticks = opt.max_spread_ticks;
  if (opt.min_oi >= 0) cfg->min_oi = opt.min_oi;
  if (opt.min_volume >= 0) cfg->min_volume = opt.min_volume;
  if (opt.max_subscribe >= 0) cfg->max_subscribe = opt.max_subscribe;
}

}  // namespace

int CalendarDaysBetween(const CivilDate& from, const CivilDate& to) {
  const int64_t a = ShanghaiWallToUnixSec(from, 12 * 3600);
  const int64_t b = ShanghaiWallToUnixSec(to, 12 * 3600);
  return static_cast<int>((b - a) / 86400);
}

int WeekdayTradingDaysBetween(const CivilDate& from, const CivilDate& to) {
  const int cal = CalendarDaysBetween(from, to);
  if (cal <= 0) return cal;
  int n = 0;
  CivilDate d = AddDays(from, 1);
  for (int i = 0; i < cal; ++i) {
    const int w = WeekdaySun0(d);
    if (w >= 1 && w <= 5) ++n;
    d = AddDays(d, 1);
  }
  return n;
}

double IntrinsicValue(char options_type, double spot, double strike) {
  if (!(spot > 0) || !(strike > 0)) return 0;
  if (IsCall(options_type)) return std::max(spot - strike, 0.0);
  if (IsPut(options_type)) return std::max(strike - spot, 0.0);
  return 0;
}

double NormCdf(double x) {
  const double a1 = 0.254829592;
  const double a2 = -0.284496736;
  const double a3 = 1.421413741;
  const double a4 = -1.453152027;
  const double a5 = 1.061405429;
  const double p = 0.3275911;
  const int sign = (x < 0) ? -1 : 1;
  x = std::fabs(x) / std::sqrt(2.0);
  const double t = 1.0 / (1.0 + p * x);
  const double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * std::exp(-x * x);
  return 0.5 * (1.0 + static_cast<double>(sign) * y);
}

double Black76Price(bool is_call, double forward, double strike, double t_years, double sigma,
                    double rate) {
  if (!(forward > 0) || !(strike > 0) || !(t_years > 0) || !(sigma > 0)) return 0;
  const double sqrt_t = std::sqrt(t_years);
  const double d1 = (std::log(forward / strike) + 0.5 * sigma * sigma * t_years) / (sigma * sqrt_t);
  const double d2 = d1 - sigma * sqrt_t;
  const double df = std::exp(-rate * t_years);
  if (is_call) {
    return df * (forward * NormCdf(d1) - strike * NormCdf(d2));
  }
  return df * (strike * NormCdf(-d2) - forward * NormCdf(-d1));
}

std::unordered_set<std::string> DominantUnderlyingIds(const DominantTable& table,
                                                      bool include_second) {
  std::unordered_set<std::string> out;
  for (const auto& e : table.Entries()) {
    if (!e.dominant.empty()) out.insert(e.dominant);
    if (include_second && !e.second.empty()) out.insert(e.second);
  }
  return out;
}

bool UnderlyingAllowed(const std::string& underlying_instr_id,
                       const std::unordered_set<std::string>& allowed_ids) {
  if (underlying_instr_id.empty() || allowed_ids.empty()) return false;
  if (allowed_ids.count(underlying_instr_id)) return true;
  // Product-only UnderlyingInstrID (no digits), e.g. "si" / "SA".
  if (!LooksLikeContractId(underlying_instr_id)) {
    for (const auto& id : allowed_ids) {
      std::string prod = id;
      while (!prod.empty() && std::isdigit(static_cast<unsigned char>(prod.back()))) prod.pop_back();
      if (prod == underlying_instr_id) return true;
    }
  }
  return false;
}

void MergeQuotesFromPreSettlement(const InstrumentStore& store,
                                  std::unordered_map<std::string, OptionQuote>* quotes) {
  if (!quotes) return;
  for (const auto& e : store.List()) {
    if (!IsValidLastPrice(e.pre_settlement_price)) continue;
    OptionQuote q;
    q.last = e.pre_settlement_price;
    q.has_last = true;
    q.source = "pre_settlement";
    ApplyQuote(&(*quotes)[e.instrument_id], q, /*overwrite_source=*/false);
  }
}

void MergeQuotesFromBook(const TickSnapshotBook& book,
                         std::unordered_map<std::string, OptionQuote>* quotes) {
  if (!quotes) return;
  for (auto& kv : *quotes) {
    DepthTick t;
    if (!book.TryGetLatest(kv.first, &t)) continue;
    OptionQuote q;
    if (IsValidLastPrice(t.last_price)) {
      q.last = t.last_price;
      q.has_last = true;
    }
    if (IsValidLastPrice(t.bid_price[0]) && IsValidLastPrice(t.ask_price[0])) {
      q.bid = t.bid_price[0];
      q.ask = t.ask_price[0];
      q.has_book = true;
    }
    q.open_interest = t.open_interest;
    q.volume = static_cast<double>(t.volume);
    q.source = "snapshot";
    ApplyQuote(&kv.second, q, /*overwrite_source=*/true);
  }
}

void MergeQuotesFromTickFiles(const std::string& data_root, const std::string& day,
                              const std::vector<std::string>& ids,
                              std::unordered_map<std::string, OptionQuote>* quotes) {
  if (!quotes || day.empty()) return;
  const fs::path dir = fs::path(data_root) / "ticks" / day;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  auto try_id = [&](const std::string& id) {
    const fs::path p = dir / (SafeFileStem(id) + ".csv");
    OptionQuote q;
    if (!ReadLastTickQuote(p, &q)) return;
    ApplyQuote(&(*quotes)[id], q, /*overwrite_source=*/true);
  };
  if (!ids.empty()) {
    for (const auto& id : ids) try_id(id);
    return;
  }
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (!ent.is_regular_file() || ent.path().extension() != ".csv") continue;
    OptionQuote q;
    if (!ReadLastTickQuote(ent.path(), &q)) continue;
    ApplyQuote(&(*quotes)[ent.path().stem().string()], q, /*overwrite_source=*/true);
  }
}

OptionScreenResult ScreenOptions(const InstrumentStore& store, const DominantTable* dominant,
                                 const std::unordered_map<std::string, OptionQuote>& quotes,
                                 const OptionScreenConfig& cfg) {
  OptionScreenResult r;
  r.cfg = cfg;
  r.orders_placed = false;
  r.trading_day = store.TradingDay();

  CivilDate as_of = ShanghaiToday();
  if (!cfg.as_of.empty()) {
    CivilDate parsed{};
    if (ParseYyyymmdd(cfg.as_of, &parsed)) as_of = parsed;
  }
  r.as_of = Yyyymmdd(as_of);

  std::unordered_set<std::string> allowed;
  if (cfg.restrict_underlying_dominant) {
    if (dominant) allowed = DominantUnderlyingIds(*dominant, cfg.include_second);
    if (r.trading_day.empty() && dominant) r.trading_day = dominant->TradingDay();
  }

  const bool trading_dte = (cfg.dte_mode == "trading");

  for (const auto& e : store.List()) {
    if (!IsOptionClass(OptClass(e))) continue;
    if (!cfg.include_spot_option && e.product_class == kPcSpotOption) continue;
    ++r.options_seen;
    if (cfg.require_trading && !e.is_trading) continue;
    if (e.expire_date.empty() || e.strike_price <= 0) continue;
    if (!IsCall(e.options_type) && !IsPut(e.options_type)) continue;

    CivilDate exp{};
    if (!ParseYyyymmdd(e.expire_date, &exp)) continue;
    const int dte = trading_dte ? WeekdayTradingDaysBetween(as_of, exp)
                                : CalendarDaysBetween(as_of, exp);
    if (dte < cfg.dte_min || dte > cfg.dte_max) continue;
    ++r.dte_kept;

    if (cfg.restrict_underlying_dominant) {
      if (!UnderlyingAllowed(e.underlying_instr_id, allowed)) continue;
    }
    ++r.underlying_kept;

    OptionScreenRow row;
    row.instrument_id = e.instrument_id;
    row.exchange_id = e.exchange_id;
    row.product_id = e.product_id;
    row.underlying_id = e.underlying_instr_id;
    row.expire_date = e.expire_date;
    row.options_type = e.options_type;
    row.dte = dte;
    row.strike = e.strike_price;
    row.price_tick = e.price_tick;

    OptionQuote oq;
    OptionQuote uq;
    if (auto it = quotes.find(e.instrument_id); it != quotes.end()) oq = it->second;
    if (auto it = quotes.find(e.underlying_instr_id); it != quotes.end()) uq = it->second;
    ScoreRow(&row, oq, uq, cfg);
    r.near.push_back(std::move(row));
  }

  std::sort(r.near.begin(), r.near.end(), [](const OptionScreenRow& a, const OptionScreenRow& b) {
    if (a.cheap != b.cheap) return a.cheap && !b.cheap;
    if (a.dte != b.dte) return a.dte < b.dte;
    if (a.diff != b.diff) return a.diff > b.diff;
    return a.instrument_id < b.instrument_id;
  });
  for (const auto& row : r.near) {
    if (row.cheap) r.cheap.push_back(row);
  }
  r.subscribe = BuildSubscribe(r, cfg.max_subscribe);
  return r;
}

bool WriteOptionScreen(const std::string& json_path, const std::string& csv_path,
                       const OptionScreenResult& result) {
  try {
    fs::create_directories(fs::path(json_path).parent_path());
    if (!csv_path.empty()) fs::create_directories(fs::path(csv_path).parent_path());
  } catch (const std::exception& e) {
    Logger::Error("option_screen", std::string("mkdir: ") + e.what());
    return false;
  }

  {
    std::ofstream out(json_path, std::ios::binary);
    if (!out) {
      Logger::Error("option_screen", "cannot write " + json_path);
      return false;
    }
    const auto& c = result.cfg;
    out << "{\n";
    out << "  \"as_of\": \"" << JsonEscape(result.as_of) << "\",\n";
    out << "  \"trading_day\": \"" << JsonEscape(result.trading_day) << "\",\n";
    out << "  \"orders_placed\": false,\n";
    out << "  \"enable_trading\": false,\n";
    out << "  \"dte_mode\": \"" << JsonEscape(c.dte_mode) << "\",\n";
    out << "  \"dte_min\": " << c.dte_min << ",\n";
    out << "  \"dte_max\": " << c.dte_max << ",\n";
    out << "  \"hv\": " << FmtNum(c.hv) << ",\n";
    out << "  \"rate\": " << FmtNum(c.rate) << ",\n";
    out << "  \"cheap_ratio\": " << FmtNum(c.cheap_ratio) << ",\n";
    out << "  \"min_theo_tv\": " << FmtNum(c.min_theo_tv) << ",\n";
    out << "  \"min_gap\": " << FmtNum(c.min_gap) << ",\n";
    out << "  \"max_spread_ticks\": " << c.max_spread_ticks << ",\n";
    out << "  \"options_seen\": " << result.options_seen << ",\n";
    out << "  \"dte_kept\": " << result.dte_kept << ",\n";
    out << "  \"underlying_kept\": " << result.underlying_kept << ",\n";
    out << "  \"near_n\": " << result.near.size() << ",\n";
    out << "  \"cheap_n\": " << result.cheap.size() << ",\n";
    out << "  \"subscribe\": [";
    for (size_t i = 0; i < result.subscribe.size(); ++i) {
      if (i) out << ",";
      out << "\"" << JsonEscape(result.subscribe[i]) << "\"";
    }
    out << "],\n";
    out << "  \"cheap\": [\n";
    for (size_t i = 0; i < result.cheap.size(); ++i) {
      out << "    ";
      WriteRowJson(out, result.cheap[i]);
      if (i + 1 < result.cheap.size()) out << ",";
      out << "\n";
    }
    out << "  ],\n";
    out << "  \"near\": [\n";
    for (size_t i = 0; i < result.near.size(); ++i) {
      out << "    ";
      WriteRowJson(out, result.near[i]);
      if (i + 1 < result.near.size()) out << ",";
      out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
  }

  if (!csv_path.empty()) {
    std::ofstream out(csv_path, std::ios::binary);
    if (!out) {
      Logger::Error("option_screen", "cannot write " + csv_path);
      return false;
    }
    out << "\xEF\xBB\xBF";
    out << "instrument_id,exchange_id,product_id,underlying_id,expire_date,cp,dte,strike,spot,opt_px,"
           "bid,ask,spread_ticks,oi,volume,intrinsic,market_tv,bs_price,theo_tv,ratio,diff,cheap,"
           "liquid,quote_source,reject_reason,explain\n";
    auto write_csv_row = [&](const OptionScreenRow& r) {
      out << r.instrument_id << "," << r.exchange_id << "," << r.product_id << "," << r.underlying_id
          << "," << r.expire_date << "," << CpName(r.options_type) << "," << r.dte << ","
          << FmtNum(r.strike) << "," << FmtNum(r.spot) << "," << FmtNum(r.opt_px) << ","
          << FmtNum(r.bid) << "," << FmtNum(r.ask) << "," << FmtNum(r.spread_ticks) << ","
          << FmtNum(r.oi) << "," << FmtNum(r.volume) << "," << FmtNum(r.intrinsic) << ","
          << FmtNum(r.market_tv) << "," << FmtNum(r.bs_price) << "," << FmtNum(r.theo_tv) << ","
          << FmtNum(r.ratio) << "," << FmtNum(r.diff) << "," << (r.cheap ? "1" : "0") << ","
          << (r.liquid ? "1" : "0") << "," << r.quote_source << "," << r.reject_reason << ",\""
          << JsonEscape(r.explain) << "\"\n";
    };
    for (const auto& row : result.near) write_csv_row(row);
  }

  Logger::Info("option_screen", "wrote " + json_path + " near=" + std::to_string(result.near.size()) +
                                    " cheap=" + std::to_string(result.cheap.size()) +
                                    " subscribe=" + std::to_string(result.subscribe.size()));
  return true;
}

std::string FindLatestFullInstruments(const std::string& data_root) {
  const fs::path dir = fs::path(data_root) / "live" / "instruments";
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
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
  if (!best_path.empty()) return best_path.string();
  const fs::path latest = dir / "latest.json";
  if (fs::exists(latest, ec)) return latest.string();
  return {};
}

int RunOptionScreenSelfTest() {
  int fail = 0;
  auto check = [&](bool ok, const char* name) {
    if (!ok) {
      std::cerr << "option_screen selftest FAIL " << name << "\n";
      ++fail;
    }
  };

  CivilDate a{2026, 8, 24};
  CivilDate exp1{2026, 8, 25};
  CivilDate exp0{2026, 8, 24};
  CivilDate exp30{2026, 9, 23};
  CivilDate exp29{2026, 9, 22};
  check(CalendarDaysBetween(a, exp1) == 1, "dte_1");
  check(CalendarDaysBetween(a, exp0) == 0, "dte_0");
  check(CalendarDaysBetween(a, exp30) == 30, "dte_30");
  check(CalendarDaysBetween(a, exp29) == 29, "dte_29");
  check(std::fabs(IntrinsicValue('1', 110, 100) - 10) < 1e-9, "call_itm");
  check(std::fabs(IntrinsicValue('2', 90, 100) - 10) < 1e-9, "put_itm");
  check(IntrinsicValue('1', 90, 100) == 0, "call_otm");

  const double atm = Black76Price(true, 100, 100, 10.0 / 365.0, 0.30, 0.02);
  check(atm > 0.5 && atm < 8.0, "bs_atm_range");
  const double itm_c = Black76Price(true, 110, 100, 10.0 / 365.0, 0.30, 0.02);
  check(itm_c > atm, "bs_call_up");
  const double put_atm = Black76Price(false, 100, 100, 10.0 / 365.0, 0.30, 0.02);
  check(std::fabs(atm - put_atm) < 0.05, "bs_atm_parity");

  InstrumentStore store;
  store.SetTradingDay("20260824");
  auto mk_opt = [](const char* id, const char* und, const char* exp, char cp, double k,
                   bool trading) {
    InstrumentInfo e;
    e.exchange_id = "SHFE";
    e.instrument_id = id;
    e.product_id = "ao_o";
    e.product_class = kPcOptions;
    e.options_type = cp;
    e.is_trading = trading;
    e.expire_date = exp;
    e.strike_price = k;
    e.underlying_instr_id = und;
    e.price_tick = 1;
    e.volume_multiple = 10;
    return e;
  };
  store.Upsert(mk_opt("ao2610C3000", "ao2610", "20260825", '1', 3000, true));  // DTE=1 keep
  store.Upsert(mk_opt("ao2610P3000", "ao2610", "20260824", '2', 3000, true));  // DTE=0 drop
  store.Upsert(mk_opt("ao2610C3100", "ao2610", "20260923", '1', 3100, true));  // DTE=30 drop
  store.Upsert(mk_opt("ao2611C3000", "ao2611", "20260825", '1', 3000, true));  // wrong und
  store.Upsert(mk_opt("ao2610C2900", "ao2610", "20260922", '1', 2900, true));  // DTE=29 keep

  const fs::path tmp = fs::temp_directory_path() / "qsev_opt_dom.json";
  {
    std::ofstream o(tmp);
    o << "{\"trading_day\":\"20260824\",\"products\":{\"SHFE.ao\":{"
         "\"dominant\":\"ao2610\",\"second\":\"ao2612\",\"dominant_oi\":1,\"second_oi\":1}}}\n";
  }
  DominantTable table;
  try {
    table = DominantTable::LoadFromFile(tmp.string());
  } catch (...) {
    check(false, "dominant_load");
    return 11;
  }

  std::unordered_map<std::string, OptionQuote> quotes;
  OptionQuote s;
  s.last = 3000;
  s.has_last = true;
  s.source = "pre_settlement";
  quotes["ao2610"] = s;
  OptionQuote cheap_px;
  cheap_px.last = 8;
  cheap_px.has_last = true;
  cheap_px.bid = 7;
  cheap_px.ask = 9;
  cheap_px.has_book = true;
  cheap_px.open_interest = 100;
  cheap_px.source = "snapshot";
  quotes["ao2610C3000"] = cheap_px;
  quotes["ao2610C2900"] = cheap_px;

  OptionScreenConfig cfg;
  cfg.as_of = "20260824";
  cfg.max_spread_ticks = 8;
  cfg.min_theo_tv = 1;
  cfg.cheap_ratio = 0.5;
  cfg.hv = 0.30;
  cfg.include_second = false;

  check(UnderlyingAllowed("ao2610", DominantUnderlyingIds(table, false)), "und_dom");
  check(!UnderlyingAllowed("ao2611", DominantUnderlyingIds(table, false)), "und_other");
  check(UnderlyingAllowed("ao", DominantUnderlyingIds(table, false)), "und_product");

  auto res = ScreenOptions(store, &table, quotes, cfg);
  check(res.orders_placed == false, "no_orders");
  check(res.near.size() == 2, "near_n");
  bool has_dte1 = false, has_dte29 = false, has_bad = false;
  for (const auto& row : res.near) {
    if (row.instrument_id == "ao2610C3000" && row.dte == 1 && row.expire_date == "20260825") {
      has_dte1 = true;
    }
    if (row.instrument_id == "ao2610C2900" && row.dte == 29) has_dte29 = true;
    if (row.instrument_id == "ao2610P3000" || row.instrument_id == "ao2610C3100" ||
        row.instrument_id == "ao2611C3000") {
      has_bad = true;
    }
  }
  check(has_dte1 && has_dte29, "dte_keep");
  check(!has_bad, "dte_drop");
  check(!res.cheap.empty(), "cheap_hit");
  if (!res.cheap.empty()) {
    check(res.cheap[0].explain.find("TheoTV") != std::string::npos, "explain");
    check(res.cheap[0].ratio >= 0 && res.cheap[0].ratio < 0.5, "cheap_ratio");
  }
  check(!res.subscribe.empty() && res.subscribe.size() <= 200, "subscribe_cap");

  const fs::path j = fs::temp_directory_path() / "qsev_opt.json";
  const fs::path c = fs::temp_directory_path() / "qsev_opt.csv";
  check(WriteOptionScreen(j.string(), c.string(), res), "write");

  std::error_code ec;
  fs::remove(tmp, ec);
  fs::remove(j, ec);
  fs::remove(c, ec);

  if (fail == 0) {
    std::cout << "{\"ok\":true,\"selftest\":true,\"module\":\"option_screen\"}\n";
    Logger::Info("option_screen", "selftest ok");
  }
  return fail == 0 ? 0 : 11;
}

int RunOptionScreen(const OptionScreenCliOptions& opt, const std::string& data_root,
                    const AppConfig* cfg_for_live) {
  if (opt.selftest) return RunOptionScreenSelfTest();

  OptionScreenConfig cfg;
  ApplyCliToConfig(opt, &cfg);

  std::string inst_path = opt.instruments_path;
  if (inst_path.empty()) inst_path = FindLatestFullInstruments(data_root);
  if (inst_path.empty()) {
    Logger::Error("option_screen", "no full-market instruments JSON under data/live/instruments/");
    return 2;
  }

  InstrumentStore store;
  if (!store.LoadJson(inst_path)) {
    Logger::Error("option_screen", "load instruments failed: " + inst_path);
    return 2;
  }
  Logger::Info("option_screen", "instruments " + inst_path + " n=" + std::to_string(store.Size()));

  DominantTable table;
  const DominantTable* table_ptr = nullptr;
  if (cfg.restrict_underlying_dominant) {
    try {
      if (!opt.dominant_path.empty()) {
        table = DominantTable::LoadFromFile(opt.dominant_path);
      } else {
        table = DominantTable::LoadLatest(data_root);
      }
      table_ptr = &table;
    } catch (const std::exception& e) {
      Logger::Error("option_screen", e.what());
      return 6;
    }
  }

  std::unordered_map<std::string, OptionQuote> quotes;
  MergeQuotesFromPreSettlement(store, &quotes);
  const std::string tick_day = !cfg.as_of.empty() ? cfg.as_of
                               : !store.TradingDay().empty() ? store.TradingDay()
                                                            : table.TradingDay();
  MergeQuotesFromTickFiles(data_root, tick_day, {}, &quotes);

  auto result = ScreenOptions(store, table_ptr, quotes, cfg);
  Logger::Info("option_screen",
               "as_of=" + result.as_of + " seen=" + std::to_string(result.options_seen) +
                   " dte=" + std::to_string(result.dte_kept) +
                   " near=" + std::to_string(result.near.size()) +
                   " cheap=" + std::to_string(result.cheap.size()) +
                   " subscribe=" + std::to_string(result.subscribe.size()) +
                   " orders_placed=false");

  if (opt.live) {
    if (!cfg_for_live) {
      Logger::Error("option_screen", "--option-screen-live needs --config");
      return 2;
    }
    if (result.subscribe.empty()) {
      Logger::Warn("option_screen", "live subscribe list empty; writing file-only screen");
    } else {
      Logger::Info("option_screen", "live subscribe n=" + std::to_string(result.subscribe.size()) +
                                        " seconds=" + std::to_string(std::max(1, opt.live_seconds)));
      TickBus bus;
      TickSnapshotBook book(256);
      MdGateway md(*cfg_for_live, bus);
      if (!md.Start()) return 4;
      if (!WaitMd(md, 5000)) {
        Logger::Error("option_screen", "Md login timeout");
        md.Stop();
        return 5;
      }
      SubscriptionManager subs(md);
      if (!subs.Sync(result.subscribe)) {
        Logger::Warn("option_screen", "subscribe Sync reported failures");
      }
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, opt.live_seconds));
      size_t n_ticks = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        while (auto t = bus.TryPop()) {
          ++n_ticks;
          book.Update(*t);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      Logger::Info("option_screen", "live ticks=" + std::to_string(n_ticks) +
                                        " book=" + std::to_string(book.Size()));
      for (const auto& id : result.subscribe) quotes[id];
      for (const auto& row : result.near) {
        quotes[row.instrument_id];
        quotes[row.underlying_id];
      }
      MergeQuotesFromBook(book, &quotes);
      result = ScreenOptions(store, table_ptr, quotes, cfg);
      md.Stop();
      Logger::Info("option_screen",
                   "after live near=" + std::to_string(result.near.size()) +
                       " cheap=" + std::to_string(result.cheap.size()) + " orders_placed=false");
    }
  }

  const size_t alert_n = std::min<size_t>(result.cheap.size(), 20);
  for (size_t i = 0; i < alert_n; ++i) {
    Logger::Warn("option_screen", std::string("CHEAP ") + result.cheap[i].explain + " id=" +
                                      result.cheap[i].instrument_id);
  }

  // Prefer as_of (screen date / live session). Instrument JSON may be an older dump.
  const std::string day = !result.as_of.empty() ? result.as_of
                           : !result.trading_day.empty() ? result.trading_day
                                                        : "unknown";
  fs::path out_dir = opt.out_dir.empty() ? fs::path(data_root) / "live" / "option_screen"
                                         : fs::path(opt.out_dir);
  const std::string json_path = (out_dir / (day + ".json")).string();
  const std::string csv_path = (out_dir / (day + ".csv")).string();
  if (!WriteOptionScreen(json_path, csv_path, result)) return 9;

  const fs::path latest = out_dir / "latest.json";
  try {
    fs::copy_file(json_path, latest, fs::copy_options::overwrite_existing);
  } catch (const std::exception& e) {
    Logger::Warn("option_screen", std::string("latest.json copy: ") + e.what());
  }

  if (cfg_for_live && cfg_for_live->enable_trading) {
    Logger::Warn("option_screen", "ENABLE_TRADING is on but option-screen never places orders");
  }
  return 0;
}

}  // namespace qsev
