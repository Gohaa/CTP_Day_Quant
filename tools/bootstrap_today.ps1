# Import ?? ep_raw into data/klines only (never copy into data/ticks).
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root

$Day = $args[0]
if (-not $Day) {
  $days = Get-ChildItem -Directory "data\live\ep_raw" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d{8}$' } |
    Sort-Object Name -Descending
  if (-not $days) {
    Write-Host "No data/live/ep_raw/YYYYMMDD yet."
    Write-Host "1) .\tools\ep\sync_to_jizhi.ps1"
    Write-Host "2) In ¼«ÖÇ run VV_SevBootstrap once"
    Write-Host "3) Re-run this script"
    exit 1
  }
  $Day = $days[0].Name
}

$Done = "data\live\ep_raw\$Day\_export_done.json"
if (-not (Test-Path $Done)) {
  Write-Host "WARN: missing $Done ¡ª has ¼«ÖÇ finished VV_SevBootstrap?"
} else {
  Write-Host "export done: $Done"
}

python tools\import_ep_to_store.py --day $Day
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "OK. Next:"
Write-Host "  python web\server.py"
Write-Host "  .\build\Release\quant_sev.exe --config .\config\live.env --live-ticks --dominant-only"
