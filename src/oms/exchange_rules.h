#pragma once

#include <string>

namespace qsev {

// consider.md §3.5 — only SHFE/INE split today vs yesterday.
inline bool ExchangeUsesCloseToday(const std::string& exchange_id) {
  return exchange_id == "SHFE" || exchange_id == "INE";
}

// CTP CombOffsetFlag[0]: Open '0' / Close '1' / CloseToday '3' / CloseYesterday '4'
inline char OffsetOpen() { return '0'; }

inline char OffsetClose(const std::string& exchange_id, bool close_today) {
  if (ExchangeUsesCloseToday(exchange_id)) return close_today ? '3' : '1';
  return '1';
}

inline const char* OffsetName(char o) {
  switch (o) {
    case '0':
      return "Open";
    case '1':
      return "Close";
    case '3':
      return "CloseToday";
    case '4':
      return "CloseYesterday";
    default:
      return "?";
  }
}

inline bool IsCloseOffset(char o) { return o == '1' || o == '3' || o == '4'; }

}  // namespace qsev
