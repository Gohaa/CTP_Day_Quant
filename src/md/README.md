# src/md

CTP **MdApi** 行情网关。

| 模块 | 职责 |
|------|------|
| `MdGateway` | 连接 / 登录 / Subscribe / UnSubscribe（含分批） |
| `SubscriptionManager` | 目标集合增量同步（step07 主力切换） |

```powershell
.\build\Release\quant_sev.exe --config .\config\live.env --live-ticks --run-seconds 30
```
