#include "store/liquidity.h"

#include "logging/logger.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace qsev {
namespace {

constexpr double kAbsMinVolume = 10000;
constexpr double kAbsMinOi = 20000;
constexpr double kVolMedianFrac = 0.10;
constexpr double kOiMedianFrac = 0.15;

double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  if (n % 2 == 1) return v[n / 2];
  return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

LiquidityCuts ComputeLiquidityCuts(const std::vector<DominantEntry>& entries) {
  LiquidityCuts c;
  c.n_entries = static_cast<int>(entries.size());
  std::vector<double> vols;
  std::vector<double> ois;
  vols.reserve(entries.size());
  ois.reserve(entries.size());
  for (const auto& e : entries) {
    ois.push_back(e.dominant_oi);
    if (e.dominant_volume > 0) vols.push_back(static_cast<double>(e.dominant_volume));
  }
  c.n_positive_volume = static_cast<int>(vols.size());
  const double med_vol = Median(std::move(vols));
  const double med_oi = Median(std::move(ois));
  c.vol_cut = kAbsMinVolume;
  c.oi_cut = kAbsMinOi;
  if (c.n_positive_volume >= 5) {
    c.vol_cut = std::max(kAbsMinVolume, kVolMedianFrac * med_vol);
  }
  if (c.n_entries >= 5) {
    c.oi_cut = std::max(kAbsMinOi, kOiMedianFrac * med_oi);
  }
  return c;
}

bool PassesLiquidity(int volume, double open_interest, const LiquidityCuts& cuts) {
  if (cuts.n_entries <= 0) return false;
  const double v = static_cast<double>(volume);
  const double oi = open_interest;
  if (v <= 0) return oi >= cuts.oi_cut;  // night: volume snapshot 0, OI decides
  return v >= cuts.vol_cut;              // day: OI alone cannot pass
}

bool PassesLiquidity(const DominantEntry& e, const LiquidityCuts& cuts) {
  return PassesLiquidity(e.dominant_volume, e.dominant_oi, cuts);
}

bool PassesLiquidity(const DominantEntry& e, const DepthTick& tick, const LiquidityCuts& cuts) {
  const int vol = std::max(e.dominant_volume, tick.volume);
  const double oi = std::max(e.dominant_oi, tick.open_interest);
  return PassesLiquidity(vol, oi, cuts);
}

bool HasTwoSidedBook(const DepthTick& tick) {
  return tick.bid_price[0] > 0 && tick.ask_price[0] > 0 && tick.bid_volume[0] > 0 &&
         tick.ask_volume[0] > 0;
}

double SessionRangeTicks(const DepthTick& tick, double price_tick) {
  if (!(price_tick > 0)) return 0;
  double hi = tick.highest_price;
  double lo = tick.lowest_price;
  if (hi > 0 && lo > 0 && hi >= lo) return (hi - lo) / price_tick;
  if (tick.open_price > 0 && tick.last_price > 0) {
    return std::fabs(tick.last_price - tick.open_price) / price_tick;
  }
  return 0;
}

double SpreadTicks(const DepthTick& tick, double price_tick) {
  if (!(price_tick > 0) || !(tick.bid_price[0] > 0) || !(tick.ask_price[0] > 0)) return 1e9;
  if (tick.ask_price[0] < tick.bid_price[0]) return 1e9;
  return (tick.ask_price[0] - tick.bid_price[0]) / price_tick;
}

double MinRangeTicksFromUniverse(const std::vector<double>& ranges) {
  constexpr double kAbsMin = 8;  // matches 8-tick stop; no room to move below this
  if (ranges.size() < 5) return kAbsMin;
  std::vector<double> v = ranges;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  const double med = (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
  return std::max(kAbsMin, 0.20 * med);
}

bool PassesVolatility(const DepthTick& tick, double price_tick, double min_range_ticks,
                      double max_spread_ticks) {
  if (!(price_tick > 0)) return false;
  if (min_range_ticks < 8) min_range_ticks = 8;
  if (SessionRangeTicks(tick, price_tick) < min_range_ticks) return false;
  if (SpreadTicks(tick, price_tick) > max_spread_ticks) return false;
  return true;
}

int RunLiquiditySelfTest() {
  int fail = 0;
  auto check = [&](bool ok, const char* m) {
    if (!ok) {
      Logger::Error("liquidity", std::string("selftest fail: ") + m);
      ++fail;
    }
  };

  std::vector<DominantEntry> rows;
  for (int i = 0; i < 8; ++i) {
    DominantEntry e;
    e.product_key = "LIQ." + std::to_string(i);
    e.dominant_volume = 100000 + i;
    e.dominant_oi = 200000;
    rows.push_back(e);
  }
  DominantEntry idx;
  idx.product_key = "CFFEX.IF";
  idx.dominant_volume = 0;
  idx.dominant_oi = 150000;
  rows.push_back(idx);
  DominantEntry dead;
  dead.product_key = "DEAD.xx";
  dead.dominant_volume = 0;
  dead.dominant_oi = 0;
  rows.push_back(dead);
  DominantEntry thin;
  thin.product_key = "THIN.yy";
  thin.dominant_volume = 500;
  thin.dominant_oi = 8000;
  rows.push_back(thin);

  const auto cuts = ComputeLiquidityCuts(rows);
  check(cuts.vol_cut >= 10000 && cuts.oi_cut >= 20000, "cuts");
  check(PassesLiquidity(rows[0], cuts), "liquid");
  check(PassesLiquidity(idx, cuts), "index night vol=0");
  check(!PassesLiquidity(dead, cuts), "dead");
  check(!PassesLiquidity(thin, cuts), "thin");
  DominantEntry zombie;
  zombie.product_key = "ZOMBIE.zz";
  zombie.dominant_volume = 200;
  zombie.dominant_oi = 80000;
  check(!PassesLiquidity(zombie, cuts), "high oi low volume");

  DepthTick t{};
  t.volume = 80000;
  t.open_interest = 9000;
  check(PassesLiquidity(thin, t, cuts), "live volume lifts thin");
  t.volume = 10;
  t.open_interest = 10;
  check(!PassesLiquidity(dead, t, cuts), "stale tick still dead");

  DepthTick book{};
  book.bid_price[0] = 1;
  book.ask_price[0] = 2;
  book.bid_volume[0] = 1;
  book.ask_volume[0] = 1;
  check(HasTwoSidedBook(book), "two-sided");
  book.ask_volume[0] = 0;
  check(!HasTwoSidedBook(book), "one-sided");

  DepthTick rng{};
  rng.highest_price = 3100;
  rng.lowest_price = 3000;
  rng.last_price = 3050;
  rng.open_price = 3040;
  rng.bid_price[0] = 3049;
  rng.ask_price[0] = 3050;
  check(SessionRangeTicks(rng, 1) == 100, "range ticks");
  check(PassesVolatility(rng, 1, 8, 4), "vol ok");
  rng.highest_price = 3002;
  rng.lowest_price = 3000;
  check(!PassesVolatility(rng, 1, 8, 4), "range too small");
  rng.highest_price = 3100;
  rng.lowest_price = 3000;
  rng.ask_price[0] = 3060;
  check(!PassesVolatility(rng, 1, 8, 4), "spread too wide");
  check(MinRangeTicksFromUniverse({10, 20, 40, 80, 100, 12, 30, 50}) >= 8, "min range");

  if (fail == 0) Logger::Info("liquidity", "selftest ok");
  return fail;
}

}  // namespace qsev
