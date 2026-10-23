# src/config / Quant_Sev/config

## live.env

| 键 | 默认 | 解析后 |
|----|------|--------|
| `DATA_ROOT` | `./data` | 仓库 `data/` |
| `FLOW_PATH` | `./flow` | `flow/{md,td}/` |
| `SESSIONS_URL` | `http://dict.openctp.cn/times` | 交易时段在线源 |

相对路径相对 **仓库根 `C:\Quant_Sev`**（由 `live.env` 位置推算）。见 `plan/layout.md`。

## 交易时段

CTP InstrumentField **无**开盘/收盘钟点。
`--sync-instruments` 会 HTTP 拉取 `SESSIONS_URL`（openctp **/times**），缓存到：

```text
data/live/sessions/latest.json
data/live/sessions/{TradingDay}.json
```

说明：`http://dict.openctp.cn/instruments` 只有合约静态字段（乘数/保证金/到期等），**没有** `TimeBegin`/`TimeEnd`；时段必须用 **/times**。
拉取失败时回退本地 cache。这不是品种宇宙白名单。
