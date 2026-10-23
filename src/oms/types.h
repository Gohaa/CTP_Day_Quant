#pragma once

#include <string>

namespace qsev {

struct OrderDraft {
  std::string instrument_id;
  std::string exchange_id;
  std::string product_key;
  char direction{'0'};  // Buy '0' / Sell '1'
  char offset{'0'};
  double price{0};
  int volume{1};
  std::string order_ref;
};

struct PositionRow {
  std::string instrument_id;
  std::string exchange_id;
  char posi_direction{'2'};  // Long '2' / Short '3'
  char position_date{'1'};   // Today '1' / History '2'
  int position{0};
  int yd_position{0};
  int today_position{0};
};

struct AccountSnapshot {
  double balance{0};
  double available{0};
  double curr_margin{0};
  double frozen_margin{0};
  bool ok{false};
};

struct DepthQuote {
  std::string instrument_id;
  double last{0};
  double bid{0};
  double ask{0};
  double upper{0};
  double lower{0};
  bool ok{false};
};

struct OrderRtn {
  std::string order_ref;
  std::string instrument_id;
  char status{0};
  int volume_traded{0};
  int volume_total{0};
  int err_id{0};
  std::string err_msg;
  bool is_error{false};
};

struct TradeRtn {
  std::string order_ref;
  std::string trade_id;
  std::string instrument_id;
  char direction{0};
  char offset{0};
  double price{0};
  int volume{0};
};

}  // namespace qsev
