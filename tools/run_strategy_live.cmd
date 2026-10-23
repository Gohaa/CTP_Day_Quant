@echo off
cd /d "%~dp0.."
if exist "Quant_Sev_Live.exe" (
  start "" "%~dp0..\Quant_Sev_Live.exe" %*
) else (
  echo missing C:\Quant_Sev\Quant_Sev_Live.exe — rebuild first
  exit /b 1
)
