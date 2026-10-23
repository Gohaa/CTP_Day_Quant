#include "store/instrument_store.h"

#include "logging/logger.h"
#include "util/charset.h"
#include "util/ctp_product_class.h"
#include "util/ctp_sanitize.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace qsev {
namespace {

std::string JsonEscape(const std::string& s) { return JsonEscapeUtf8(ToUtf8FromCtpText(s)); }

std::string JsonChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u == 0 || u < 0x20 || u >= 0x80) return {};
  return std::string(1, c);
}

void WriteNullOrNumber(std::ostream& out, const char* key, double v, bool valid) {
  out << "\"" << key << "\":";
  if (!valid || !IsValidCtpNumber(v)) {
    out << "null";
  } else {
    out << v;
  }
}

void DeriveSessionSummary(InstrumentInfo& e) {
  e.session_day_open.clear();
  e.session_day_close.clear();
  e.session_night_open.clear();
  e.session_night_close.clear();
  auto to_min = [](const std::string& t) -> int {
    if (t.size() < 5) return -1;
    try {
      const int hh = std::stoi(t.substr(0, 2));
      const int mm = std::stoi(t.substr(3, 2));
      return hh * 60 + mm;
    } catch (...) {
      return -1;
    }
  };
  for (const auto& s : e.sessions) {
    const int b = to_min(s.time_begin);
    const int en = to_min(s.time_end);
    if (b < 0 || en < 0) continue;
    const bool overnight = en < b;                 // e.g. 21:00-02:30
    const bool nightish = overnight || b >= 20 * 60;  // start at/after 20:00
    if (nightish) {
      if (e.session_night_open.empty()) e.session_night_open = s.time_begin;
      e.session_night_close = s.time_end;
    } else {
      if (e.session_day_open.empty()) e.session_day_open = s.time_begin;
      e.session_day_close = s.time_end;
    }
  }
}

