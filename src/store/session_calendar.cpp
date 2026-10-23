#include "store/session_calendar.h"

#include "logging/logger.h"
#include "util/http_get.h"
#include "util/time_shanghai.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace qsev {
namespace {

std::string ExtractString(const std::string& obj, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const auto k = obj.find(pat);
  if (k == std::string::npos) return {};
  const auto colon = obj.find(':', k + pat.size());
  if (colon == std::string::npos) return {};
  const auto q1 = obj.find('"', colon + 1);
  if (q1 == std::string::npos) return {};
  const auto q2 = obj.find('"', q1 + 1);
  if (q2 == std::string::npos) return {};
  return obj.substr(q1 + 1, q2 - q1 - 1);
}

int ExtractInt(const std::string& obj, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const auto k = obj.find(pat);
  if (k == std::string::npos) return 0;
  const auto colon = obj.find(':', k + pat.size());
  if (colon == std::string::npos) return 0;
  size_t i = colon + 1;
  while (i < obj.size() && (obj[i] == ' ' || obj[i] == '\t')) ++i;
  size_t j = i;
  while (j < obj.size() && (std::isdigit(static_cast<unsigned char>(obj[j])) || obj[j] == '-')) ++j;
  if (j == i) return 0;
  try {
    return std::stoi(obj.substr(i, j - i));
  } catch (...) {
    return 0;
  }
}

std::string NormalizeProductKey(const std::string& exchange, const std::string& product_id) {
  std::string pid = product_id;
  if (exchange == "CZCE" || exchange == "CFFEX") {
    for (char& c : pid) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  } else {
    for (char& c : pid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return exchange + "." + pid;
}

std::string JsonEscape(const std::string& s) {
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

void SortSegments(std::vector<TradingSegment>& segs) {
  std::sort(segs.begin(), segs.end(),
            [](const TradingSegment& a, const TradingSegment& b) { return a.segment_no < b.segment_no; });
}

}  // namespace

SessionCalendar SessionCalendar::LoadFromJson(const std::string& json_text, const std::string& source_tag) {
  SessionCalendar cal;
  cal.source_ = source_tag.empty() ? ExtractString(json_text, "source") : source_tag;
  if (cal.source_.empty()) cal.source_ = "json";

  // Cache shape: { "products": { "SHFE.rb": [ {segment_no,time_begin,time_end}, ... ] } }
  const auto prod = json_text.find("\"products\"");
  if (prod != std::string::npos) {
    size_t i = json_text.find('{', prod);
    if (i != std::string::npos) {
      ++i;
      while (i < json_text.size()) {
        while (i < json_text.size() &&
               (json_text[i] == ' ' || json_text[i] == '\n' || json_text[i] == '\r' ||
                json_text[i] == '\t' || json_text[i] == ',')) {
          ++i;
        }
        if (i >= json_text.size() || json_text[i] == '}') break;
        if (json_text[i] != '"') {
          ++i;
          continue;
        }
        const auto q2 = json_text.find('"', i + 1);
        if (q2 == std::string::npos) break;
        const std::string key = json_text.substr(i + 1, q2 - i - 1);
        i = json_text.find('[', q2);
        if (i == std::string::npos) break;
        const auto arr_end = json_text.find(']', i);
        if (arr_end == std::string::npos) break;
        const std::string arr = json_text.substr(i, arr_end - i + 1);
        std::vector<TradingSegment> segs;
        size_t p = 0;
        while (true) {
          const auto o1 = arr.find('{', p);
          if (o1 == std::string::npos) break;
          const auto o2 = arr.find('}', o1);
          if (o2 == std::string::npos) break;
          const std::string obj = arr.substr(o1, o2 - o1 + 1);
          TradingSegment s;
          s.segment_no = ExtractInt(obj, "segment_no");
          if (s.segment_no == 0) s.segment_no = ExtractInt(obj, "SegmentNo");
          s.time_begin = ExtractString(obj, "time_begin");
          if (s.time_begin.empty()) s.time_begin = ExtractString(obj, "TimeBegin");
          s.time_end = ExtractString(obj, "time_end");
          if (s.time_end.empty()) s.time_end = ExtractString(obj, "TimeEnd");
          if (!s.time_begin.empty() && !s.time_end.empty()) segs.push_back(s);
          p = o2 + 1;
        }
        if (!segs.empty()) {
          SortSegments(segs);
          cal.by_product_[key] = std::move(segs);
        }
        i = arr_end + 1;
      }
    }
  }

  // openctp /times raw: { "data": [ {ExchangeID,ProductID,SegmentNo,TimeBegin,TimeEnd}, ... ] }
  if (cal.by_product_.empty()) {
    const auto data_pos = json_text.find("\"data\"");
    if (data_pos != std::string::npos) {
      size_t i = json_text.find('[', data_pos);
      if (i != std::string::npos) {
        ++i;
        while (i < json_text.size()) {
          while (i < json_text.size() &&
                 (json_text[i] == ' ' || json_text[i] == '\n' || json_text[i] == '\r' ||
                  json_text[i] == '\t' || json_text[i] == ',')) {
            ++i;
          }
          if (i >= json_text.size() || json_text[i] == ']') break;
          if (json_text[i] != '{') {
            ++i;
            continue;
          }
          int depth = 0;
          size_t j = i;
          for (; j < json_text.size(); ++j) {
            if (json_text[j] == '{') ++depth;
            else if (json_text[j] == '}') {
              --depth;
              if (depth == 0) {
                ++j;
                break;
              }
            }
          }
          const std::string obj = json_text.substr(i, j - i);
          const std::string ex = ExtractString(obj, "ExchangeID");
          const std::string pid = ExtractString(obj, "ProductID");
          TradingSegment s;
          s.segment_no = ExtractInt(obj, "SegmentNo");
          s.time_begin = ExtractString(obj, "TimeBegin");
          s.time_end = ExtractString(obj, "TimeEnd");
          if (!ex.empty() && !pid.empty() && !s.time_begin.empty() && !s.time_end.empty()) {
            cal.by_product_[NormalizeProductKey(ex, pid)].push_back(s);
          }
          i = j;
        }
        for (auto& kv : cal.by_product_) SortSegments(kv.second);
      }
    }
  }

  Logger::Info("sessions", "parsed source=" + cal.source_ +
                                " products=" + std::to_string(cal.by_product_.size()));
  return cal;
}

SessionCalendar SessionCalendar::LoadFromFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Logger::Error("sessions", "cannot open " + path);
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return LoadFromJson(oss.str(), path);
}

SessionCalendar SessionCalendar::FetchOpenCtpTimes(const std::string& url, const std::string& cache_path) {
  Logger::Info("sessions", "fetching " + url);
  const std::string body = HttpGet(url, 90000);
  if (body.empty()) {
    Logger::Error("sessions", "fetch empty body");
    return {};
  }
  auto cal = LoadFromJson(body, url);
  if (!cal.Empty() && !cache_path.empty()) {
    cal.WriteCache(cache_path);
  }
  return cal;
}

bool SessionCalendar::WriteCache(const std::string& path) const {
  namespace fs = std::filesystem;
  try {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
  } catch (const std::exception& e) {
    Logger::Error("sessions", std::string("mkdir failed: ") + e.what());
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    Logger::Error("sessions", "cannot write " + path);
    return false;
  }
  out << "{\n  \"source\": \"" << JsonEscape(source_) << "\",\n  \"products\": {\n";
  bool first_prod = true;
  for (const auto& kv : by_product_) {
    if (!first_prod) out << ",\n";
    first_prod = false;
    out << "    \"" << JsonEscape(kv.first) << "\": [";
    for (size_t i = 0; i < kv.second.size(); ++i) {
      const auto& s = kv.second[i];
      if (i) out << ", ";
      out << "{\"segment_no\":" << s.segment_no << ",\"time_begin\":\"" << JsonEscape(s.time_begin)
          << "\",\"time_end\":\"" << JsonEscape(s.time_end) << "\"}";
    }
    out << "]";
  }
  out << "\n  }\n}\n";
  Logger::Info("sessions", "cached " + path + " products=" + std::to_string(by_product_.size()));
  return true;
}

const std::vector<TradingSegment>* SessionCalendar::Find(const std::string& product_key) const {
  const auto it = by_product_.find(product_key);
  return it == by_product_.end() ? nullptr : &it->second;
}

namespace {

bool IsNightishBegin(int begin_sec) {
  // Night sessions typically start at/after 20:00.
  return begin_sec >= 20 * 3600;
}

bool IsOvernightSegment(int begin_sec, int end_sec) {
  // e.g. 21:00–02:30 or 21:00–01:00
  return end_sec < begin_sec;
}

bool LocalSecInSegment(int sod, int begin_sec, int end_sec) {
  if (IsOvernightSegment(begin_sec, end_sec)) {
    return sod >= begin_sec || sod < end_sec;
  }
  return sod >= begin_sec && sod < end_sec;
}

}  // namespace

bool SessionCalendar::IsInSession(const std::string& product_key, int64_t unix_ns) const {
  const auto* segs = Find(product_key);
  if (!segs || segs->empty()) {
    // No calendar: accept all (minute bars still wall-clock aligned).
    return true;
  }
  const auto wall = UnixSecToShanghai(unix_ns / 1000000000LL);
  const int sod = wall.hour * 3600 + wall.minute * 60 + wall.second;
  for (const auto& s : *segs) {
    const int b = ParseHmsToSec(s.time_begin);
    const int e = ParseHmsToSec(s.time_end);
    if (b < 0 || e < 0) continue;
    if (LocalSecInSegment(sod, b, e)) return true;
  }
  return false;
}

SessionBound SessionCalendar::DayBounds(const std::string& product_key,
                                        const std::string& trading_day) const {
  SessionBound out;
  CivilDate td{};
  if (!ParseYyyymmdd(trading_day, &td)) return out;
  const auto* segs = Find(product_key);
  if (!segs || segs->empty()) {
    // Fallback: 09:00–15:00 on trading day calendar.
    out.open_unix_ns = ShanghaiWallToUnixNs(td, 9 * 3600, 0);
    out.close_unix_sec = ShanghaiWallToUnixSec(td, 15 * 3600);
    out.ok = true;
    return out;
  }

  bool first = true;
  int64_t open_ns = 0;
  int64_t close_sec = 0;

  for (const auto& s : *segs) {
    const int b = ParseHmsToSec(s.time_begin);
    const int e = ParseHmsToSec(s.time_end);
    if (b < 0 || e < 0) continue;

    CivilDate begin_date = td;
    CivilDate end_date = td;
    if (IsOvernightSegment(b, e)) {
      // 21:00–02:30: Monday TradingDay night starts Friday 21:00 (weekend), not Sunday.
      begin_date = AddDays(td, -1);
      while (IsWeekend(begin_date)) begin_date = AddDays(begin_date, -1);
      end_date = td;
    } else if (IsNightishBegin(b)) {
      // 21:00–23:00: Monday TradingDay night is Friday 21:00 (no Sat/Sun session).
      begin_date = AddDays(td, -1);
      while (IsWeekend(begin_date)) begin_date = AddDays(begin_date, -1);
      end_date = begin_date;
    }

    const int64_t seg_open_ns = ShanghaiWallToUnixNs(begin_date, b, 0);
    const int64_t seg_close_sec = ShanghaiWallToUnixSec(end_date, e);
    if (first || seg_open_ns < open_ns) open_ns = seg_open_ns;
    if (first || seg_close_sec > close_sec) close_sec = seg_close_sec;
    first = false;
  }

  if (first) return out;
  out.open_unix_ns = open_ns;
  out.close_unix_sec = close_sec;
  out.ok = true;
  return out;
}

int64_t SessionOpenUnixFromLast(const std::vector<TradingSegment>& segs, int64_t last_unix_sec) {
  if (last_unix_sec <= 0) return 0;
  const auto wall = UnixSecToShanghai(last_unix_sec);
  const int sod = wall.hour * 3600 + wall.minute * 60 + wall.second;
  int night_begin = -1;
  int night_end = -1;
  int day_begin = -1;
  for (const auto& s : segs) {
    const int b = ParseHmsToSec(s.time_begin);
    const int e = ParseHmsToSec(s.time_end);
    if (b < 0) continue;
    if (b >= 20 * 3600) {
      if (night_begin < 0) {
        night_begin = b;
        night_end = e;
      }
    } else if (day_begin < 0) {
      day_begin = b;
    }
  }
  if (night_begin >= 0) {
    const bool overnight = night_end >= 0 && night_end < night_begin;
    const bool in_night = sod >= night_begin || (overnight && sod < night_end);
    CivilDate d = wall.date;
    if (in_night) {
      if (sod < night_begin) d = AddDays(d, -1);
    } else {
      d = AddDays(d, -1);
    }
    // Non-overnight night (21:00–23:00): skip Sat/Sun so Monday maps to Friday.
    if (!overnight) {
      while (IsWeekend(d)) d = AddDays(d, -1);
    }
    return ShanghaiWallToUnixSec(d, night_begin);
  }
  if (day_begin < 0) day_begin = 9 * 3600;
  return ShanghaiWallToUnixSec(wall.date, day_begin);
}

int64_t SessionCalendar::SessionOpenUnix(const std::string& product_key,
                                         int64_t last_unix_sec) const {
  const auto* segs = Find(product_key);
  static const std::vector<TradingSegment> kEmpty;
  return SessionOpenUnixFromLast(segs ? *segs : kEmpty, last_unix_sec);
}

}  // namespace qsev
