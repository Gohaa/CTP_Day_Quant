# Quant_Sev — 期货量化交易软件

C++ 主框架（CTP 实盘行情/交易、K线、回测、AI 门禁）；Python 仅做极智导出导入、爬虫/仓单、只读 AI 侧车（**不用天勤**）。

- 本机路径：[`plan/layout.md`](plan/layout.md)（仓库 `C:\Quant_Sev`，极智 `C:\Quantv9.5`）
- 路线与数据契约：[ `plan/consider.md`](plan/consider.md)
- 分步清单：[`plan/step00-index.md`](plan/step00-index.md)
- 数据双通道（极智历史 / CTP 实盘）：[`plan/data-pipeline.md`](plan/data-pipeline.md)
- 开盘前命令：[`plan/ep-bootstrap.md`](plan/ep-bootstrap.md)
- 旧 AI 规格：[`plan/ai-from-lib.md`](plan/ai-from-lib.md)

## 语言与职责边界

| 层 | 技术 | 职责 |
|------|------|------|
| **主框架** | **C++** | CTP 实盘行情/交易、Tick→K线、回测、OMS/风控、AI 发单门禁、对外 HTTP/IPC |
| **辅助** | **Python** | 极智历史/当日导出导入、网页爬虫、交易所/仓单；step15 只读 AI 侧车（规格在 `lib/`） |
| **UI** | 静态 HTML | `web/static/`；最终接 C++ API（当前 `web/server.py` 仅迁移期读盘预览） |

## 本仓库目录（自包含，不再嵌套 FuturesAI）

| 路径 | 含义 |
|------|------|
| `plan/` | 实施路线 |
| `lib/` | 旧 Python/AI 归档（对照，非生产入口） |
| `config/` | `live.env`（gitignore）密钥与路径 |
| `data/` | **数据根**（`DATA_ROOT=./data`）；含 `klines/`、`ticks/{TradingDay}/`、`live/` |
| `data/klines/` | K 线 Parquet：极智导入 + CTP 盘中 merge（§0.2） |
| `data/ticks/` | **仅 CTP** 实盘 Tick（按交易日） |
| `data/live/ep_raw/` | **仅极智** 导出的原始 M1+Tick |
| `data/live/` | dominant / instruments / sessions / logs / guards… |
| `flow/` | CTP `.con` 流水（`flow/md`、`flow/td`） |
| `src/` | C++ 源码 |
| `web/` | 静态页 + 迁移期预览；生产 API 归 C++ |
| `img/` | 布局参考图 |
| `third_party/ctp/` | CTP 头文件与库 |
| `tools/ep/` | 极智 `VV_SevBootstrap` 源与同步脚本 |

## 模块

每个 `src/*` 目录均有独立 `README.md`。

| 模块 | 职责 |
|------|------|
| `src/config` | 实盘配置加载（密码脱敏）；路径相对仓库根解析 |
| `src/logging` | 按交易日滚动日志 |
| `src/md` | CTP MdApi 行情 |
| `src/td` | CTP TraderApi 交易/查询 |
| `src/market` | TickBus / BarBuilder / Parquet 落盘 |
| `src/store` | InstrumentStore / 主力表（废除死表） |
| `src/oms` | 报撤单与订单状态机 |
| `src/risk` | 追踪止损 / EOD |
| `src/strategy` | 金字塔等策略 |
| `src/ai` | AI 门禁（C++）；Prompt 规格在 `lib/` |
| `src/util` | 公共工具 |

## 构建（Windows）

迁址后必须在本目录**重新** cmake（旧 `build/CMakeCache.txt` 指向 `C:/dev/FuturesAI/Quant_Sev` 无效）：

```powershell
cd C:\Quant_Sev
cmake -B build -DQUANT_SEV_WITH_CTP=OFF
cmake --build build --config Release
.\build\Release\quant_sev.exe --help
```

放入期货公司生产版 `.lib/.dll` 到 `third_party/ctp/lib` 与 `bin` 后：

```powershell
cmake -B build -DQUANT_SEV_WITH_CTP=ON
cmake --build build --config Release
```

默认 `enable_trading=false`。密钥只放本机 `config/live.env`（已 gitignore）。

## 常用 CLI

```powershell
.\build\Release\quant_sev.exe --config .\config\live.env --select-dominant --run-seconds 20
.\build\Release\quant_sev.exe --config .\config\live.env --sync-instruments
.\build\Release\quant_sev.exe --config .\config\live.env --live-ticks --dominant-only
# 默认一直跑到 Ctrl+C；抽样可加 --run-seconds 60
.\build\Release\quant_sev.exe --config .\config\live.env --option-screen
# 临期期权只读筛；盘中补盘口再加 --option-screen-live --run-seconds 15
```

## Web 预览（迁移期，非交易主路径）

```powershell
cd C:\Quant_Sev
.\tools\ep\sync_to_jizhi.ps1    # 拷到 C:\Quantv9.5\...\用户策略
# 在极智里跑 VV_SevBootstrap（补历史）
.\tools\bootstrap_today.ps1    # 导入 parquet，不写 data/ticks
.\build\Release\quant_sev.exe --config .\config\live.env --live-ticks --dominant-only
python web/server.py
```

打开 http://127.0.0.1:8765/ 。行情页为 ChartWindow 风格 Canvas（分时/1m/5m/Tick）。

| URL | 页面 |
|-----|------|
| http://127.0.0.1:8765/ | Quote |
| http://127.0.0.1:8765/analyze | Analyze（离线桩） |
| http://127.0.0.1:8765/equity | Equity |
| http://127.0.0.1:8765/backtest | Backtest |

生产目标：C++ 提供同名 API，Python 服务撤出交易/回测主路径。

## 删除旧 Python 的规则

仅当某能力已在本仓库 C++ 路径验收通过后，再删除 `lib/` 中对应文件，并在 `lib/README.md` 迁移表勾选。未完成前保留 `lib/` 作对照（含 AI Prompt）。
