# Quant_Sev Web (migration preview)

**Architecture:** Quant_Sev core is **C++** (CTP live md/td, bars, backtest, AI, risk/OMS).
Python here is **not** the trading/backtest engine.

web/server.py is a **temporary** static-file + parquet preview server so HTML layouts
can be checked against data/klines. Production API will be served by C++.

## Run

双击 `C:\Quant_Sev\Quant_Sev_Live.exe` 会一并启动本服务，浏览器打开 **http://127.0.0.1:8765/**。关掉 Live.exe 网页服务一起停。

单独预览（不跑实盘）仍可用：

```powershell
cd C:\Quant_Sev
python web/server.py
```

## Pages

| URL | Page |
|-----|------|
| / | Quote |
| /analyze | Analyze (offline stub) |
| /equity | Equity |
| /backtest | Backtest charts |

## Python elsewhere (tools only)

- 极智导出导入（`tools/ep`），不用天勤
- Web scrapers / exchange notices / warehouse receipts
- Never: live CTP md, live orders, main backtest loop