void WriteOne(std::ostream& out, const InstrumentInfo& e) {
  out << "{"
      << "\"exchange_id\":\"" << JsonEscape(e.exchange_id) << "\","
      << "\"instrument_id\":\"" << JsonEscape(e.instrument_id) << "\","
      << "\"exchange_inst_id\":\"" << JsonEscape(e.exchange_inst_id) << "\","
      << "\"product_id\":\"" << JsonEscape(e.product_id) << "\","
      << "\"instrument_name\":\"" << JsonEscape(e.instrument_name) << "\","
      << "\"product_class\":\"" << JsonEscape(JsonChar(e.product_class)) << "\","
      << "\"product_key\":\"" << JsonEscape(InstrumentStore::ProductKey(e.exchange_id, e.product_id))
      << "\","
      << "\"inst_life_phase\":\"" << JsonEscape(JsonChar(e.inst_life_phase)) << "\","
      << "\"options_type\":\"" << JsonEscape(JsonChar(e.options_type)) << "\","
      << "\"is_trading\":" << (e.is_trading ? "true" : "false") << ","
      << "\"delivery_year\":" << e.delivery_year << ","
      << "\"delivery_month\":" << e.delivery_month << ","
      << "\"volume_multiple\":" << e.volume_multiple << ","
      << "\"price_tick\":" << e.price_tick << ","
      << "\"tick_value\":" << InstrumentStore::TickValue(e) << ","
      << "\"max_market_order_volume\":" << e.max_market_order_volume << ","
      << "\"min_market_order_volume\":" << e.min_market_order_volume << ","
      << "\"max_limit_order_volume\":" << e.max_limit_order_volume << ","
      << "\"min_limit_order_volume\":" << e.min_limit_order_volume << ","
      << "\"create_date\":\"" << JsonEscape(e.create_date) << "\","
      << "\"open_date\":\"" << JsonEscape(e.open_date) << "\","
      << "\"expire_date\":\"" << JsonEscape(e.expire_date) << "\","
      << "\"start_deliv_date\":\"" << JsonEscape(e.start_deliv_date) << "\","
      << "\"end_deliv_date\":\"" << JsonEscape(e.end_deliv_date) << "\","
      << "\"strike_price\":" << e.strike_price << ","
      << "\"underlying_instr_id\":\"" << JsonEscape(e.underlying_instr_id) << "\","
      << "\"underlying_multiple\":" << e.underlying_multiple << ",";
  WriteNullOrNumber(out, "long_margin_ratio", e.long_margin_ratio, e.instrument_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "short_margin_ratio", e.short_margin_ratio, e.instrument_margin_valid);
  out << ","
      << "\"limits_valid\":" << (e.limits_valid ? "true" : "false") << ",";
  WriteNullOrNumber(out, "upper_limit_price", e.upper_limit_price, e.limits_valid);
  out << ",";
  WriteNullOrNumber(out, "lower_limit_price", e.lower_limit_price, e.limits_valid);
  out << ",";
  WriteNullOrNumber(out, "pre_settlement_price", e.pre_settlement_price, e.limits_valid);
  out << ",";
  WriteNullOrNumber(out, "limit_up_ratio", e.limit_up_ratio, e.limits_valid && e.pre_settlement_price > 0);
  out << ",";
  WriteNullOrNumber(out, "limit_down_ratio", e.limit_down_ratio,
                    e.limits_valid && e.pre_settlement_price > 0);
  out << ","
      << "\"investor_margin_valid\":" << (e.investor_margin_valid ? "true" : "false") << ",";
  WriteNullOrNumber(out, "long_margin_ratio_by_money", e.long_margin_ratio_by_money, e.investor_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "long_margin_ratio_by_volume", e.long_margin_ratio_by_volume,
                    e.investor_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "short_margin_ratio_by_money", e.short_margin_ratio_by_money,
                    e.investor_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "short_margin_ratio_by_volume", e.short_margin_ratio_by_volume,
                    e.investor_margin_valid);
  out << ","
      << "\"margin_is_relative\":" << (e.margin_is_relative ? "true" : "false") << ","
      << "\"margin_hedge_flag\":\"" << JsonEscape(JsonChar(e.margin_hedge_flag)) << "\","
      << "\"exchange_margin_valid\":" << (e.exchange_margin_valid ? "true" : "false") << ",";
  WriteNullOrNumber(out, "exch_long_margin_ratio_by_money", e.exch_long_margin_ratio_by_money,
                    e.exchange_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "exch_long_margin_ratio_by_volume", e.exch_long_margin_ratio_by_volume,
                    e.exchange_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "exch_short_margin_ratio_by_money", e.exch_short_margin_ratio_by_money,
                    e.exchange_margin_valid);
  out << ",";
  WriteNullOrNumber(out, "exch_short_margin_ratio_by_volume", e.exch_short_margin_ratio_by_volume,
                    e.exchange_margin_valid);
  out << ","
      << "\"sessions_valid\":" << (e.sessions_valid ? "true" : "false") << ","
      << "\"session_day_open\":\"" << JsonEscape(e.session_day_open) << "\","
      << "\"session_day_close\":\"" << JsonEscape(e.session_day_close) << "\","
      << "\"session_night_open\":\"" << JsonEscape(e.session_night_open) << "\","
      << "\"session_night_close\":\"" << JsonEscape(e.session_night_close) << "\","
      << "\"sessions\":[";
  for (size_t i = 0; i < e.sessions.size(); ++i) {
    const auto& s = e.sessions[i];
    if (i) out << ",";
    out << "{\"segment_no\":" << s.segment_no << ",\"time_begin\":\"" << JsonEscape(s.time_begin)
        << "\",\"time_end\":\"" << JsonEscape(s.time_end) << "\"}";
  }
  out << "]}";
}

