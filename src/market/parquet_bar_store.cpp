#include "market/parquet_bar_store.h"

#include "logging/logger.h"
#include "util/time_shanghai.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace qsev {
namespace {

namespace fs = std::filesystem;

std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
      o.push_back(c);
    } else if (c == '\n') {
      o += "\\n";
    } else {
      o.push_back(c);
    }
  }
  return o;
}

std::string DefaultHelper(const std::string& data_root) {
  return (fs::path(data_root).parent_path() / "tools" / "klines_parquet_write.py").string();
}

std::string DefaultReadHelper(const std::string& data_root) {
  return (fs::path(data_root).parent_path() / "tools" / "klines_parquet_read.py").string();
}

std::string DirFingerprint(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  std::ostringstream oss;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    if (e.path().extension() != ".parquet") continue;
    std::error_code ec2;
    const auto sz = e.file_size(ec2);
    oss << e.path().filename().string() << ':' << sz << ';';
  }
  return oss.str();
}

double JsonNumAt(const std::string& t, size_t from, const char* key) {
  const std::string pat = std::string("\"") + key + "\"";
  auto k = t.find(pat, from);
  if (k == std::string::npos) return 0;
  auto c = t.find(':', k);
  if (c == std::string::npos) return 0;
  return std::atof(t.c_str() + c + 1);
}

int64_t JsonIntAt(const std::string& t, size_t from, const char* key) {
  return static_cast<int64_t>(JsonNumAt(t, from, key));
}

bool ParseBarsJson(const std::string& body, std::vector<KlineBar>* out) {
  if (!out) return false;
  out->clear();
  auto arr0 = body.find("\"bars\"");
  if (arr0 == std::string::npos) return false;
  arr0 = body.find('[', arr0);
  if (arr0 == std::string::npos) return false;
  size_t i = arr0;
  while (true) {
    const auto o1 = body.find('{', i);
    if (o1 == std::string::npos) break;
    const auto o2 = body.find('}', o1);
    if (o2 == std::string::npos) break;
    KlineBar b;
    b.open = JsonNumAt(body, o1, "open");
    b.high = JsonNumAt(body, o1, "high");
    b.low = JsonNumAt(body, o1, "low");
    b.close = JsonNumAt(body, o1, "close");
    b.volume = JsonNumAt(body, o1, "volume");
    b.time_unix = JsonIntAt(body, o1, "time_unix");
    if (b.close > 0) out->push_back(b);
    i = o2 + 1;
    if (body.find(']', i) < body.find('{', i)) break;
  }
  return !out->empty();
}

struct LoadCacheEntry {
  std::string fingerprint;
  int limit{0};
  std::vector<KlineBar> bars;
};

std::mutex g_load_mu;
std::unordered_map<std::string, LoadCacheEntry> g_load_cache;

}  // namespace

ParquetBarStore::ParquetBarStore(std::string data_root, std::string helper_py)
    : data_root_(std::move(data_root)), helper_py_(std::move(helper_py)) {
  if (helper_py_.empty()) {
    helper_py_ = DefaultHelper(data_root_);
  }
}

std::string ParquetBarStore::PathFor(const std::string& data_root, const Bar& bar) {
  const int64_t sec = bar.datetime_ns / 1000000000LL;
  const ShanghaiWall wall = UnixSecToShanghai(sec);
  const int year = wall.date.year;
  return (fs::path(data_root) / "klines" / bar.product_key /
          (std::to_string(bar.interval_min) + "m") / (std::to_string(year) + ".parquet"))
      .string();
}

void ParquetBarStore::Append(const Bar& bar) {
  if (bar.product_key.empty() || bar.datetime_ns <= 0) {
    return;
  }
  Row r;
  r.datetime_ns = bar.datetime_ns;
  r.open = bar.open;
  r.high = bar.high;
  r.low = bar.low;
  r.close = bar.close;
  r.volume = bar.volume;
  r.time_unix = bar.time_unix;
  const std::string path = PathFor(data_root_, bar);
  std::lock_guard<std::mutex> lock(mu_);
  pending_[path].push_back(r);
}

std::size_t ParquetBarStore::PendingBars() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::size_t n = 0;
  for (const auto& kv : pending_) {
    n += kv.second.size();
  }
  return n;
}

