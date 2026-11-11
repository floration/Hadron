Hadron services recovery first
What this restores: start types as recorded when Hadron first ran
Only start types are restored. Service state can't be restored offline.

A. If Windows doesn't start (WinRE)
1. Windows opens the recovery environment by itself after failed starts. Choose
   Troubleshoot > Advanced options > Command Prompt.
   If it doesn't open, start the PC from Windows installation media (a USB stick), choose
   your language, then Repair your computer > Troubleshoot > Advanced options > Command Prompt.
2. If the Windows drive uses BitLocker, unlock it first (you need the recovery key):
     manage-bde -unlock C: -RecoveryPassword <your-48-digit-key>
   (use the drive letter WinRE shows for Windows; it is often not C:).
3. Find the drive that holds Windows. Either try:  dir C:\Windows  then  dir D:\Windows
   or run:  bcdedit | find "osdevice"
4. Run this. Replace the last D: with the Windows drive, and the first D: with the drive
   that holds this folder (the same drive, unless Hadron's data folder is on another one):
     D:\ProgramData\Hadron\services\recovery\first\restore.cmd D:
   The script refuses if D:\Windows\System32\config\SYSTEM is missing, loads that hive,
   reads Select\Current, writes the values below for keys that exist, and unloads it.
5. Close the window and choose Continue.

Which folder to use
Each apply has its own folder in the recovery folder, named by the UTC time it started
(yyyyMMdd-HHmmss), so the names sort oldest first. "first" holds the start types from when
Hadron first ran on this PC. List them with:
     dir D:\ProgramData\Hadron\services\recovery
Run the newest folder first. If Windows still doesn't start, run the next older one, and so on:
each puts back the start types from before its own apply. "first" goes back to how Hadron
first saw this PC.

If you copied this folder to a USB stick, run it from there and give the Windows drive:
     E:\restore.cmd D:
WinRE has no Explorer to open a .zip (reasoning): use the plain folder. The zip
     D:\ProgramData\Hadron\services\recovery-first.zip
holds the same files, for copying.

B. If Windows starts in Safe Mode
Open an administrator Command Prompt and run:
     C:\ProgramData\Hadron\services\recovery\first\restore-online.cmd     (C: is where Windows normally runs)
It checks for admin rights with fltmc and writes the same values to the running registry.
Or start Hadron in Safe Mode and use Default: as first seen.

C. Names the scripts can't carry
None.

D. Values (96 items)
  DiagTrack  Start=2 (Auto)
  dmwappushservice  Start=3 (Manual)  (needed TrustedInstaller: see E)
  MapsBroker  Start=2 (Auto)  DelayedAutostart=1
  lfsvc  Start=3 (Manual)
  RetailDemo  Start=3 (Manual)
  WMPNetworkSvc  Start=3 (Manual)
  MessagingService  Start=3 (Manual)
  SmsRouter  Start=3 (Manual)
  SEMgrSvc  Start=3 (Manual)
  PhoneSvc  Start=3 (Manual)
  WalletService  Start=3 (Manual)
  icssvc  Start=3 (Manual)
  wisvc  Start=3 (Manual)
  BcastDVRUserService  Start=3 (Manual)
  autotimesvc  Start=3 (Manual)
  XblAuthManager  Start=3 (Manual)
  XblGameSave  Start=3 (Manual)
  XboxNetApiSvc  Start=3 (Manual)
  Spooler  Start=2 (Auto)
  PrintNotify  Start=3 (Manual)
  Fax  Start=3 (Manual)
  bthserv  Start=3 (Manual)
  BTAGService  Start=3 (Manual)
  BthAvctpSvc  Start=3 (Manual)
  BluetoothUserService  Start=3 (Manual)
  XboxGipSvc  Start=3 (Manual)
  WSearch  Start=2 (Auto)  DelayedAutostart=1
  CDPSvc  Start=2 (Auto)  DelayedAutostart=1
  CDPUserSvc  Start=2 (Auto)
  OneSyncSvc  Start=2 (Auto)  DelayedAutostart=1
  PimIndexMaintenanceSvc  Start=3 (Manual)
  SharedAccess  Start=3 (Manual)
  RmSvc  Start=3 (Manual)
  PcaSvc  Start=2 (Auto)
  SensorService  Start=3 (Manual)
  SensrSvc  Start=3 (Manual)
  SensorDataService  Start=3 (Manual)
  SSDPSRV  Start=3 (Manual)
  upnphost  Start=3 (Manual)
  lltdsvc  Start=3 (Manual)
  QWAVE  Start=3 (Manual)
  stisvc  Start=3 (Manual)
  WiaRpc  Start=3 (Manual)
  FrameServer  Start=3 (Manual)
  ShellHWDetection  Start=2 (Auto)
  ScDeviceEnum  Start=3 (Manual)
  SCardSvr  Start=3 (Manual)
  SCPolicySvc  Start=3 (Manual)
  vmickvpexchange  Start=3 (Manual)
  vmicguestinterface  Start=3 (Manual)
  vmicshutdown  Start=3 (Manual)
  vmicheartbeat  Start=3 (Manual)
  vmicvmsession  Start=3 (Manual)
  vmicrdv  Start=3 (Manual)
  vmictimesync  Start=3 (Manual)
  vmicvss  Start=3 (Manual)
  HvHost  Start=3 (Manual)
  RasAuto  Start=3 (Manual)
  RemoteAccess  Start=4 (Disabled)
  SysMain  Start=2 (Auto)
  DPS  Start=2 (Auto)  DelayedAutostart=1
  WdiSystemHost  Start=3 (Manual)
  WdiServiceHost  Start=3 (Manual)
  WerSvc  Start=3 (Manual)
  DiagSvc  Start=3 (Manual)
  DusmSvc  Start=2 (Auto)
  UnistoreSvc  Start=3 (Manual)
  UserDataSvc  Start=3 (Manual)
  NcbService  Start=3 (Manual)
  dot3svc  Start=3 (Manual)
  cphs  Start=3 (Manual)
  TabletInputService  Start=3 (Manual)
  WpnService  Start=2 (Auto)
  wlidsvc  Start=3 (Manual)
  LicenseManager  Start=3 (Manual)
  CaptureService  Start=3 (Manual)
  NgcSvc  Start=3 (Manual)
  defragsvc  Start=3 (Manual)
  WlanSvc  Start=2 (Auto)
  wuauserv  Start=3 (Manual)
  UsoSvc  Start=2 (Auto)  DelayedAutostart=1
  WaaSMedicSvc  Start=3 (Manual)  (needed TrustedInstaller: see E)
  InstallService  Start=3 (Manual)
  Schedule  Start=2 (Auto)
  NVDisplay.ContainerLocalSystem  Start=2 (Auto)
  ExampleUpdater  Start=2 (Auto)  DelayedAutostart=1
  luafv  Start=2 (Auto)
  igfxn  Start=3 (Manual)
  IntcDAud  Start=3 (Manual)
  HyperVideo  Start=3 (Manual)
  hyperkbd  Start=3 (Manual)
  BTHUSB  Start=3 (Manual)
  Serial  Start=3 (Manual)
  nvlddmkm  Start=3 (Manual)
  xboxgip  Start=3 (Manual)
  hvservice  Start=3 (Manual)

E. Rows that may print FAILED
Some service keys accept writes only from TrustedInstaller, not from administrators or SYSTEM.
Hadron writes those while impersonating TrustedInstaller. In WinRE and in Safe Mode the scripts
run as SYSTEM or as an administrator, so the reg add for such a key may fail and print FAILED.
That changes nothing else. Restore those rows with Hadron's Default: as first seen,
with the TrustedInstaller toggle on, once Windows starts.
Rows here that needed TrustedInstaller: 2 (marked in D).

F. Not in these scripts (55 items)
Hadron never changes these, so the scripts never write them either: items at Boot or System
start, storage and boot-path drivers, items Hadron never touches (core services Windows needs
to start, sign in or reach the network, filter drivers, and services whose failure restarts
Windows), items it never puts in a preset (Defender, TrustedInstaller, Volume Shadow Copy and
others) and launch-protected services. Windows sets the storage and boot-path drivers per PC:
writing an old value back after a disk or controller change can stop Windows from starting.
  TrustedInstaller
  VSS
  swprv
  WinDefend
  WdNisSvc
  SecurityHealthService
  wscsvc
  BrokerInfrastructure
  RpcSs
  RpcEptMapper
  DcomLaunch
  PlugPlay
  Audiosrv
  AudioEndpointBuilder
  Dhcp
  Dnscache
  nsi
  EventLog
  Winmgmt
  CryptSvc
  BFE
  mpssvc
  ProfSvc
  ACPI
  pci
  pdc
  stornvme
  storahci
  disk
  partmgr
  volmgr
  volsnap
  fvevol
  iorate
  rdyboost
  EhStorClass
  Ntfs
  FltMgr
  Wof
  FileInfo
  CldFlt
  WdFilter
  WdBoot
  WdNisDrv
  Wdf01000
  CNG
  tcpip
  kbdclass
  mouclass
  USBXHCI
  intelppm
  mssmbios
  Beep
  ExampleAudioFlt
  ExampleCriticalDrv