std::string ExtractStringField(const std::string& obj, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const auto k = obj.find(pat);
  if (k == std::string::npos) return {};
  const auto colon = obj.find(':', k + pat.size());
  if (colon == std::string::npos) return {};
  size_t i = colon + 1;
  while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) ++i;
  if (i < obj.size() && obj.compare(i, 4, "null") == 0) return {};
  const auto q1 = obj.find('"', colon + 1);
  if (q1 == std::string::npos) return {};
  const auto q2 = obj.find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  return obj.substr(q1 + 1, q2 - q1 - 1);
}

bool ExtractOptionalNumber(const std::string& obj, const std::string& key, double* out) {
  const std::string pat = "\"" + key + "\"";
  const auto k = obj.find(pat);
  if (k == std::string::npos) return false;
  const auto colon = obj.find(':', k + pat.size());
  if (colon == std::string::npos) return false;
  size_t i = colon + 1;
  while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) ++i;
  if (i < obj.size() && obj.compare(i, 4, "null") == 0) return false;
  size_t j = i;
  while (j < obj.size() &&
         (std::isdigit(static_cast<unsigned char>(obj[j])) || obj[j] == '.' || obj[j] == '-' ||
          obj[j] == '+' || obj[j] == 'e' || obj[j] == 'E')) {
    ++j;
  }
  if (j == i) return false;
  try {
    *out = std::stod(obj.substr(i, j - i));
    return true;
  } catch (...) {
    return false;
  }
}

double ExtractNumberField(const std::string& obj, const std::string& key) {
  double v = 0;
  ExtractOptionalNumber(obj, key, &v);
  return v;
}

bool ExtractBoolField(const std::string& obj, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const auto k = obj.find(pat);
  if (k == std::string::npos) return false;
  const auto colon = obj.find(':', k + pat.size());
  if (colon == std::string::npos) return false;
  return obj.compare(colon + 1, 4, "true") == 0 || obj.find("true", colon + 1) < obj.find(',', colon);
}

char ExtractCharField(const std::string& obj, const std::string& key) {
  const auto s = ExtractStringField(obj, key);
  return s.empty() ? 0 : s[0];
}

}  // namespace

void InstrumentStore::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  by_id_.clear();
  trading_day_.clear();
  ready_.store(false);
  enriched_.store(false);
}

void InstrumentStore::Upsert(const InstrumentInfo& info) {
  std::lock_guard<std::mutex> lock(mu_);
  by_id_[info.instrument_id] = info;
}

void InstrumentStore::PatchLimits(const std::string& instrument_id, double upper, double lower,
                                  double pre_settlement) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_id_.find(instrument_id);
  if (it == by_id_.end()) return;
  auto& e = it->second;
  e.upper_limit_price = SanitizeCtpNumber(upper);
  e.lower_limit_price = SanitizeCtpNumber(lower);
  e.pre_settlement_price = SanitizeCtpNumber(pre_settlement);
  e.limits_valid = e.upper_limit_price > 0 || e.lower_limit_price > 0;
  if (e.pre_settlement_price > 0 && e.limits_valid) {
    e.limit_up_ratio = (e.upper_limit_price - e.pre_settlement_price) / e.pre_settlement_price;
    e.limit_down_ratio = (e.pre_settlement_price - e.lower_limit_price) / e.pre_settlement_price;
  } else {
    e.limit_up_ratio = 0;
    e.limit_down_ratio = 0;
  }
}

void InstrumentStore::PatchInvestorMargin(const std::string& instrument_id, double long_by_money,
                                          double long_by_volume, double short_by_money,
                                          double short_by_volume, bool is_relative, char hedge_flag) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_id_.find(instrument_id);
  if (it == by_id_.end()) return;
  auto& e = it->second;
  e.long_margin_ratio_by_money = SanitizeCtpRatio(long_by_money);
  e.long_margin_ratio_by_volume = SanitizeCtpNumber(long_by_volume);
  e.short_margin_ratio_by_money = SanitizeCtpRatio(short_by_money);
  e.short_margin_ratio_by_volume = SanitizeCtpNumber(short_by_volume);
  e.margin_is_relative = is_relative;
  e.margin_hedge_flag = hedge_flag;
  e.investor_margin_valid =
      IsValidCtpRatio(long_by_money) || IsValidCtpRatio(short_by_money) ||
      IsValidCtpNumber(long_by_volume) || IsValidCtpNumber(short_by_volume);
}

