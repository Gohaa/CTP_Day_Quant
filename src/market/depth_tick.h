#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace qsev {

// Full five-level tick (plan step03 / DepthMarketData subset + ids).
struct DepthTick {
  char instrument_id[81]{};
  char exchange_id[9]{};
  char trading_day[9]{};
  char action_day[9]{};
  char update_time[9]{};
  int update_millisec{0};

  double last_price{0};
  double open_price{0};
  double highest_price{0};
  double lowest_price{0};
  double pre_close_price{0};
  double pre_settlement_price{0};
  double upper_limit_price{0};
  double lower_limit_price{0};
  double average_price{0};

  int volume{0};
  double turnover{0};
  double open_interest{0};
  double pre_open_interest{0};

  double bid_price[5]{};
  int bid_volume[5]{};
  double ask_price[5]{};
  int ask_volume[5]{};

  std::string InstrumentId() const { return instrument_id; }
};

inline void CopyCStr(char* dst, size_t n, const char* src) {
  if (!dst || n == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

}  // namespace qsev
