#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace qsev {

// One L1 print. Direction is NEVER 内外盘 — compare consecutive ticks.
struct ChipTick {
  double last{0};
  double last_qty{0};
  double volume{0};  // cumulative TotalQty / CTP Volume
  double bid{0};
  double ask{0};
  double bid_qty{0};
  double ask_qty{0};
  int64_t unix_sec{0};  // Shanghai wall from csv datetime; 0 if unknown
};

struct ChipBar {
  double open{0};
  double high{0};
  double low{0};
  double close{0};
  double volume{0};
  double open_interest{0};
  int64_t unix_sec{0};
};

struct ChipAggressor {
  double buy{0};   // 主动买（外盘方向）
  double sell{0};  // 主动卖（内盘方向）
};

struct ChipLevel {
  double price{0};
  double buy{0};   // 多头 / 主动买
  double sell{0};  // 空头 / 主动卖
};

struct ChipProfile {
  std::string mode;  // tick | kline
  std::string symbol;
  std::string instrument_id;
  std::string source;
  double price_tick{0};
  double last{0};
  double avg_cost{0};
  double poc{0};
  double profit_pct{0};
  double conc90_low{0};
  double conc90_high{0};
  int tick_count{0};
  int bar_count{0};
  // Current product session open (openctp /times via data/live/sessions), not previous day.
  int64_t session_open_unix{0};
  std::string session_open;
  double tick_high{0};  // session last-price high (tick ladder top)
  double tick_low{0};   // session last-price low (tick ladder bottom)
  std::vector<ChipLevel> levels;
};

enum class ChipDist { Triangle, Uniform };

struct ChipCliOptions {
  bool enabled{false};
  bool selftest{false};
  std::string mode{"tick"};
  std::string symbol;
  std::string day;
  std::string out_path;
  std::string ticks_csv;
  std::string bars_csv;
  std::string dist{"triangle"};
  double decay{1.0};
  int lookback{0};  // 0 = all
  bool session_window{false};  // kline: keep current session only (never previous)
};

// Consecutive L1: 现价/现手/成交量 vs 上一买价卖价买量卖量. Skip quote-only (qty=0).
ChipAggressor TickAggressor(const ChipTick* prev, const ChipTick& cur);

ChipProfile BuildTickChip(const std::vector<ChipTick>& ticks, double price_tick);

// 同花顺移动筹码: 按 data/live 的 PriceTick 分档；每档同时累计多头/空头.
// 阳线投放记多头，阴线记空头；换手*衰减搬移历史成本。期货换手 = volume/OI，无 OI 则 volume/累计成交.
ChipProfile BuildKlineChip(const std::vector<ChipBar>& bars, double price_tick, ChipDist dist,
                           double decay);

std::string ChipProfileToJson(const ChipProfile& p);
std::string ChipProfileToCsv(const ChipProfile& p);

int RunChipSelfTest();
int RunChip(const ChipCliOptions& opt, const std::string& data_root);
bool TryLoadSessionTickChip(const std::string& data_root, const std::string& symbol, ChipProfile* out,
                            const std::string& instrument_id = {});

}  // namespace qsev
