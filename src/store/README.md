# src/store

| 模块 | 职责 |
|------|------|
| `InstrumentStore` | Td `ReqQryInstrument` → 内存；主力落盘见下 |
| 派生 API | `ProductKey` / `TickSize` / `Multiplier` / `TickValue` / `ListProductKeys` |
| `DominantSelector` | step06 选主力 → `Quant_Sev/data/live/dominant/{day}.json` |
| `DominantTable` | 读 dominant JSON → 订阅 / sync 目标列表 |
| `LiquidityCuts` | 实盘开仓剔除成交量/持仓左尾品种（相对当日主力表，无品种名单） |
| `SessionCalendar` | openctp `/times` 交易时段（非 CTP 字段） |
| `OptionScreener` | step14 临期期权（DTE&lt;30）+ HV-BS 时间价值低估；默认不发单 |

**主力基本信息：** `Quant_Sev/data/live/instruments/dominant/{day}.json` + `latest.json`
（`DATA_ROOT=./data`）

```powershell
.\build\Release\quant_sev.exe --config .\config\live.env --select-dominant --run-seconds 20
.\build\Release\quant_sev.exe --config .\config\live.env --sync-instruments
.\build\Release\quant_sev.exe --option-screen-selftest
.\build\Release\quant_sev.exe --config .\config\live.env --option-screen
# 盘中补真实买卖价：再加 --option-screen-live --run-seconds 15
```
