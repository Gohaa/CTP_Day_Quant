#pragma once

#include "strategy/intent.h"
#include "strategy/modules/modules.h"

#include <string>

namespace qsev {
namespace strategy {

// AI 开仓：两条路径（异动+筹码+盘口）或（趋势+支撑阻力）→ 侧车/模型 → 结构化 OpenIntent。
// C++ 只执行结构化字段（step15）；不执行模型散文；不下单。

struct AiOpenSnapshot {
  ModuleResult burst;
  ModuleResult trend;
  ModuleResult chip;
  ModuleResult book;
  ModuleResult sr;
};

AiOpenSnapshot BuildAiOpenSnapshot(const Context& ctx);
OpenIntent AiOpenIntent(const Context& ctx);
OpenIntent AiOpenIntent(const Context& ctx, const std::string& data_root);

}  // namespace strategy
}  // namespace qsev
