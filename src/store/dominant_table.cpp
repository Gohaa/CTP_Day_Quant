#include "store/dominant_table.h"

#include "logging/logger.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace qsev {
namespace {

std::string ExtractJsonString(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const auto k = s.find(pat);
  if (k == std::string::npos) return {};
  const auto colon = s.find(':', k + pat.size());
  if (colon == std::string::npos) return {};
  const auto q1 = s.find('"', colon + 1);
  if (q1 == std::string::npos) return {};
  const auto q2 = s.find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  return s.substr(q1 + 1, q2 - q1 - 1);
}

double ExtractJsonNumber(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const auto k = s.find(pat);
  if (k == std::string::npos) return 0;
  const auto colon = s.find(':', k + pat.size());
  if (colon == std::string::npos) return 0;
  size_t i = colon + 1;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  size_t j = i;
  while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.' || s[j] == 'e' ||
                          s[j] == 'E' || s[j] == '+' || s[j] == '-')) {
    ++j;
  }
  if (j == i) return 0;
  try {
    return std::stod(s.substr(i, j - i));
  } catch (...) {
    return 0;
  }
}

bool ProductAllowed(const std::string& key, const std::vector<std::string>& filter) {
  if (filter.empty()) return true;
  for (const auto& f : filter) {
    if (f == key) return true;
  }
  return false;
}

}  // namespace

DominantTable DominantTable::LoadFromFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open dominant table: " + path);
  std::ostringstream oss;
  oss << in.rdbuf();
  const std::string text = oss.str();

  DominantTable t;
  t.trading_day_ = ExtractJsonString(text, "trading_day");
  if (t.trading_day_.empty()) t.trading_day_ = "unknown";

  // Parse each "EXCH.prod": { ... } block inside "products".
  const auto prod_pos = text.find("\"products\"");
  if (prod_pos == std::string::npos) {
    throw std::runtime_error("dominant json missing products: " + path);
  }
  size_t i = text.find('{', prod_pos);
  if (i == std::string::npos) throw std::runtime_error("dominant json malformed products");
  ++i;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t' ||
                               text[i] == ',')) {
      ++i;
    }
    if (i >= text.size() || text[i] == '}') break;
    if (text[i] != '"') {
      ++i;
      continue;
    }
    const auto key_end = text.find('"', i + 1);
    if (key_end == std::string::npos) break;
    const std::string product_key = text.substr(i + 1, key_end - i - 1);
    const auto brace = text.find('{', key_end);
    if (brace == std::string::npos) break;
    int depth = 0;
    size_t j = brace;
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
    const std::string block = text.substr(brace, j - brace);
    DominantEntry e;
    e.product_key = product_key;
    e.dominant = ExtractJsonString(block, "dominant");
    e.second = ExtractJsonString(block, "second");
    e.dominant_oi = ExtractJsonNumber(block, "dominant_oi");
    e.second_oi = ExtractJsonNumber(block, "second_oi");
    e.dominant_volume = static_cast<int>(ExtractJsonNumber(block, "dominant_volume"));
    e.second_volume = static_cast<int>(ExtractJsonNumber(block, "second_volume"));
    t.entries_.push_back(e);
    i = j;
  }

  {
    static std::mutex log_mu;
    static std::string last_path;
    std::lock_guard<std::mutex> lock(log_mu);
    if (path != last_path) {
      last_path = path;
      Logger::Info("dominant_table", "loaded " + path + " day=" + t.trading_day_ +
                                         " products=" + std::to_string(t.entries_.size()));
    }
  }
  return t;
}

DominantTable DominantTable::LoadLatest(const std::string& data_root) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::path(data_root) / "live" / "dominant";
  if (!fs::exists(dir)) {
    throw std::runtime_error("dominant dir missing: " + dir.string());
  }
  std::string best_name;
  fs::path best_path;
  for (const auto& ent : fs::directory_iterator(dir)) {
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() != ".json") continue;
    const std::string name = ent.path().stem().string();
    if (best_name.empty() || name > best_name) {
      best_name = name;
      best_path = ent.path();
    }
  }
  if (best_path.empty()) {
    throw std::runtime_error("no dominant json under " + dir.string());
  }
  return LoadFromFile(best_path.string());
}

bool DominantTable::WriteDayFile(const std::string& data_root, const std::string& trading_day) const {
  namespace fs = std::filesystem;
  if (trading_day.empty() || trading_day == "unknown" || entries_.empty()) return false;
  const fs::path path = fs::path(data_root) / "live" / "dominant" / (trading_day + ".json");
  std::error_code ec;
  if (fs::exists(path, ec)) return true;
  if (!DominantSelector::WriteJson(path.string(), trading_day, entries_)) return false;
  Logger::Info("dominant_table", "wrote " + path.string() + " from last select day=" + trading_day_);
  return true;
}

DominantTable DominantTable::LoadForTradingDay(const std::string& data_root,
                                               const std::string& trading_day) {
  namespace fs = std::filesystem;
  if (!trading_day.empty() && trading_day != "unknown") {
    const fs::path want = fs::path(data_root) / "live" / "dominant" / (trading_day + ".json");
    std::error_code ec;
    if (fs::exists(want, ec)) return LoadFromFile(want.string());
    auto latest = LoadLatest(data_root);
    latest.WriteDayFile(data_root, trading_day);
    if (fs::exists(want, ec)) return LoadFromFile(want.string());
    return latest;
  }
  return LoadLatest(data_root);
}

std::vector<std::string> DominantTable::InstrumentIds(const std::vector<std::string>& product_filter,
                                                      bool skip_zero_oi,
                                                      bool include_second) const {
  std::vector<std::string> out;
  for (const auto& e : entries_) {
    if (!ProductAllowed(e.product_key, product_filter)) continue;
    if (!e.dominant.empty() && (!skip_zero_oi || e.dominant_oi > 0)) {
      out.push_back(e.dominant);
    }
    if (include_second && !e.second.empty() && (!skip_zero_oi || e.second_oi > 0)) {
      out.push_back(e.second);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace qsev
