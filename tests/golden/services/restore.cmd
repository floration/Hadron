@echo off
rem Hadron services offline recovery, 20261111-101500: Stripped Services
rem Run from WinRE > Troubleshoot > Advanced options > Command Prompt. See README.txt.
rem It writes only Start (and DelayedAutostart where noted) for service keys that already exist.
rem It never creates or deletes a key and never searches other drives.
setlocal EnableExtensions
set "T=%~1"
if "%T%"=="" set "T=%~d0"
if not exist "%T%\Windows\System32\config\SYSTEM" goto :nowin
echo Windows to repair: %T%\Windows
reg load HKLM\HXOFF "%T%\Windows\System32\config\SYSTEM"
if errorlevel 1 goto :noload
set "CS="
for /f "tokens=3" %%V in ('reg query HKLM\HXOFF\Select /v Current 2^>nul ^| find "Current"') do set /a CS=%%V
if not defined CS goto :nosel
set "CSN=00%CS%"
set "K=HKLM\HXOFF\ControlSet%CSN:~-3%\Services"
echo Control set: ControlSet%CSN:~-3%
set /a OK=0
set /a MISS=0
set /a BAD=0
call :put "DiagTrack" 2 -   & rem Auto
call :put "MapsBroker" 2 1   & rem Auto (delayed)
call :put "Spooler" 2 -   & rem Auto
call :put "CDPUserSvc" 2 0   & rem Auto
echo 2 name(s) can't be typed in this script: see README.txt, section C.
reg unload HKLM\HXOFF
echo Done: %OK% restored, %MISS% not on this Windows, %BAD% failed.
echo Close this window and choose Continue to start Windows.
exit /b 0

:put
reg query "%K%\%~1" >nul 2>&1
if errorlevel 1 (echo skip %~1: no such key & set /a MISS+=1 & exit /b 0)
reg add "%K%\%~1" /v Start /t REG_DWORD /d %~2 /f >nul
if errorlevel 1 (echo FAILED %~1 & set /a BAD+=1 & exit /b 0)
if not "%~3"=="-" reg add "%K%\%~1" /v DelayedAutostart /t REG_DWORD /d %~3 /f >nul
echo restored %~1 Start=%~2
set /a OK+=1
exit /b 0

:nowin
echo No Windows installation at %T% (no %T%\Windows\System32\config\SYSTEM).
echo Run it with the drive that holds Windows, for example: restore.cmd D:
exit /b 1
:noload
echo reg load failed: is the drive BitLocker-locked? See README.txt, section A.
exit /b 1
:nosel
echo Could not read Select\Current from the offline hive. Nothing was changed.
reg unload HKLM\HXOFF
exit /b 1
