#pragma once

#include "util/time_shanghai.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace qsev {

struct TradingSegment {
  int segment_no{0};
  std::string time_begin;  // HH:MM:SS
  std::string time_end;    // HH:MM:SS (may be earlier than begin if overnight)
};

struct SessionBound {
  int64_t open_unix_ns{0};
  int64_t close_unix_sec{0};
  bool ok{false};
};

// Exchange trading-session calendar.
// CTP InstrumentField has NO open/close clock times.
// Source of truth: live HTTP http://dict.openctp.cn/times (NOT /instruments — that API has no
// TimeBegin/TimeEnd). Cached under data/live/sessions/. Not a product-universe whitelist.
class SessionCalendar {
 public:
  static SessionCalendar LoadFromJson(const std::string& json_text, const std::string& source_tag);
  static SessionCalendar LoadFromFile(const std::string& path);
  // Fetch openctp times API; on success optionally write cache_path.
  static SessionCalendar FetchOpenCtpTimes(const std::string& url, const std::string& cache_path = {});

  bool Empty() const { return by_product_.empty(); }
  size_t Size() const { return by_product_.size(); }
  const std::string& Source() const { return source_; }

  // product_key e.g. SHFE.rb / CFFEX.IF / CZCE.SA
  const std::vector<TradingSegment>* Find(const std::string& product_key) const;

  bool WriteCache(const std::string& path) const;

  // True if Shanghai wall time (unix ns) falls inside any segment of product.
  bool IsInSession(const std::string& product_key, int64_t unix_ns) const;

  // Trading-day session open/close for daily bar (night segments attributed to TradingDay).
  // trading_day = YYYYMMDD from CTP tick. Weekend nights: prefer SessionOpenUnixFromLast.
  SessionBound DayBounds(const std::string& product_key, const std::string& trading_day) const;

  // Current trading-session open from last observed Shanghai unix (not a hardcoded 21:00).
  // Night product: previous (or same) evening night-open → next day close.
  // Day-only: that calendar day's first daytime open (09:00 / 09:30).
  int64_t SessionOpenUnix(const std::string& product_key, int64_t last_unix_sec) const;

 private:
  std::string source_;
  std::unordered_map<std::string, std::vector<TradingSegment>> by_product_;
};

// Shared by quote / chip: session open unix from last print + product segments (data/live/sessions).
int64_t SessionOpenUnixFromLast(const std::vector<TradingSegment>& segs, int64_t last_unix_sec);

}  // namespace qsev