void InstrumentStore::PatchExchangeMargin(const std::string& instrument_id, double long_by_money,
                                          double long_by_volume, double short_by_money,
                                          double short_by_volume) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_id_.find(instrument_id);
  if (it == by_id_.end()) return;
  auto& e = it->second;
  e.exch_long_margin_ratio_by_money = SanitizeCtpRatio(long_by_money);
  e.exch_long_margin_ratio_by_volume = SanitizeCtpNumber(long_by_volume);
  e.exch_short_margin_ratio_by_money = SanitizeCtpRatio(short_by_money);
  e.exch_short_margin_ratio_by_volume = SanitizeCtpNumber(short_by_volume);
  e.exchange_margin_valid =
      IsValidCtpRatio(long_by_money) || IsValidCtpRatio(short_by_money) ||
      IsValidCtpNumber(long_by_volume) || IsValidCtpNumber(short_by_volume);
}

void InstrumentStore::PatchSessions(const std::string& instrument_id,
                                    const std::vector<TradingSegment>& sessions) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_id_.find(instrument_id);
  if (it == by_id_.end()) return;
  auto& e = it->second;
  e.sessions = sessions;
  e.sessions_valid = !sessions.empty();
  DeriveSessionSummary(e);
}

void InstrumentStore::MarkReady() { ready_.store(true); }

void InstrumentStore::MarkEnriched() { enriched_.store(true); }

void InstrumentStore::SetTradingDay(const std::string& day) {
  std::lock_guard<std::mutex> lock(mu_);
  trading_day_ = day;
}

std::string InstrumentStore::TradingDay() const {
  std::lock_guard<std::mutex> lock(mu_);
  return trading_day_;
}

const InstrumentInfo* InstrumentStore::Find(const std::string& instrument_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = by_id_.find(instrument_id);
  return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<InstrumentInfo> InstrumentStore::List() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<InstrumentInfo> out;
  out.reserve(by_id_.size());
  for (const auto& kv : by_id_) out.push_back(kv.second);
  return out;
}

size_t InstrumentStore::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return by_id_.size();
}

double InstrumentStore::TickValue(const InstrumentInfo& info) {
  return info.price_tick * static_cast<double>(info.volume_multiple);
}

double InstrumentStore::EstimateOpenMargin(const InstrumentInfo& info, bool is_short, double price,
                                           int lots) {
  if (!(price > 0) || lots <= 0 || info.volume_multiple <= 0) return 0;
  double by_money = 0;
  double by_vol = 0;
  if (is_short) {
    if (info.investor_margin_valid) {
      by_money = info.short_margin_ratio_by_money;
      by_vol = info.short_margin_ratio_by_volume;
    } else if (info.exchange_margin_valid) {
      by_money = info.exch_short_margin_ratio_by_money;
      by_vol = info.exch_short_margin_ratio_by_volume;
    } else if (info.instrument_margin_valid) {
      by_money = info.short_margin_ratio;
    }
  } else {
    if (info.investor_margin_valid) {
      by_money = info.long_margin_ratio_by_money;
      by_vol = info.long_margin_ratio_by_volume;
    } else if (info.exchange_margin_valid) {
      by_money = info.exch_long_margin_ratio_by_money;
      by_vol = info.exch_long_margin_ratio_by_volume;
    } else if (info.instrument_margin_valid) {
      by_money = info.long_margin_ratio;
    }
  }
  if (!(by_money > 0) && !(by_vol > 0)) return 0;
  const double notional = price * static_cast<double>(info.volume_multiple) * static_cast<double>(lots);
  return notional * by_money + by_vol * static_cast<double>(lots);
}

