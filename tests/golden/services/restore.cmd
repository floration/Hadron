@echo off
rem Hadron services offline recovery, 20261111-101500: Stripped Services
rem Run from WinRE > Troubleshoot > Advanced options > Command Prompt. See README.txt.
rem It writes only Start (and DelayedAutostart where noted) for service keys that already exist.
rem It never creates or deletes a key, never changes a key that is Boot or System start now,
rem and never searches other drives.
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
set /a SYS=0
set /a BAD=0
call :put "DiagTrack" 2 -   & rem Auto
call :put "MapsBroker" 2 1   & rem Auto (delayed)
call :put "Spooler" 2 -   & rem Auto
call :put "CDPUserSvc" 2 0   & rem Auto
echo 3 names can't be typed in this script: see README.txt, section C.
reg unload HKLM\HXOFF
if errorlevel 1 echo WARNING: reg unload HKLM\HXOFF failed. Close regedit, then run: reg unload HKLM\HXOFF
echo Done: %OK% restored, %MISS% not on this Windows, %SYS% now Boot or System start (left as they are), %BAD% failed.
if not "%BAD%"=="0" echo Failed rows: see README.txt, section E.
echo Close this window and choose Continue to start Windows.
exit /b 0

:put
set "N=%~1"
reg query "%K%\%N%" >nul 2>&1
if errorlevel 1 goto :miss
set "CUR="
for /f "tokens=1,3" %%A in ('reg query "%K%\%N%" /v Start 2^>nul') do if /i "%%A"=="Start" set "CUR=%%B"
if "%CUR%"=="0x0" goto :sysnow
if "%CUR%"=="0x1" goto :sysnow
reg add "%K%\%N%" /v Start /t REG_DWORD /d %~2 /f >nul
if errorlevel 1 goto :bad
if "%~3"=="-" goto :putok
reg add "%K%\%N%" /v DelayedAutostart /t REG_DWORD /d %~3 /f >nul
if errorlevel 1 goto :baddelay
:putok
echo restored "%N%" Start=%~2
set /a OK+=1
exit /b 0
:miss
echo skip "%N%": no such key
set /a MISS+=1
exit /b 0
:sysnow
echo skip "%N%": Start is now %CUR%, Boot or System start: left as it is
set /a SYS+=1
exit /b 0
:bad
echo FAILED "%N%": Start was not written
set /a BAD+=1
exit /b 0
:baddelay
echo FAILED "%N%": Start was written, DelayedAutostart was not
set /a BAD+=1
exit /b 0

:nowin
echo No Windows installation at %T% (no %T%\Windows\System32\config\SYSTEM).
echo Run it with the drive that holds Windows, for example: restore.cmd D:
exit /b 1
:noload
echo reg load failed. Either the drive is BitLocker-locked, see README.txt section A,
echo or HKLM\HXOFF is still loaded from an earlier run: run  reg unload HKLM\HXOFF  and try again.
exit /b 1
:nosel
echo Could not read Select\Current from the offline hive. Nothing was changed.
reg unload HKLM\HXOFF
exit /b 1
