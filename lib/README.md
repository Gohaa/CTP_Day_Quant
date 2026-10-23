# lib/ — 旧 Python / AI 归档

本目录是迁址前系统的对照归档，位于仓库根 `C:\Quant_Sev\lib`。  
生产主路径是本仓库 **C++**（`src/`）。规格如何迁入见 [`plan/ai-from-lib.md`](../plan/ai-from-lib.md)。

> **不要**再把本目录当作生产入口。不要 `cd lib && python main.py` 接实盘。

## 还保留的用途

| 用途 | 怎么用 |
|------|--------|
| AI Prompt / 模型表 | `strategies/prompts.py`、`config.py` `MODEL_DEFINITIONS`、`analyzers.py` 信号 JSON |
| step10 读盘验收 | `backtest/engine.py::load_klines`，数据根是仓库 `data/klines` |
| 风控开关对照 | `config.py` 里 `ENABLE_TRAILING_STOP` 等 → 将来 `RiskGuard` |

```powershell
cd C:\Quant_Sev
python -c "from lib.backtest.engine import load_klines"  # 视 PYTHONPATH 而定
# 或:
cd C:\Quant_Sev\lib
python -c "from backtest.engine import load_klines; print('ok')"
```

K 线真源：**`C:\Quant_Sev\data\klines`**。从 `lib/` 跑脚本请显式 `--data-dir ../data`。

**缺口：** `strategies/__init__.py` 引用 `BrooksStrategy`（`price_action.py`），该文件当前不在归档里。AI 实盘前必须找回或按 `base.py` + `prompts.py` 重建。

天勤下载已废弃：`download_klines.py` 只打印改走极智的提示。

## 迁移对照表（完成后删除对应文件）

| 旧文件 / 目录 | Quant_Sev 对应 | 状态 |
|---------------|----------------|------|
| `data_feed.py` | `src/md` + `src/market` | 未完成 |
| `trader.py` | `src/td` + `src/oms` | 未完成 |
| `config.py` | `src/config` + 将来 AI 段 | 骨架中 |
| `analyzers.py` / `strategies/` | `src/ai` + 只读侧车 | 未完成；缺 `price_action.py` |
| `index/equity/backtest/quote` HTML | [`web/`](../web/) | migrated |
| `download_klines.py` | 极智 `tools/ep` | 已废弃 |
| `engine._TICK_VALUE` / `router._BUILTIN_SYMBOLS` | **废除** → CTP `InstrumentStore` | 见 step05 |

## 删除规则

1. C++ 中等价功能通过 `plan/step*.md` 验收。  
2. 在本表将状态改为「已完成」。  
3. 删除对应旧文件，避免双轨混乱。  
4. 全部核心交易路径迁完后，可删除整个 `lib/`（Prompt 文本应先拷到 `src/ai` 或 `data/ai/prompts/`）。