double InstrumentStore::EstimateOpenMarginOrHaircut(const InstrumentInfo& info, bool is_short,
                                                    double price, int lots) {
  const double est = EstimateOpenMargin(info, is_short, price, lots);
  if (est > 0) return est;
  if (!(price > 0) || lots <= 0 || info.volume_multiple <= 0) return 0;
  constexpr double kHaircut = 0.12;
  return price * static_cast<double>(info.volume_multiple) * static_cast<double>(lots) * kHaircut;
}

std::string InstrumentStore::ProductKey(const std::string& exchange, const std::string& product_id) {
  std::string pid = product_id;
  if (exchange == "CZCE" || exchange == "CFFEX") {
    for (char& c : pid) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  } else {
    for (char& c : pid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return exchange + "." + pid;
}

std::optional<double> InstrumentStore::TickSize(const std::string& instrument_id) const {
  const auto* p = Find(instrument_id);
  if (!p) return std::nullopt;
  return p->price_tick;
}

std::optional<int> InstrumentStore::Multiplier(const std::string& instrument_id) const {
  const auto* p = Find(instrument_id);
  if (!p) return std::nullopt;
  return p->volume_multiple;
}

std::optional<double> InstrumentStore::TickValueOf(const std::string& instrument_id) const {
  const auto* p = Find(instrument_id);
  if (!p) return std::nullopt;
  return TickValue(*p);
}

std::vector<std::string> InstrumentStore::ListProductKeys(char product_class) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> out;
  out.reserve(256);
  for (const auto& kv : by_id_) {
    const auto& e = kv.second;
    if (product_class != 0 && e.product_class != product_class) continue;
    out.push_back(ProductKey(e.exchange_id, e.product_id));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool InstrumentStore::WriteJson(const std::string& path) const {
  std::string day;
  std::vector<InstrumentInfo> rows;
  {
    std::lock_guard<std::mutex> lock(mu_);
    day = trading_day_.empty() ? "unknown" : trading_day_;
    rows.reserve(by_id_.size());
    for (const auto& kv : by_id_) rows.push_back(kv.second);
  }
  return WriteJson(path, day, rows);
}

bool InstrumentStore::WriteJson(const std::string& path, const std::string& trading_day,
                                const std::vector<InstrumentInfo>& rows) {
  namespace fs = std::filesystem;
  try {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
  } catch (const std::exception& e) {
    Logger::Error("instruments", std::string("mkdir failed: ") + e.what());
    return false;
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    Logger::Error("instruments", "cannot write " + path);
    return false;
  }

  size_t futures = 0;
  size_t options = 0;
  size_t spot_opt = 0;
  size_t other = 0;
  size_t with_limits = 0;
  size_t with_inv_margin = 0;
  size_t with_exch_margin = 0;
  size_t with_sessions = 0;
  for (const auto& e : rows) {
    if (IsFuturesClass(e.product_class)) ++futures;
    else if (e.product_class == kPcOptions) ++options;
    else if (e.product_class == kPcSpotOption) ++spot_opt;
    else ++other;
    if (e.limits_valid) ++with_limits;
    if (e.investor_margin_valid) ++with_inv_margin;
    if (e.exchange_margin_valid) ++with_exch_margin;
    if (e.sessions_valid) ++with_sessions;
  }

  out << "{\n";
  out << "  \"trading_day\": \"" << JsonEscape(trading_day) << "\",\n";
  out << "  \"count\": " << rows.size() << ",\n";
  out << "  \"futures\": " << futures << ",\n";
  out << "  \"options\": " << options << ",\n";
  out << "  \"spot_options\": " << spot_opt << ",\n";
  out << "  \"other\": " << other << ",\n";
  out << "  \"with_limits\": " << with_limits << ",\n";
  out << "  \"with_investor_margin\": " << with_inv_margin << ",\n";
  out << "  \"with_exchange_margin\": " << with_exch_margin << ",\n";
  out << "  \"with_sessions\": " << with_sessions << ",\n";
  out << "  \"instruments\": [\n";
  for (size_t i = 0; i < rows.size(); ++i) {
    out << "    ";
    WriteOne(out, rows[i]);
    if (i + 1 < rows.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  Logger::Info("instruments", "wrote " + path + " count=" + std::to_string(rows.size()) +
                                  " futures=" + std::to_string(futures) +
                                  " options=" + std::to_string(options) +
                                  " limits=" + std::to_string(with_limits) +
                                  " inv_margin=" + std::to_string(with_inv_margin) +
                                  " exch_margin=" + std::to_string(with_exch_margin) +
                                  " sessions=" + std::to_string(with_sessions));
  return true;
}

bool InstrumentStore::LoadJson(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Logger::Error("instruments", "cannot open " + path);
    return false;
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  const std::string text = oss.str();

  Clear();
  SetTradingDay(ExtractStringField(text, "trading_day"));

  const auto arr = text.find("\"instruments\"");
  if (arr == std::string::npos) {
    Logger::Error("instruments", "missing instruments array");
    return false;
  }
  size_t i = text.find('[', arr);
  if (i == std::string::npos) return false;
  ++i;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t' ||
                               text[i] == ',')) {
      ++i;
    }
    if (i >= text.size() || text[i] == ']') break;
    if (text[i] != '{') {
      ++i;
      continue;
    }
    int depth = 0;
    size_t j = i;
    for (; j < text.size(); ++j) {
      if (text[j] == '{') ++depth;
      else if (text[j] == '}') {
        --depth;
        if (depth == 0) {
          ++j;
          break;
        }
      }
    }
    const std::string obj = text.substr(i, j - i);
    InstrumentInfo e;
    e.exchange_id = ExtractStringField(obj, "exchange_id");
    e.instrument_id = ExtractStringField(obj, "instrument_id");
    e.exchange_inst_id = ExtractStringField(obj, "exchange_inst_id");
    e.product_id = ExtractStringField(obj, "product_id");
    e.instrument_name = ExtractStringField(obj, "instrument_name");
    e.product_class = ExtractCharField(obj, "product_class");
    e.inst_life_phase = ExtractCharField(obj, "inst_life_phase");
    e.options_type = ExtractCharField(obj, "options_type");
    e.is_trading = ExtractBoolField(obj, "is_trading");
    e.delivery_year = static_cast<int>(ExtractNumberField(obj, "delivery_year"));
    e.delivery_month = static_cast<int>(ExtractNumberField(obj, "delivery_month"));
    e.volume_multiple = static_cast<int>(ExtractNumberField(obj, "volume_multiple"));
    e.price_tick = ExtractNumberField(obj, "price_tick");
    e.max_market_order_volume = static_cast<int>(ExtractNumberField(obj, "max_market_order_volume"));
    e.min_market_order_volume = static_cast<int>(ExtractNumberField(obj, "min_market_order_volume"));
    e.max_limit_order_volume = static_cast<int>(ExtractNumberField(obj, "max_limit_order_volume"));
    e.min_limit_order_volume = static_cast<int>(ExtractNumberField(obj, "min_limit_order_volume"));
    e.create_date = ExtractStringField(obj, "create_date");
    e.open_date = ExtractStringField(obj, "open_date");
    e.expire_date = ExtractStringField(obj, "expire_date");
    e.start_deliv_date = ExtractStringField(obj, "start_deliv_date");
    e.end_deliv_date = ExtractStringField(obj, "end_deliv_date");
    e.strike_price = ExtractNumberField(obj, "strike_price");
    e.underlying_instr_id = ExtractStringField(obj, "underlying_instr_id");
    e.underlying_multiple = ExtractNumberField(obj, "underlying_multiple");
    e.instrument_margin_valid =
        ExtractOptionalNumber(obj, "long_margin_ratio", &e.long_margin_ratio) ||
        ExtractOptionalNumber(obj, "short_margin_ratio", &e.short_margin_ratio);
    e.limits_valid = ExtractBoolField(obj, "limits_valid");
    ExtractOptionalNumber(obj, "upper_limit_price", &e.upper_limit_price);
    ExtractOptionalNumber(obj, "lower_limit_price", &e.lower_limit_price);
    ExtractOptionalNumber(obj, "pre_settlement_price", &e.pre_settlement_price);
    ExtractOptionalNumber(obj, "limit_up_ratio", &e.limit_up_ratio);
    ExtractOptionalNumber(obj, "limit_down_ratio", &e.limit_down_ratio);
    e.investor_margin_valid = ExtractBoolField(obj, "investor_margin_valid");
    ExtractOptionalNumber(obj, "long_margin_ratio_by_money", &e.long_margin_ratio_by_money);
    ExtractOptionalNumber(obj, "long_margin_ratio_by_volume", &e.long_margin_ratio_by_volume);
    ExtractOptionalNumber(obj, "short_margin_ratio_by_money", &e.short_margin_ratio_by_money);
    ExtractOptionalNumber(obj, "short_margin_ratio_by_volume", &e.short_margin_ratio_by_volume);
    e.margin_is_relative = ExtractBoolField(obj, "margin_is_relative");
    e.margin_hedge_flag = ExtractCharField(obj, "margin_hedge_flag");
    e.exchange_margin_valid = ExtractBoolField(obj, "exchange_margin_valid");
    ExtractOptionalNumber(obj, "exch_long_margin_ratio_by_money", &e.exch_long_margin_ratio_by_money);
    ExtractOptionalNumber(obj, "exch_long_margin_ratio_by_volume", &e.exch_long_margin_ratio_by_volume);
    ExtractOptionalNumber(obj, "exch_short_margin_ratio_by_money", &e.exch_short_margin_ratio_by_money);
    ExtractOptionalNumber(obj, "exch_short_margin_ratio_by_volume", &e.exch_short_margin_ratio_by_volume);
    e.sessions_valid = ExtractBoolField(obj, "sessions_valid");
    e.session_day_open = ExtractStringField(obj, "session_day_open");
    e.session_day_close = ExtractStringField(obj, "session_day_close");
    e.session_night_open = ExtractStringField(obj, "session_night_open");
    e.session_night_close = ExtractStringField(obj, "session_night_close");
    // Reload session segments if present.
    {
      const auto skey = obj.find("\"sessions\"");
      if (skey != std::string::npos) {
        const auto a1 = obj.find('[', skey);
        const auto a2 = obj.find(']', a1 == std::string::npos ? 0 : a1);
        if (a1 != std::string::npos && a2 != std::string::npos && a2 > a1) {
          const std::string arr = obj.substr(a1, a2 - a1 + 1);
          size_t p = 0;
          while (true) {
            const auto o1 = arr.find('{', p);
            if (o1 == std::string::npos) break;
            const auto o2 = arr.find('}', o1);
            if (o2 == std::string::npos) break;
            const std::string sobj = arr.substr(o1, o2 - o1 + 1);
            TradingSegment s;
            s.segment_no = static_cast<int>(ExtractNumberField(sobj, "segment_no"));
            s.time_begin = ExtractStringField(sobj, "time_begin");
            s.time_end = ExtractStringField(sobj, "time_end");
            if (!s.time_begin.empty()) e.sessions.push_back(s);
            p = o2 + 1;
          }
          if (!e.sessions.empty()) {
            e.sessions_valid = true;
            DeriveSessionSummary(e);
          }
        }
      }
    }
    if (!e.instrument_id.empty()) Upsert(e);
    i = j;
  }
  MarkReady();
  MarkEnriched();
  Logger::Info("instruments", "loaded " + path + " size=" + std::to_string(Size()) +
                                  " day=" + TradingDay());
  return Size() > 0;
}

}  // namespace qsev
