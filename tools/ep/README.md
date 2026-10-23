# tools/ep — 极智启动补全

| 文件 | 说明 |
|------|------|
| `VV_ExportHistory.py` | **实际在跑**：M1/M5/D 日常导出；`FILL_TICK=True` 时只导当日 Tick 补缺口 |
| `VV_SevBootstrap.py` | 旧引导脚本，未在使用 |
| `sync_to_jizhi.ps1` | 复制策略到 `C:\Quantv9.5\Quant\Strategy\用户策略` |

Tick 只写 `data/live/ep_raw/{日}/tick/`，不要拷进 `data/ticks/`。QuoteTape 会把极智 Tick 拼进缺的分钟。

见 [`plan/data-pipeline.md`](../../plan/data-pipeline.md)、[`plan/ep-bootstrap.md`](../../plan/ep-bootstrap.md)。
