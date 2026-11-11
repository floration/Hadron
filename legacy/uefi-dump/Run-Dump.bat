@echo off
rem Runs Dump-UEFI.ps1 as Administrator. Read-only: nothing is written to the BIOS.
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Dump-UEFI.ps1"
pause
