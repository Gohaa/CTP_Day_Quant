# src/market

| 文件 | 职责 |
|------|------|
| `tick_bus.*` / `tick_snapshot_book.*` | Tick 总线与快照 |
| `bar_builder.*` | Tick → **1m / 5m / 15m / 1d**（日 K 按品种会话时段） |
| `parquet_bar_store.*` | 写入 `Quant_Sev/data/klines`（step09，§0.2） |
| `tick_day_store.*` | 写入 `Quant_Sev/data/ticks/{TradingDay}/{InstrumentID}.csv`（五档，UTF-8） |
| `depth_tick.*` | CTP 五档快照结构 |

`--live-ticks`：合成 K 线落盘 + 默认按交易日写 tick CSV（`--no-save-ticks` 关闭）。
`.parquet` 为二进制，预览用 `python tools/preview_parquet.py <path>`。
