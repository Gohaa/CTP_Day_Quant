#pragma once

#include "market/depth_tick.h"
#include "store/dominant_selector.h"

#include <vector>

namespace qsev {

// Live-trade liquidity vs the rest of the dominant table. No product-name list.
// Day session: session volume must clear vol_cut. High OI alone cannot pass (zombie months).
// Night snapshot volume==0: OI must clear oi_cut (CFFEX still in).
//   vol_cut = max(10000, 10% of median volume among volume>0)
//   oi_cut  = max(20000, 15% of median OI)
// Volatility: session high-low in ticks (CTP Highest/Lowest). SL is 8 ticks — range below
// that (or below 20% of the universe median) is skipped. Wide spread also skipped.

struct LiquidityCuts {
  double vol_cut{0};
  double oi_cut{0};
  int n_entries{0};
  int n_positive_volume{0};
};

LiquidityCuts ComputeLiquidityCuts(const std::vector<DominantEntry>& entries);

bool PassesLiquidity(int volume, double open_interest, const LiquidityCuts& cuts);
bool PassesLiquidity(const DominantEntry& e, const LiquidityCuts& cuts);
bool PassesLiquidity(const DominantEntry& e, const DepthTick& tick, const LiquidityCuts& cuts);

bool HasTwoSidedBook(const DepthTick& tick);

// Session high-low in price ticks. 0 if High/Low missing.
double SessionRangeTicks(const DepthTick& tick, double price_tick);
double SpreadTicks(const DepthTick& tick, double price_tick);
// min_range_ticks typically max(8, 20% of median session range). max_spread default 4.
bool PassesVolatility(const DepthTick& tick, double price_tick, double min_range_ticks,
                      double max_spread_ticks = 4);
// ranges: session range in ticks for names that have data. Empty → 8.
double MinRangeTicksFromUniverse(const std::vector<double>& ranges);

int RunLiquiditySelfTest();

}  // namespace qsev
