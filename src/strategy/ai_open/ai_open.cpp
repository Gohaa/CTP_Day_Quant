#include "strategy/ai_open/ai_open.h"
#include "strategy/ai_open/signal.h"

#include <string>

namespace qsev {
namespace strategy {

AiOpenSnapshot BuildAiOpenSnapshot(const Context& ctx) {
  AiOpenSnapshot s;
  s.burst = DetectBurst(ctx);
  s.trend = JudgeTrend(ctx);
  s.chip = DiscoverChip(ctx);
  s.book = ReadBook(ctx);
  s.sr = FindSupportResistance(ctx);
  return s;
}

OpenIntent AiOpenIntent(const Context& ctx) { return AiOpenIntent(ctx, {}); }

OpenIntent AiOpenIntent(const Context& ctx, const std::string& data_root) {
  OpenIntent out;
  out.strategy_id = "ai_open";
  const auto snap = BuildAiOpenSnapshot(ctx);
  const auto pack = ScoreModules(ctx);
  if (!data_root.empty()) {
    const auto sig = LoadTradeSignal(data_root, ctx.product_key);
    if (sig.ok) {
      out = IntentFromSignal(sig);
      out.reason += " gate=" + VoteGate(pack) + " burst=" + std::to_string(snap.burst.score) +
                    " trend=" + std::to_string(snap.trend.score) + " sum=" + std::to_string(pack.sum);
      return out;
    }
  }
  out.side = VoteSide(pack);
  out.reason = "sidecar idle; gate=" + VoteGate(pack) + " burst=" + std::to_string(snap.burst.score) +
               " trend=" + std::to_string(snap.trend.score) + " chip=" + std::to_string(snap.chip.score) +
               " book=" + std::to_string(snap.book.score) + " sr=" + std::to_string(snap.sr.score) +
               " sum=" + std::to_string(pack.sum);
  return out;
}

}  // namespace strategy
}  // namespace qsev
