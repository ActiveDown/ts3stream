@echo off
setlocal
if not exist "%~dp0start-ts3stream-limited.ps1" (
    echo Extract the entire ZIP first. Keep the CMD and PS1 files together.
    pause
    exit /b 1
)
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0start-ts3stream-limited.ps1" -MaxMbps 4
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
