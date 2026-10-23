# Sync 极智导出策略 into 极智量化 user-strategy folder.
# Default: C:\Quantv9.5\Quant\Strategy\用户策略
# Override: $env:JIZHI_STRATEGY_DIR  or  $env:JIZHI_ROOT
# 本机当前极智在 C:\Quant_EP 时：
#   $env:JIZHI_ROOT = "C:\Quant_EP"; .\tools\ep\sync_to_jizhi.ps1
#
# 实际在跑的是 VV_ExportHistory.py（不是 VV_SevBootstrap）。

$ErrorActionPreference = "Stop"
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent  # Quant_Sev repo root

$DestDir = $env:JIZHI_STRATEGY_DIR
if (-not $DestDir) {
  $jizhiRoot = if ($env:JIZHI_ROOT) { $env:JIZHI_ROOT } else { "C:\Quantv9.5" }
  $DestDir = Join-Path $jizhiRoot "Quant\Strategy\用户策略"
}
New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

function Copy-Strategy($name) {
  $src = Join-Path $PSScriptRoot $name
  if (-not (Test-Path $src)) { throw "missing $src" }
  $dest = Join-Path $DestDir $name
  Copy-Item -Force $src $dest
  Write-Host "OK -> $dest"
}

Copy-Strategy "VV_ExportHistory.py"
if (Test-Path (Join-Path $PSScriptRoot "VV_SevBootstrap.py")) {
  Copy-Strategy "VV_SevBootstrap.py"
}

$Hint = Join-Path $DestDir "VV_ExportHistory_README.txt"
@"
Quant_Sev 导出策略（极智里实际运行这一份）:
  $DestDir\VV_ExportHistory.py

1. FILL_TICK=True（当前）：在极智中【停止】再【运行】VV_ExportHistory，只导当日 Tick
   补 CTP 缺口。输出: $Root\data\live\ep_raw\YYYYMMDD\tick\*.csv
   区域 = 上一交易日 20:00 ~ now（不要用 WriteCSV 的固定根数/'A'）。
   不要拷进 data\ticks。Live 约 60s 会把 ep_raw tick 拼进分时。
   补完后把 FILL_TICK 改回 False，再跑本脚本同步。
2. FILL_TICK=False（日常）：每个交易日一次 M1/M5/D。
   输出: $Root\data\live\ep_raw\YYYYMMDD\m1|m5|d\*.csv
3. 然后本机:
   cd $Root
   python tools/import_ep_to_store.py
   （Tick 不进 parquet；分时靠 QuoteTape = ep_raw tick + CTP ticks）

实盘才跑:
   .\build\Release\quant_sev.exe --config config\live.env --live-ticks --dominant-only
   Tick → data/ticks/{TradingDay}/ ；K 线只 merge 盘中新棒，不覆盖极智历史。

QUANT_SEV_ROOT 默认 C:\Quant_Sev。
VV_SevBootstrap 未在使用；不要把它当正在跑的策略。
不要把 VV_SignalTrader / VV_RiskTrail 接到 Quant_Sev。
"@ | Set-Content -Encoding UTF8 $Hint

Write-Host "Hint -> $Hint"