bool ParquetBarStore::FlushPath(const std::string& path, const std::vector<Row>& rows, bool replace) {
  if (rows.empty()) {
    return true;
  }
  if (!fs::exists(helper_py_)) {
    Logger::Error("parquet", "helper missing: " + helper_py_);
    return false;
  }

  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const fs::path job =
      fs::temp_directory_path() / ("qsev_klines_" + std::to_string(stamp) + ".json");
  {
    std::ostringstream oss;
    oss << "{\"path\":\"" << JsonEscape(path) << "\",\"replace\":" << (replace ? "true" : "false")
        << ",\"rows\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      if (i) {
        oss << ',';
      }
      oss << "{\"datetime\":" << r.datetime_ns << ",\"open\":" << r.open << ",\"high\":" << r.high
          << ",\"low\":" << r.low << ",\"close\":" << r.close << ",\"volume\":" << r.volume
          << ",\"time_unix\":" << r.time_unix << '}';
    }
    oss << "]}";
    std::ofstream out(job, std::ios::binary);
    if (!out) {
      Logger::Error("parquet", "cannot write job json");
      return false;
    }
    out << oss.str();
  }

  // Prefer `py` on Windows store Python, then `python`.
  const std::string cmd1 = "py -3 \"" + helper_py_ + "\" \"" + job.string() + "\"";
  const std::string cmd2 = "python \"" + helper_py_ + "\" \"" + job.string() + "\"";
  int rc = std::system(cmd1.c_str());
  if (rc != 0) {
    rc = std::system(cmd2.c_str());
  }
  std::error_code ec;
  fs::remove(job, ec);
  if (rc != 0) {
    Logger::Error("parquet", "helper failed rc=" + std::to_string(rc) + " path=" + path);
    return false;
  }
  Logger::Info("parquet", "flushed +" + std::to_string(rows.size()) + " -> " + path);
  return true;
}

std::size_t ParquetBarStore::Flush(bool replace) {
  std::unordered_map<std::string, std::vector<Row>> batch;
  {
    std::lock_guard<std::mutex> lock(mu_);
    batch.swap(pending_);
  }
  std::size_t ok = 0;
  for (auto& kv : batch) {
    if (FlushPath(kv.first, kv.second, replace)) {
      ++ok;
    } else {
      // put back on failure
      std::lock_guard<std::mutex> lock(mu_);
      auto& dst = pending_[kv.first];
      dst.insert(dst.end(), kv.second.begin(), kv.second.end());
    }
  }
  return ok;
}

bool ParquetBarStore::LoadRecent(const std::string& data_root, const std::string& product_key,
                                 int interval_min, int limit, std::vector<KlineBar>* out) {
  if (!out || data_root.empty() || product_key.empty() || interval_min <= 0) return false;
  out->clear();
  const int use_limit = limit > 0 ? limit : 600;
  const fs::path idir =
      fs::path(data_root) / "klines" / product_key / (std::to_string(interval_min) + "m");
  const std::string fingerprint = DirFingerprint(idir);
  const std::string cache_key =
      product_key + "|" + std::to_string(interval_min) + "|" + std::to_string(use_limit);

  {
    std::lock_guard<std::mutex> lock(g_load_mu);
    auto it = g_load_cache.find(cache_key);
    if (it != g_load_cache.end() && it->second.fingerprint == fingerprint &&
        it->second.limit == use_limit && !it->second.bars.empty()) {
      *out = it->second.bars;
      return true;
    }
  }

  if (fingerprint.empty()) return false;

  const std::string helper = DefaultReadHelper(data_root);
  if (!fs::exists(helper)) {
    Logger::Error("parquet", "read helper missing: " + helper);
    return false;
  }

  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const fs::path job =
      fs::temp_directory_path() / ("qsev_klines_r_" + std::to_string(stamp) + ".json");
  const fs::path result =
      fs::temp_directory_path() / ("qsev_klines_r_" + std::to_string(stamp) + "_out.json");
  {
    std::ostringstream oss;
    oss << "{\"data_root\":\"" << JsonEscape(data_root) << "\",\"product\":\""
        << JsonEscape(product_key) << "\",\"interval_min\":" << interval_min
        << ",\"limit\":" << use_limit << ",\"out\":\"" << JsonEscape(result.string()) << "\"}";
    std::ofstream outf(job, std::ios::binary);
    if (!outf) return false;
    outf << oss.str();
  }

  const std::string cmd1 = "py -3 \"" + helper + "\" --job \"" + job.string() + "\"";
  const std::string cmd2 = "python \"" + helper + "\" --job \"" + job.string() + "\"";
  int rc = std::system(cmd1.c_str());
  if (rc != 0) rc = std::system(cmd2.c_str());
  std::error_code ec;
  fs::remove(job, ec);
  if (rc != 0 || !fs::exists(result)) {
    fs::remove(result, ec);
    Logger::Error("parquet", "read helper failed product=" + product_key);
    return false;
  }

  std::string body;
  {
    std::ifstream in(result, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    body = oss.str();
  }
  fs::remove(result, ec);
  if (!ParseBarsJson(body, out)) return false;

  {
    std::lock_guard<std::mutex> lock(g_load_mu);
    LoadCacheEntry e;
    e.fingerprint = fingerprint;
    e.limit = use_limit;
    e.bars = *out;
    g_load_cache[cache_key] = std::move(e);
  }
  return true;
}

}  // namespace qsev
