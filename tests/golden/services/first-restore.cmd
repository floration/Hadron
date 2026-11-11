@echo off
rem Hadron services offline recovery, first: start types as recorded when Hadron first ran
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
call :put "dmwappushservice" 3 -   & rem Manual
call :put "MapsBroker" 2 1   & rem Auto (delayed)
call :put "lfsvc" 3 -   & rem Manual
call :put "RetailDemo" 3 -   & rem Manual
call :put "WMPNetworkSvc" 3 -   & rem Manual
call :put "MessagingService" 3 -   & rem Manual
call :put "SmsRouter" 3 -   & rem Manual
call :put "SEMgrSvc" 3 -   & rem Manual
call :put "PhoneSvc" 3 -   & rem Manual
call :put "WalletService" 3 -   & rem Manual
call :put "icssvc" 3 -   & rem Manual
call :put "wisvc" 3 -   & rem Manual
call :put "BcastDVRUserService" 3 -   & rem Manual
call :put "autotimesvc" 3 -   & rem Manual
call :put "XblAuthManager" 3 -   & rem Manual
call :put "XblGameSave" 3 -   & rem Manual
call :put "XboxNetApiSvc" 3 -   & rem Manual
call :put "Spooler" 2 -   & rem Auto
call :put "PrintNotify" 3 -   & rem Manual
call :put "Fax" 3 -   & rem Manual
call :put "bthserv" 3 -   & rem Manual
call :put "BTAGService" 3 -   & rem Manual
call :put "BthAvctpSvc" 3 -   & rem Manual
call :put "BluetoothUserService" 3 -   & rem Manual
call :put "XboxGipSvc" 3 -   & rem Manual
call :put "WSearch" 2 1   & rem Auto (delayed)
call :put "CDPSvc" 2 1   & rem Auto (delayed)
call :put "CDPUserSvc" 2 -   & rem Auto
call :put "OneSyncSvc" 2 1   & rem Auto (delayed)
call :put "PimIndexMaintenanceSvc" 3 -   & rem Manual
call :put "SharedAccess" 3 -   & rem Manual
call :put "RmSvc" 3 -   & rem Manual
call :put "PcaSvc" 2 -   & rem Auto
call :put "SensorService" 3 -   & rem Manual
call :put "SensrSvc" 3 -   & rem Manual
call :put "SensorDataService" 3 -   & rem Manual
call :put "SSDPSRV" 3 -   & rem Manual
call :put "upnphost" 3 -   & rem Manual
call :put "lltdsvc" 3 -   & rem Manual
call :put "QWAVE" 3 -   & rem Manual
call :put "stisvc" 3 -   & rem Manual
call :put "WiaRpc" 3 -   & rem Manual
call :put "FrameServer" 3 -   & rem Manual
call :put "ShellHWDetection" 2 -   & rem Auto
call :put "ScDeviceEnum" 3 -   & rem Manual
call :put "SCardSvr" 3 -   & rem Manual
call :put "SCPolicySvc" 3 -   & rem Manual
call :put "vmickvpexchange" 3 -   & rem Manual
call :put "vmicguestinterface" 3 -   & rem Manual
call :put "vmicshutdown" 3 -   & rem Manual
call :put "vmicheartbeat" 3 -   & rem Manual
call :put "vmicvmsession" 3 -   & rem Manual
call :put "vmicrdv" 3 -   & rem Manual
call :put "vmictimesync" 3 -   & rem Manual
call :put "vmicvss" 3 -   & rem Manual
call :put "HvHost" 3 -   & rem Manual
call :put "RasAuto" 3 -   & rem Manual
call :put "RemoteAccess" 4 -   & rem Disabled
call :put "SysMain" 2 -   & rem Auto
call :put "DPS" 2 1   & rem Auto (delayed)
call :put "WdiSystemHost" 3 -   & rem Manual
call :put "WdiServiceHost" 3 -   & rem Manual
call :put "WerSvc" 3 -   & rem Manual
call :put "DiagSvc" 3 -   & rem Manual
call :put "DusmSvc" 2 -   & rem Auto
call :put "UnistoreSvc" 3 -   & rem Manual
call :put "UserDataSvc" 3 -   & rem Manual
call :put "NcbService" 3 -   & rem Manual
call :put "dot3svc" 3 -   & rem Manual
call :put "cphs" 3 -   & rem Manual
call :put "TabletInputService" 3 -   & rem Manual
call :put "WpnService" 2 -   & rem Auto
call :put "wlidsvc" 3 -   & rem Manual
call :put "LicenseManager" 3 -   & rem Manual
call :put "CaptureService" 3 -   & rem Manual
call :put "NgcSvc" 3 -   & rem Manual
call :put "defragsvc" 3 -   & rem Manual
call :put "WlanSvc" 2 -   & rem Auto
call :put "wuauserv" 3 -   & rem Manual
call :put "UsoSvc" 2 1   & rem Auto (delayed)
call :put "WaaSMedicSvc" 3 -   & rem Manual
call :put "InstallService" 3 -   & rem Manual
call :put "Schedule" 2 -   & rem Auto
call :put "NVDisplay.ContainerLocalSystem" 2 -   & rem Auto
call :put "ExampleUpdater" 2 1   & rem Auto (delayed)
call :put "luafv" 2 -   & rem Auto
call :put "igfxn" 3 -   & rem Manual
call :put "IntcDAud" 3 -   & rem Manual
call :put "HyperVideo" 3 -   & rem Manual
call :put "hyperkbd" 3 -   & rem Manual
call :put "BTHUSB" 3 -   & rem Manual
call :put "Serial" 3 -   & rem Manual
call :put "nvlddmkm" 3 -   & rem Manual
call :put "xboxgip" 3 -   & rem Manual
call :put "hvservice" 3 -   & rem Manual
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
echo A BitLocker-locked drive shows no Windows folder: unlock it first, see README.txt section A.
exit /b 1
:noload
echo reg load failed. Either the drive is BitLocker-locked, see README.txt section A,
echo or HKLM\HXOFF is still loaded from an earlier run: run  reg unload HKLM\HXOFF  and try again.
exit /b 1
:nosel
echo Could not read Select\Current from the offline hive. Nothing was changed.
reg unload HKLM\HXOFF
exit /b 1
