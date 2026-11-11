#!/usr/bin/env python3
"""tests/fixtures/services/make_services_fixture.py — writes the Services page fixture.

    python3 -I tests/fixtures/services/make_services_fixture.py <out-dir>

Writes <out-dir>/{snapshot.txt, hw.txt, first.txt} and <out-dir>/interrupted/{snapshot.txt, hw.txt, first.txt,
undo-20261110-213012.log}, plus three regression fixtures from the Analyze round (docs/pipeline/svcdrv/4-analyze.md):
  delayedlinger/  Spooler is Manual with a lingering DelayedAutostart=1, first.txt says plain Auto (A7);
  forged/         a finished undo log with valid CRCs that would set stornvme/disk/RpcSs to Disabled (A4);
  multi/          an interrupted apply and an interrupted undo of it (the undo must not hide the apply).
The committed copies in tests/fixtures/services/ are this script's output; CI regenerates them into a temp folder and
compares (`cmp`), so edit this script, not the text files.

The machine is a small, hand-written picture of a Windows 10 22H2 (19045) desktop: every KB entry, the floor items the
guards protect, a boot disk chain, class and device filters, per-user templates with instances and a few third-party
items. Start types follow common Windows 10 defaults but are NOT a measured stock table (the KB carries no stock values
for that reason). Synthetic, on purpose:
  - dmwappushservice carries `X acl=ti` (an admin write is refused), so the TrustedInstaller marking shows in a preset;
  - ExampleAudioFlt (a device LowerFilters entry) and ExampleCriticalDrv (ErrorControl Critical) exercise two guards.
Formats: docs/design/services.md (snapshot, hw dump, undo log). The writer mirrors store.cpp exactly, so the selftest's
parse -> write round trip reproduces snapshot.txt byte for byte.
"""
import os
import sys
import zlib

SNAP_HDR = 'HXSVC\t1\tsnapshot'
HW_HDR = 'HXSVC\t1\thw'
UNDO_HDR = 'HXSVC\t1\tundo'


def esc(s):
    return s.replace('\\', '\\\\').replace('\t', ' ').replace('\r', '').replace('\n', '\\n')


SVCHOST = r'%SystemRoot%\system32\svchost.exe -k '
# name, display, type, cfg, trigger, account, image, deps, desc, company  (state etc. derived)
OWN, SHARE, UTPL, UINST, KDRV, FSDRV = 0x10, 0x20, 0x60, 0xE0, 0x1, 0x2

SERVICES = [
    # ---- KB: bloat
    ('DiagTrack', 'Connected User Experiences and Telemetry', OWN, '2/-', 0, 'LocalSystem', SVCHOST + 'utcsvc -p', 'RpcSs', 'The Connected User Experiences and Telemetry service enables features that support in-application and connected user experiences.'),
    ('dmwappushservice', 'Device Management Wireless Application Protocol (WAP) Push message Routing Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'rpcss', 'Routes Wireless Application Protocol (WAP) Push messages received by the device.'),
    ('MapsBroker', 'Downloaded Maps Manager', SHARE, '2/1', 0, 'NT AUTHORITY\\NetworkService', SVCHOST + 'NetworkService -p', 'rpcss', 'Windows service for application access to downloaded maps.'),
    ('lfsvc', 'Geolocation Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'rpcss', 'This service monitors the current location of the system and manages geofences.'),
    ('RetailDemo', 'Retail Demo Service', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'rdxgroup', '', 'The Retail Demo service controls device activity while the device is in retail demo mode.'),
    ('WMPNetworkSvc', 'Windows Media Player Network Sharing Service', OWN, '3/-', 0, 'NT AUTHORITY\\NetworkService', r'"%ProgramFiles%\Windows Media Player\wmpnetwk.exe"', 'http;WSearch', 'Shares Windows Media Player libraries to other networked players and media devices using Universal Plug and Play'),
    ('MessagingService', 'MessagingService', UTPL, '3/-', 1, '', SVCHOST + 'UnistackSvcGroup', '', 'Service supporting text messaging and related functionality.'),
    ('SmsRouter', 'Microsoft Windows SMS Router Service.', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalServiceNetworkRestricted -p', 'RpcSs', 'Routes messages based on rules to appropriate clients.'),
    ('SEMgrSvc', 'Payments and NFC/SE Manager', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', 'RpcSs', 'Manages payments and Near Field Communication (NFC) based secure elements.'),
    ('PhoneSvc', 'Phone Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', '', 'Manages the telephony state on the device'),
    ('WalletService', 'WalletService', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'appmodel -p', 'RpcSs', 'Hosts objects used by clients of the wallet'),
    ('icssvc', 'Windows Mobile Hotspot Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalSystemNetworkRestricted -p', 'rpcss', 'Provides the ability to share a cellular data connection with another device.'),
    ('wisvc', 'Windows Insider Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'rpcss', 'Provides infrastructure support for the Windows Insider Program.'),
    ('BcastDVRUserService', 'GameDVR and Broadcast User Service', UTPL, '3/-', 0, '', SVCHOST + 'BcastDVRUserService', '', 'This user service is used for Game Recordings and Live Broadcasts'),
    ('autotimesvc', 'Cellular Time', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'autoTimeSvc', 'rpcss', 'This service sets time based on NITZ messages from a Mobile Network'),
    ('XblAuthManager', 'Xbox Live Auth Manager', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', '', 'Provides authentication and authorization services for interacting with Xbox Live.'),
    ('XblGameSave', 'Xbox Live Game Save', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'UserManager;XblAuthManager', 'This service syncs save data for Xbox Live save enabled games.'),
    ('XboxNetApiSvc', 'Xbox Live Networking Service', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'BFE;mpssvc;IKEEXT;KeyIso', 'This service supports the Windows.Networking.XboxLive application programming interface.'),
    # ---- KB: stripped
    ('Spooler', 'Print Spooler', OWN, '2/-', 0, 'LocalSystem', r'%SystemRoot%\System32\spoolsv.exe', 'RPCSS;http', 'This service spools print jobs and handles interaction with the printer.'),
    ('PrintNotify', 'Printer Extensions and Notifications', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'print', 'rpcss', 'This service opens custom printer dialog boxes and handles notifications from a remote print server or a printer.'),
    ('Fax', 'Fax', OWN, '3/-', 0, 'NT AUTHORITY\\NetworkService', r'%systemroot%\system32\fxssvc.exe', 'TapiSrv;RpcSs;Spooler', 'Enables you to send and receive faxes.'),
    ('bthserv', 'Bluetooth Support Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', 'rpcss', 'The Bluetooth service supports discovery and association of remote Bluetooth devices.'),
    ('BTAGService', 'Bluetooth Audio Gateway Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted', 'rpcss', 'Service supporting the audio gateway role of the Bluetooth Handsfree Profile.'),
    ('BthAvctpSvc', 'AVCTP service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', '', 'This is Audio Video Control Transport Protocol service'),
    ('BluetoothUserService', 'Bluetooth User Support Service', UTPL, '3/-', 1, '', SVCHOST + 'BthAppGroup -p', '', 'The Bluetooth user service supports proper functionality of Bluetooth features relevant to each user session.'),
    ('XboxGipSvc', 'Xbox Accessory Management Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', '', 'This service manages connected Xbox Accessories.'),
    ('WSearch', 'Windows Search', OWN, '2/1', 0, 'LocalSystem', r'%systemroot%\system32\SearchIndexer.exe /Embedding', 'RPCSS;BrokerInfrastructure', 'Provides content indexing, property caching, and search results for files, e-mail, and other content.'),
    ('CDPSvc', 'Connected Devices Platform Service', SHARE, '2/1', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', 'ncbservice;RpcSS;Tcpip', 'This service is used for Connected Devices Platform scenarios'),
    ('CDPUserSvc', 'Connected Devices Platform User Service', UTPL, '2/-', 0, '', SVCHOST + 'UnistackSvcGroup', '', 'This user service is used for Connected Devices Platform scenarios'),
    ('OneSyncSvc', 'Sync Host', UTPL, '2/1', 0, '', SVCHOST + 'UnistackSvcGroup', '', 'This service synchronizes mail, contacts, calendar and various other user data.'),
    ('PimIndexMaintenanceSvc', 'Contact Data', UTPL, '3/-', 0, '', SVCHOST + 'UnistackSvcGroup', 'UnistoreSvc', 'Indexes contact data for fast contact searching.'),
    ('SharedAccess', 'Internet Connection Sharing (ICS)', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'Netman;WinMgmt', 'Provides network address translation, addressing, name resolution and/or intrusion prevention services for a home or small office network.'),
    ('RmSvc', 'Radio Management Service', SHARE, '3/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted', 'RpcSs', 'Radio Management and Airplane Mode Service'),
    ('PcaSvc', 'Program Compatibility Assistant Service', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'This service provides support for the Program Compatibility Assistant (PCA).'),
    ('SensorService', 'Sensor Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'A service for sensors that manages different sensors\' functionality.'),
    ('SensrSvc', 'Sensor Monitoring Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceAndNoImpersonation -p', '', 'Monitors various sensors in order to expose data and adapt to system and user state.'),
    ('SensorDataService', 'Sensor Data Service', OWN, '3/-', 1, 'LocalSystem', r'%SystemRoot%\System32\SensorDataService.exe', '', 'Delivers data from a variety of sensors'),
    ('SSDPSRV', 'SSDP Discovery', SHARE, '3/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceAndNoImpersonation -p', 'HTTP;NSI', 'Discovers networked devices and services that use the SSDP discovery protocol, such as UPnP devices.'),
    ('upnphost', 'UPnP Device Host', SHARE, '3/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceAndNoImpersonation -p', 'SSDPSRV;HTTP', 'Allows UPnP devices to be hosted on this computer.'),
    ('lltdsvc', 'Link-Layer Topology Discovery Mapper', SHARE, '3/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', 'rpcss;tcpip', 'Creates a Network Map, consisting of PC and device topology (connectivity) information.'),
    ('QWAVE', 'Quality Windows Audio Video Experience', SHARE, '3/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceAndNoImpersonation -p', 'psched;rpcss', 'Quality Windows Audio Video Experience (qWave) is a networking platform for Audio Video (AV) streaming applications on IP home networks.'),
    ('stisvc', 'Windows Image Acquisition (WIA)', OWN, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'imgsvc', 'RpcSs', 'Provides image acquisition services for scanners and cameras'),
    ('WiaRpc', 'Still Image Acquisition Events', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', 'RpcSs', 'Launches applications associated with still image acquisition events.'),
    ('FrameServer', 'Windows Camera Frame Server', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'Camera', '', 'Enables multiple clients to access video frames from camera devices.'),
    ('ShellHWDetection', 'Shell Hardware Detection', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'RpcSs', 'Provides notifications for AutoPlay hardware events.'),
    ('ScDeviceEnum', 'Smart Card Device Enumeration Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted', '', 'Creates software device nodes for all smart card readers accessible to a given session.'),
    ('SCardSvr', 'Smart Card', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceAndNoImpersonation', '', 'Manages access to smart cards read by this computer.'),
    ('SCPolicySvc', 'Smart Card Removal Policy', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'netsvcs', 'RpcSs', 'Allows the system to be configured to lock the user desktop upon smart card removal.'),
    ('vmickvpexchange', 'Hyper-V Data Exchange Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Provides a mechanism to exchange data between the virtual machine and the operating system running on the physical computer.'),
    ('vmicguestinterface', 'Hyper-V Guest Service Interface', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Provides an interface for the Hyper-V host to interact with specific services running inside the virtual machine.'),
    ('vmicshutdown', 'Hyper-V Guest Shutdown Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Provides a mechanism to shut down the operating system of this virtual machine from the management interfaces on the physical computer.'),
    ('vmicheartbeat', 'Hyper-V Heartbeat Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'ICService -p', '', 'Monitors the state of this virtual machine by reporting a heartbeat at regular intervals.'),
    ('vmicvmsession', 'Hyper-V PowerShell Direct Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Provides a mechanism to manage virtual machine with PowerShell via VM session without a virtual network.'),
    ('vmicrdv', 'Hyper-V Remote Desktop Virtualization Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'ICService -p', '', 'Provides a platform for communication between the virtual machine and the operating system running on the physical computer.'),
    ('vmictimesync', 'Hyper-V Time Synchronization Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', 'VmGid', 'Synchronizes the system time of this virtual machine with the system time of the physical computer.'),
    ('vmicvss', 'Hyper-V Volume Shadow Copy Requestor', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Coordinates the communications that are required to use Volume Shadow Copy Service to back up applications and data on this virtual machine.'),
    ('HvHost', 'HV Host Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Provides an interface for the Hyper-V hypervisor to provide per-partition performance counters to the host operating system.'),
    ('RasAuto', 'Remote Access Auto Connection Manager', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'RasMan;Tapisrv', 'Creates a connection to a remote network whenever a program references a remote DNS or NetBIOS name or address.'),
    ('RemoteAccess', 'Routing and Remote Access', SHARE, '4/-', 0, 'LocalSystem', SVCHOST + 'netsvcs', 'RpcSS;Bfe;RasMan;Http', 'Offers routing services to businesses in local area and wide area network environments.'),
    # ---- KB: minimum (contested)
    ('SysMain', 'SysMain', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', 'rpcss', 'Maintains and improves system performance over time.'),
    ('DPS', 'Diagnostic Policy Service', SHARE, '2/1', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNoNetwork -p', '', 'The Diagnostic Policy Service enables problem detection, troubleshooting and resolution for Windows components.'),
    ('WdiSystemHost', 'Diagnostic System Host', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', 'RpcSs', 'The Diagnostic System Host is used by the Diagnostic Policy Service to host diagnostics that need to run in a Local System context.'),
    ('WdiServiceHost', 'Diagnostic Service Host', SHARE, '3/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', 'RpcSs', 'The Diagnostic Service Host is used by the Diagnostic Policy Service to host diagnostics that need to run in a Local Service context.'),
    ('WerSvc', 'Windows Error Reporting Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'WerSvcGroup', '', 'Allows errors to be reported when programs stop working or responding and allows existing solutions to be delivered.'),
    ('DiagSvc', 'Diagnostic Execution Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'diagnostics', 'RpcSs', 'Executes diagnostic actions for troubleshooting support'),
    ('DusmSvc', 'Data Usage', SHARE, '2/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', 'RpcSS', 'Network data usage, data limit, restrict background data, metered networks.'),
    ('UnistoreSvc', 'User Data Storage', UTPL, '3/-', 0, '', SVCHOST + 'UnistackSvcGroup', '', 'Handles storage of structured user data, including contact info, calendars, messages, and other content.'),
    ('UserDataSvc', 'User Data Access', UTPL, '3/-', 0, '', SVCHOST + 'UnistackSvcGroup', 'UnistoreSvc', 'Provides apps access to structured user data, including contact info, calendars, messages, and other content.'),
    ('NcbService', 'Network Connection Broker', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', 'RpcSs', 'Brokers connections that allow Windows Store Apps to receive notifications from the internet.'),
    ('dot3svc', 'Wired AutoConfig', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', 'Ndisuio;RpcSs;Eaphost', 'The Wired AutoConfig (DOT3SVC) service is responsible for performing IEEE 802.1X authentication on Ethernet interfaces.'),
    ('cphs', 'Intel(R) Content Protection HECI Service', OWN, '3/-', 0, 'LocalSystem', r'C:\Windows\System32\DriverStore\FileRepository\iigd_dch.inf_amd64_0123\IntelCpHeciSvc.exe', '', 'Intel(R) Content Protection HECI Service - enables communication with the Content Protection FW'),
    # ---- KB: explanation only / never in presets
    ('TabletInputService', 'Touch Keyboard and Handwriting Panel Service', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Enables Touch Keyboard and Handwriting Panel pen and ink functionality'),
    ('WpnService', 'Windows Push Notifications System Service', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'rpcss', 'This service runs in session 0 and hosts the notification platform and connection provider.'),
    ('wlidsvc', 'Microsoft Account Sign-in Assistant', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'RpcSs', 'Enables user sign-in through Microsoft account identity services.'),
    ('LicenseManager', 'Windows License Manager Service', SHARE, '3/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalService -p', 'RpcSs', 'Provides infrastructure support for the Microsoft Store.'),
    ('CaptureService', 'CaptureService', UTPL, '3/-', 0, '', SVCHOST + 'LocalService -p', '', 'Enables optional screen capture functionality for applications that call the Windows.Graphics.Capture API.'),
    ('NgcSvc', 'Microsoft Passport', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Provides process isolation for cryptographic keys used to authenticate to a user\'s associated identity providers.'),
    ('defragsvc', 'Optimize drives', OWN, '3/-', 0, 'localSystem', SVCHOST + 'defragsvc', 'RPCSS', 'Helps the computer run more efficiently by optimizing files on storage drives.'),
    ('WlanSvc', 'WLAN AutoConfig', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', 'nativewifip;RpcSs;Ndisuio;wcmsvc', 'The WLANSVC service provides the logic required to configure, discover, connect to, and disconnect from a wireless local area network (WLAN).'),
    ('wuauserv', 'Windows Update', SHARE, '3/-', 1, 'LocalSystem', SVCHOST + 'netsvcs -p', 'rpcss', 'Enables the detection, download, and installation of updates for Windows and other programs.'),
    ('UsoSvc', 'Update Orchestrator Service', SHARE, '2/1', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'rpcss', 'Manages Windows Updates.'),
    ('WaaSMedicSvc', 'Windows Update Medic Service', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'wusvcs -p', '', 'Enables remediation and protection of Windows Update components.'),
    ('InstallService', 'Microsoft Store Install Service', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', '', 'Provides infrastructure support for the Microsoft Store.'),
    ('TrustedInstaller', 'Windows Modules Installer', OWN, '3/-', 0, 'localSystem', r'%SystemRoot%\servicing\TrustedInstaller.exe', '', 'Enables installation, modification, and removal of Windows updates and optional components.'),
    ('VSS', 'Volume Shadow Copy', OWN, '3/-', 0, 'LocalSystem', r'%systemroot%\system32\vssvc.exe', 'RPCSS', 'Manages and implements Volume Shadow Copies used for backup and other purposes.'),
    ('swprv', 'Microsoft Software Shadow Copy Provider', OWN, '3/-', 0, 'LocalSystem', SVCHOST + 'swprv', 'RPCSS', 'Manages software-based volume shadow copies taken by the Volume Shadow Copy service.'),
    ('WinDefend', 'Microsoft Defender Antivirus Service', OWN, '2/-', 0, 'LocalSystem', r'"C:\ProgramData\Microsoft\Windows Defender\platform\4.18.25010.11-0\MsMpEng.exe"', 'RpcSs', 'Helps protect users from malware and other potentially unwanted software'),
    ('WdNisSvc', 'Microsoft Defender Antivirus Network Inspection Service', OWN, '3/-', 0, 'NT AUTHORITY\\LocalService', r'"C:\ProgramData\Microsoft\Windows Defender\platform\4.18.25010.11-0\NisSrv.exe"', 'RpcSs;WdNisDrv', 'Helps guard against intrusion attempts targeting known and newly discovered vulnerabilities in network protocols'),
    ('SecurityHealthService', 'Windows Security Service', OWN, '3/-', 0, 'LocalSystem', r'%systemroot%\system32\SecurityHealthService.exe', 'RpcSs', 'Windows Security Service handles unified device protection and health information'),
    ('wscsvc', 'Security Center', SHARE, '2/1', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', 'RpcSs', 'The WSCSVC (Windows Security Center) service monitors and reports security health settings on the computer.'),
    ('BrokerInfrastructure', 'Background Tasks Infrastructure Service', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'DcomLaunch -p', 'RpcEptMapper;DcomLaunch;RpcSs', 'Windows infrastructure service that controls which background tasks can run on the system.'),
    # Disabled by an earlier tweak: the Windows-stock golden shows its published stock note (kb-audit svcdrv-kb M1).
    ('TimeBrokerSvc', 'Time Broker', SHARE, '4/-', 1, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', 'RpcSs', 'Coordinates execution of background work for WinRT application.'),
    # ---- floor (no KB entry needed: the guards protect them)
    ('RpcSs', 'Remote Procedure Call (RPC)', SHARE, '2/-', 0, 'NT AUTHORITY\\NetworkService', SVCHOST + 'rpcss -p', 'RpcEptMapper;DcomLaunch', 'The RPCSS service is the Service Control Manager for COM and DCOM servers.'),
    ('RpcEptMapper', 'RPC Endpoint Mapper', SHARE, '2/-', 0, 'NT AUTHORITY\\NetworkService', SVCHOST + 'RPCSS -p', '', 'Resolves RPC interfaces identifiers to transport endpoints.'),
    ('DcomLaunch', 'DCOM Server Process Launcher', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'DcomLaunch -p', '', 'The DCOMLAUNCH service launches COM and DCOM servers in response to object activation requests.'),
    ('PlugPlay', 'Plug and Play', SHARE, '3/-', 0, 'LocalSystem', SVCHOST + 'DcomLaunch -p', '', 'Enables a computer to recognize and adapt to hardware changes with little or no user input.'),
    ('Audiosrv', 'Windows Audio', OWN, '2/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', 'AudioEndpointBuilder;RpcSs', 'Manages audio for Windows-based programs.'),
    ('AudioEndpointBuilder', 'Windows Audio Endpoint Builder', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'LocalSystemNetworkRestricted -p', '', 'Manages audio devices for the Windows Audio service.'),
    ('Dhcp', 'DHCP Client', SHARE, '2/-', 0, 'NT Authority\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', 'NSI;Afd', 'Registers and updates IP addresses and DNS records for this computer.'),
    ('Dnscache', 'DNS Client', SHARE, '2/-', 1, 'NT AUTHORITY\\NetworkService', SVCHOST + 'NetworkService -p', 'nsi', 'The DNS Client service (dnscache) caches Domain Name System (DNS) names and registers the full computer name for this computer.'),
    ('nsi', 'Network Store Interface Service', SHARE, '2/-', 0, 'NT Authority\\LocalService', SVCHOST + 'LocalService -p', 'rpcss;nsiproxy', 'This service delivers network notifications (e.g. interface addition/deleting etc) to user mode clients.'),
    ('EventLog', 'Windows Event Log', SHARE, '2/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNetworkRestricted -p', '', 'This service manages events and event logs.'),
    ('Winmgmt', 'Windows Management Instrumentation', SHARE, '2/-', 0, 'localSystem', SVCHOST + 'netsvcs -p', 'RPCSS', 'Provides a common interface and object model to access management information about operating system, devices, applications and services.'),
    ('CryptSvc', 'Cryptographic Services', SHARE, '2/-', 1, 'NT Authority\\NetworkService', SVCHOST + 'NetworkService -p', 'RpcSs', 'Provides key management and catalog services.'),
    ('BFE', 'Base Filtering Engine', SHARE, '2/-', 0, 'NT AUTHORITY\\LocalService', SVCHOST + 'LocalServiceNoNetworkFirewall -p', 'RpcSs', 'The Base Filtering Engine (BFE) is a service that manages firewall and Internet Protocol security (IPsec) policies.'),
    ('mpssvc', 'Windows Defender Firewall', SHARE, '2/-', 0, 'NT Authority\\LocalService', SVCHOST + 'LocalServiceNoNetworkFirewall -p', 'mpsdrv;bfe', 'Windows Defender Firewall helps protect your computer by preventing unauthorized users from gaining access to your computer.'),
    ('ProfSvc', 'User Profile Service', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'RpcSs', 'This service is responsible for loading and unloading user profiles.'),
    ('Schedule', 'Task Scheduler', SHARE, '2/-', 0, 'LocalSystem', SVCHOST + 'netsvcs -p', 'RPCSS;SystemEventsBroker', 'Enables a user to configure and schedule automated tasks on this computer.'),
    # ---- third-party (no KB entry: never in a preset)
    ('NVDisplay.ContainerLocalSystem', 'NVIDIA Display Container LS', OWN, '2/-', 0, 'LocalSystem', r'"C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_0123\Display.NvContainer\NVDisplay.Container.exe" -s NVDisplay.ContainerLocalSystem', '', 'Container service for NVIDIA root features'),
    ('ExampleUpdater', 'Example Updater Service', OWN, '2/1', 0, 'LocalSystem', r'"C:\Program Files\Example\Updater\ExampleUpdater.exe" /svc', '', 'Keeps Example software up to date.'),
]

# name, display, type, cfg, group, image, errctl, company
DRIVERS = [
    ('ACPI', 'Microsoft ACPI Driver', KDRV, '0/-', 'Core', r'System32\drivers\ACPI.sys', 3),
    ('pci', 'PCI Bus Driver', KDRV, '0/-', 'Boot Bus Extender', r'System32\drivers\pci.sys', 3),
    ('pdc', 'pdc', KDRV, '0/-', 'Boot Bus Extender', r'system32\drivers\pdc.sys', 3),
    ('stornvme', 'Microsoft Standard NVM Express Driver', KDRV, '0/-', 'SCSI Miniport', r'System32\drivers\stornvme.sys', 1),
    ('storahci', 'Microsoft Standard SATA AHCI Driver', KDRV, '0/-', 'SCSI Miniport', r'System32\drivers\storahci.sys', 3),
    ('disk', 'Disk Driver', KDRV, '0/-', 'SCSI Class', r'System32\drivers\disk.sys', 1),
    ('partmgr', 'Partition driver', KDRV, '0/-', 'System Bus Extender', r'System32\drivers\partmgr.sys', 3),
    ('volmgr', 'Volume Manager Driver', KDRV, '0/-', 'System Bus Extender', r'System32\drivers\volmgr.sys', 3),
    ('volsnap', 'Volume Shadow Copy driver', KDRV, '0/-', 'Filter', r'System32\drivers\volsnap.sys', 3),
    ('fvevol', 'BitLocker Drive Encryption Filter Driver', KDRV, '0/-', 'Filter', r'System32\drivers\fvevol.sys', 3),
    ('iorate', 'Disk I/O Rate Filter Driver', KDRV, '0/-', 'Filter', r'system32\drivers\iorate.sys', 1),
    ('rdyboost', 'ReadyBoost', KDRV, '0/-', 'PnP Filter', r'System32\drivers\rdyboost.sys', 1),
    ('EhStorClass', 'Enhanced Storage Filter Driver', KDRV, '0/-', 'PnP Filter', r'System32\drivers\EhStorClass.sys', 1),
    ('Ntfs', 'Ntfs', FSDRV, '3/-', 'Boot File System', '', 1),
    ('FltMgr', 'FltMgr', FSDRV, '0/-', 'FSFilter Infrastructure', r'system32\drivers\fltmgr.sys', 3),
    ('Wof', 'Windows Overlay File System Filter Driver', FSDRV, '0/-', 'FSFilter Compression', '', 1),
    ('FileInfo', 'File Information FS MiniFilter', FSDRV, '0/-', 'FSFilter Bottom', r'System32\drivers\fileinfo.sys', 1),
    ('CldFlt', 'Windows Cloud Files Filter Driver', FSDRV, '2/-', 'FSFilter HSM', r'system32\drivers\cldflt.sys', 1),
    ('luafv', 'UAC File Virtualization', FSDRV, '2/-', 'FSFilter Virtualization', r'\SystemRoot\system32\drivers\luafv.sys', 1),
    ('WdFilter', 'Microsoft Defender Antivirus Mini-Filter Driver', FSDRV, '0/-', 'FSFilter Anti-Virus', r'system32\drivers\wd\WdFilter.sys', 1),
    ('WdBoot', 'Microsoft Defender Antivirus Boot Driver', KDRV, '0/-', 'Early-Launch', r'system32\drivers\wd\WdBoot.sys', 1),
    ('WdNisDrv', 'Microsoft Defender Antivirus Network Inspection System Driver', KDRV, '3/-', 'NDIS', r'system32\drivers\wd\WdNisDrv.sys', 1),
    ('Wdf01000', 'Kernel Mode Driver Frameworks service', KDRV, '0/-', 'WdfLoadGroup', r'system32\drivers\Wdf01000.sys', 1),
    ('CNG', 'CNG', KDRV, '0/-', 'Core', r'System32\Drivers\cng.sys', 1),
    ('tcpip', 'TCP/IP Protocol Driver', KDRV, '0/-', 'PNP_TDI', r'System32\drivers\tcpip.sys', 1),
    ('kbdclass', 'Keyboard Class Driver', KDRV, '3/-', 'Keyboard Class', r'System32\drivers\kbdclass.sys', 1),
    ('mouclass', 'Mouse Class Driver', KDRV, '3/-', 'Pointer Class', r'System32\drivers\mouclass.sys', 1),
    ('USBXHCI', 'USB xHCI Compliant Host Controller', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\USBXHCI.SYS', 1),
    ('intelppm', 'Intel Processor Driver', KDRV, '3/-', 'Extended Base', r'\SystemRoot\System32\drivers\intelppm.sys', 1),
    ('mssmbios', 'Microsoft System Management BIOS Driver', KDRV, '1/-', 'System Bus Extender', r'System32\drivers\mssmbios.sys', 1),
    ('Beep', 'Beep', KDRV, '1/-', 'Base', '', 1),
    ('igfxn', 'igfxn', KDRV, '3/-', '', r'\SystemRoot\System32\DriverStore\FileRepository\iigd_dch.inf_amd64_0123\igdkmdn64.sys', 1),
    ('IntcDAud', 'Intel(R) Display Audio', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\IntcDAud.sys', 1),
    ('HyperVideo', 'HyperVideo', KDRV, '3/-', 'Video', r'\SystemRoot\System32\drivers\HyperVideo.sys', 1),
    ('hyperkbd', 'hyperkbd', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\hyperkbd.sys', 1),
    ('BTHUSB', 'Bluetooth Radio USB Driver', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\BTHUSB.sys', 1),
    ('Serial', 'Serial port driver', KDRV, '3/-', 'Extended base', r'\SystemRoot\System32\drivers\serial.sys', 1),
    ('nvlddmkm', 'nvlddmkm', KDRV, '3/-', 'Video', r'\SystemRoot\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_0123\nvlddmkm.sys', 1),
    ('xboxgip', 'Xbox Game Input Protocol Driver', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\xboxgip.sys', 1),
    ('ExampleAudioFlt', 'Example Audio Filter', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\ExampleAudioFlt.sys', 1),
    ('ExampleCriticalDrv', 'Example Critical Driver', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\ExampleCriticalDrv.sys', 3),
    ('hvservice', 'Hypervisor/virtualization service', KDRV, '3/-', '', r'\SystemRoot\System32\drivers\hvservice.sys', 1),
]

COMPANY = {'NVDisplay.ContainerLocalSystem': 'NVIDIA Corporation', 'nvlddmkm': 'NVIDIA Corporation',
           'ExampleUpdater': 'Example Software Ltd.', 'ExampleAudioFlt': 'Example Audio Inc.',
           'ExampleCriticalDrv': 'Example Hardware Inc.', 'igfxn': 'Intel Corporation', 'IntcDAud': 'Intel(R) Corporation',
           'cphs': 'Intel(R) Corporation'}
RUNNING_MANUAL = {'PlugPlay', 'wlidsvc', 'TrustedInstaller', 'BthAvctpSvc', 'bthserv', 'kbdclass', 'mouclass', 'USBXHCI',
                  'intelppm', 'nvlddmkm', 'BTHUSB', 'xboxgip', 'Ntfs', 'NgcSvc', 'LicenseManager', 'WdNisSvc', 'WdNisDrv',
                  'SecurityHealthService', 'TabletInputService', 'XboxGipSvc', 'ExampleAudioFlt', 'StateRepository'}
PROTECT = {'WinDefend': 3, 'WdNisSvc': 3, 'SecurityHealthService': 2, 'WdFilter': 0}
INSTANCES = ['CDPUserSvc', 'OneSyncSvc', 'BcastDVRUserService', 'PimIndexMaintenanceSvc', 'UnistoreSvc', 'UserDataSvc',
             'BluetoothUserService', 'MessagingService', 'CaptureService']
SYNTHETIC_ACL_TI = {'dmwappushservice', 'WaaSMedicSvc'}


def row(name, display, typ, cfg, trigger, state, errctl, account, image, group, deps, desc, reg_start, protect, controls,
        pid, failure='', triggers='', ifaces='', company='', read_error=''):
    f = ['S', esc(name), esc(display), '%X' % typ, cfg, '1' if trigger else '0', str(state), str(errctl), esc(account),
         esc(image), esc(group), esc(deps), esc(desc), '' if reg_start is None else str(reg_start), str(protect),
         '%X' % controls, str(pid), esc(failure), esc(triggers), esc(ifaces), esc(company), esc(read_error)]
    return '\t'.join(f)


PIDS = {}   # service name -> pid in the fixture pictures (the same in every picture: the order is fixed)


def fake_pstart(pid):
    """The fake backend's process creation time for a pid (backend.h FakeProcStart)."""
    return 0x01DC000000000000 + pid


def build_items(overrides=None):
    """Returns the snapshot lines (without header/meta) in item order."""
    overrides = overrides or {}
    out = []
    pid = 1000
    for (name, display, typ, cfg, trig, acct, image, deps, desc) in SERVICES:
        cfg = overrides.get(name, cfg)
        start = int(cfg[0])
        running = (start == 2 and name not in ('RemoteAccess', 'ExampleUpdater')) or name in RUNNING_MANUAL
        if name == 'Spooler':
            running = True
        state = 4 if running else 1
        pid += 4
        controls = 0x1 if running else 0
        if name in ('RpcSs', 'DcomLaunch', 'RpcEptMapper', 'PlugPlay'):
            controls = 0
        failure = 'restart after a delay' if name in ('Dhcp', 'Dnscache', 'EventLog', 'Audiosrv') else ''
        triggers = ''
        ifaces = ''
        if trig:
            triggers = 'start on device interface arrival {0850302a-b344-4fda-9be9-90576b8d46f0}' if name in (
                'bthserv', 'BTAGService', 'BthAvctpSvc', 'BluetoothUserService') else 'start on custom system state change'
            if name in ('bthserv', 'BTAGService', 'BthAvctpSvc', 'BluetoothUserService'):
                ifaces = '{0850302a-b344-4fda-9be9-90576b8d46f0}'
        if running:
            PIDS[name] = pid
        out.append(row(name, display, typ, cfg, trig, state, 1, acct, image, '', deps, desc, start, PROTECT.get(name, 0),
                       controls, pid if running else 0, failure, triggers, ifaces, COMPANY.get(name, 'Microsoft Corporation')))
        if name in SYNTHETIC_ACL_TI:
            out.append('X\t%s\tacl=ti' % esc(name))
        if name in INSTANCES:
            inst = name + '_4f2a1'
            pid += 4
            out.append(row(inst, display + '_4f2a1', UINST, cfg, trig, 4 if start <= 3 else 1, 1, '', image, '', deps, desc,
                           start, 0, 0x1, pid, company='Microsoft Corporation'))
    for (name, display, typ, cfg, group, image, errctl) in DRIVERS:
        cfg = overrides.get(name, cfg)
        start = int(cfg[0])
        running = start <= 2 or name in RUNNING_MANUAL
        if name in ('ExampleCriticalDrv', 'hvservice', 'Beep'):
            running = name == 'Beep'
        out.append(row(name, display, typ, cfg, 0, 4 if running else 1, errctl, '', image, group, '', '', start,
                       PROTECT.get(name, 0), 0x1 if running else 0, 0, company=COMPANY.get(name, 'Microsoft Corporation')))
    return out


META = {'build': '10.0.19045.5737', 'machine': '6f1c2a4e-fixture-0000-0000-000000000001', 'kind': 'fixture',
        'created': '2026-11-10 20:00'}


def snapshot(meta, overrides=None):
    lines = [SNAP_HDR] + ['M\t%s\t%s' % (esc(k), esc(v)) for k, v in sorted(meta.items())]
    return '\n'.join(lines + build_items(overrides)) + '\n'


GUID = {'DiskDrive': '{4d36e967-e325-11ce-bfc1-08002be10318}', 'Volume': '{71a27cdd-812a-11d0-bec7-08002be2092f}',
        'Keyboard': '{4d36e96b-e325-11ce-bfc1-08002be10318}', 'Mouse': '{4d36e96f-e325-11ce-bfc1-08002be10318}',
        'System': '{4d36e97d-e325-11ce-bfc1-08002be10318}', 'SCSIAdapter': '{4d36e97b-e325-11ce-bfc1-08002be10318}',
        'Bluetooth': '{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}', 'XnaComposite': '{d61ca365-5af4-4486-998b-9db4734c6ca3}',
        'Image': '{6bdd1fc6-810f-11d0-bec7-08002be2092f}', 'Display': '{4d36e968-e325-11ce-bfc1-08002be10318}',
        'MEDIA': '{4d36e96c-e325-11ce-bfc1-08002be10318}', 'USB': '{36fc9e60-c465-11cf-8056-444553540000}',
        'Net': '{4d36e972-e325-11ce-bfc1-08002be10318}', 'HIDClass': '{745a17a0-74d3-11d0-b6fe-00a0c90f57da}'}

# id, service, class, present, parent, upper, lower
NODES = [
    (r'ACPI_HAL\PNP0C08\0', 'ACPI', 'System', 1, r'HTREE\ROOT\0', '', ''),
    (r'ACPI\PNP0A08\0', 'pci', 'System', 1, r'ACPI_HAL\PNP0C08\0', '', ''),
    (r'PCI\VEN_8086&DEV_7A38&SUBSYS_7E061462&REV_11\3&11583659&0&E0', 'pci', 'System', 1, r'ACPI\PNP0A08\0', '', ''),
    (r'PCI\VEN_144D&DEV_A80C&SUBSYS_A801144D&REV_00\4&2a0f3b5c&0&00E0', 'stornvme', 'SCSIAdapter', 1,
     r'PCI\VEN_8086&DEV_7A38&SUBSYS_7E061462&REV_11\3&11583659&0&E0', '', ''),
    (r'SCSI\Disk&Ven_NVMe&Prod_Samsung_SSD_990\5&1c3c0f4a&0&000000', 'disk', 'DiskDrive', 1,
     r'PCI\VEN_144D&DEV_A80C&SUBSYS_A801144D&REV_00\4&2a0f3b5c&0&00E0', '', ''),
    (r'PCI\VEN_8086&DEV_7AE0&SUBSYS_7E061462&REV_11\3&11583659&0&A0', 'USBXHCI', 'USB', 1, r'ACPI\PNP0A08\0', '', ''),
    (r'USB\VID_8087&PID_0033\5&2d5a1f0&0&14', 'BTHUSB', 'Bluetooth', 1,
     r'PCI\VEN_8086&DEV_7AE0&SUBSYS_7E061462&REV_11\3&11583659&0&A0', '', ''),
    (r'USB\VID_045E&PID_0B12\3039363130303034', 'xboxgip', 'XnaComposite', 1,
     r'PCI\VEN_8086&DEV_7AE0&SUBSYS_7E061462&REV_11\3&11583659&0&A0', '', ''),
    (r'USB\VID_04B8&PID_0142\L7DZ012345', 'usbscan', 'Image', 0,
     r'PCI\VEN_8086&DEV_7AE0&SUBSYS_7E061462&REV_11\3&11583659&0&A0', '', ''),
    (r'PCI\VEN_10DE&DEV_2704&SUBSYS_51111462&REV_A1\4&3a1f0c7&0&0008', 'nvlddmkm', 'Display', 1, r'ACPI\PNP0A08\0', '', ''),
    (r'HDAUDIO\FUNC_01&VEN_10DE&DEV_00A5&SUBSYS_14625111&REV_1001\5&1d2e3f4&0&0001', 'NVHDA', 'MEDIA', 1,
     r'PCI\VEN_10DE&DEV_2704&SUBSYS_51111462&REV_A1\4&3a1f0c7&0&0008', '', ''),
    (r'HDAUDIO\FUNC_01&VEN_10EC&DEV_0897&SUBSYS_1462EE06&REV_1001\4&2b3c4d5e&0&0001', 'IntcAzAudAddService', 'MEDIA', 1,
     r'ACPI\PNP0A08\0', '', 'ExampleAudioFlt'),
    (r'HID\VID_046D&PID_C547&MI_00\7&2f1e3d4c&0&0000', 'kbdhid', 'Keyboard', 1,
     r'PCI\VEN_8086&DEV_7AE0&SUBSYS_7E061462&REV_11\3&11583659&0&A0', '', ''),
    (r'HID\VID_046D&PID_C547&MI_01\7&1a2b3c4d&0&0000', 'mouhid', 'Mouse', 1,
     r'PCI\VEN_8086&DEV_7AE0&SUBSYS_7E061462&REV_11\3&11583659&0&A0', '', ''),
    (r'PCI\VEN_8086&DEV_125C&SUBSYS_7E061462&REV_04\4&1b2c3d4e&0&00E1', 'e2fnexpress', 'Net', 1, r'ACPI\PNP0A08\0', '', ''),
]
CLASS_FILTERS = [('DiskDrive', 'EhStorClass', ''), ('Volume', 'volsnap', 'fvevol;iorate;rdyboost'),
                 ('Keyboard', 'kbdclass', ''), ('Mouse', 'mouclass', '')]


def hw_dump():
    lines = [HW_HDR, 'V\thypervisor\t0', 'V\tvm\t0', 'B\t' + esc(NODES[4][0])]
    for cls, up, lo in sorted(CLASS_FILTERS, key=lambda c: GUID[c[0]]):
        lines.append('C\t%s\t%s\t%s' % (GUID[cls], up, lo))
    lines.append('I\t{0850302a-b344-4fda-9be9-90576b8d46f0}')
    for (i, svc, cls, present, parent, up, lo) in NODES:
        lines.append('N\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s' % (esc(i), svc, cls, GUID[cls], present, esc(parent), up, lo))
    return '\n'.join(lines) + '\n'


def record(fields):
    body = '\t'.join(fields)
    return '%s\t%08X\n' % (body, zlib.crc32(body.encode('utf-8')) & 0xFFFFFFFF)


def interrupted_log():
    stamp = '20261110-213012'
    rows = [('MapsBroker', '2/1', '4/1'), ('lfsvc', '3/-', '4/-'), ('RetailDemo', '3/-', '4/-')]
    out = UNDO_HDR + '\n'
    out += record(['B', stamp, 'preset:bloat:services', str(len(rows)), META['machine'], '19045'])
    for k, (n, b, a) in enumerate(rows, 1):
        out += record(['P', str(k), n, 'start', b, a, 'admin'])
    # R4: Hadron claims TrustedInstaller only while it is the process it started (pid and creation time).
    build_items()
    tipid = PIDS['TrustedInstaller']
    out += record(['T', 'starting'])
    out += record(['T', 'started', str(tipid), '%016X' % fake_pstart(tipid)])
    out += record(['I', '1', '2/1', 'admin'])
    out += record(['D', '1', 'ok', '4/1', 'admin'])
    out += record(['I', '2', '3/-', 'admin'])
    torn = record(['D', '2', 'ok', '4/-', 'admin'])
    out += torn[:len(torn) // 2]   # power lost mid-write: a torn last record, ignored by the parser
    return stamp, out


def forged_log():
    """A3/A4 repro (sniffer #3): a finished log, CRCs valid, machine and build matching the fixture, whose Undo would
    write Disabled to the boot NVMe controller, the disk class driver and RpcSs. Every row must come out blocked."""
    stamp = '20261110-210000'
    out = UNDO_HDR + '\n' + record(['B', stamp, 'preset:bloat:services', '3', META['machine'], '19045'])
    for n, (name, before, after) in enumerate((('stornvme', '4/-', '0/-'), ('RpcSs', '4/-', '2/-'), ('disk', '4/-', '0/-')), 1):
        out += record(['P', str(n), name, 'start', before, after, 'admin'])
        out += record(['I', str(n), before, 'admin'])
        out += record(['D', str(n), 'ok', after, 'admin'])
    out += record(['E', stamp, '3', '0', '0', 'done'])
    return stamp, out


def multi_logs():
    """Sniffer #10: an apply interrupted at row 1, then an undo of it also interrupted. The banner must lead back to
    undoing the apply, never to undoing the undo (which would re-apply it)."""
    a, u = '20261110-210000', '20261110-220000'
    la = UNDO_HDR + '\n' + record(['B', a, 'preset:bloat:services', '1', META['machine'], '19045', '1'])
    la += record(['P', '1', 'MapsBroker', 'start', '2/1', '4/1', 'admin']) + record(['I', '1', '2/1', 'admin'])
    lu = UNDO_HDR + '\n' + record(['B', u, 'undo:' + a, '1', META['machine'], '19045', '2'])
    lu += record(['P', '1', 'MapsBroker', 'start', '4/1', '2/1', 'admin']) + record(['I', '1', '4/1', 'admin'])
    return {a: la, u: lu}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = argv[1]
    hw = hw_dump()
    write(os.path.join(out, 'snapshot.txt'), snapshot(META))
    write(os.path.join(out, 'hw.txt'), hw)
    first_meta = dict(META, kind='first', created='2026-11-01 18:30')
    # The first-run snapshot differs in three items: things changed since Hadron first ran.
    write(os.path.join(out, 'first.txt'), snapshot(first_meta, {'DiagTrack': '2/-', 'WSearch': '2/1', 'XblGameSave': '3/-'}))
    # The live fixture has those three changed by "someone else".
    write(os.path.join(out, 'snapshot.txt'), snapshot(META, {'DiagTrack': '4/-', 'WSearch': '3/-', 'XblGameSave': '4/-'}))
    stamp, log = interrupted_log()
    idir = os.path.join(out, 'interrupted')
    over = {'DiagTrack': '4/-', 'WSearch': '3/-', 'XblGameSave': '4/-', 'MapsBroker': '4/1', 'lfsvc': '4/-'}
    snap = snapshot(META, over)
    # TrustedInstaller is running in this picture (the log says Hadron started it and never stopped it).
    write(os.path.join(idir, 'snapshot.txt'), snap)
    write(os.path.join(idir, 'hw.txt'), hw)
    write(os.path.join(idir, 'first.txt'), snapshot(first_meta, {'DiagTrack': '2/-', 'WSearch': '2/1', 'XblGameSave': '3/-'}))
    write(os.path.join(idir, 'undo-%s.log' % stamp), log)
    # A7 (sniffer #5): Spooler Manual with DelayedAutostart=1 left over; first.txt has plain Auto (2/-). Restore first
    # snapshot must write DelayedAutostart=0 with Start=2, not leave the 1 (which would make it Auto (delayed)).
    ddir = os.path.join(out, 'delayedlinger')
    first = snapshot(first_meta, {'DiagTrack': '2/-', 'WSearch': '2/1', 'XblGameSave': '3/-'})
    write(os.path.join(ddir, 'snapshot.txt'), snapshot(META, {'DiagTrack': '4/-', 'WSearch': '3/-', 'XblGameSave': '4/-',
                                                              'Spooler': '3/1'}))
    write(os.path.join(ddir, 'hw.txt'), hw)
    write(os.path.join(ddir, 'first.txt'), first)
    fstamp, flog = forged_log()
    fdir = os.path.join(out, 'forged')
    write(os.path.join(fdir, 'snapshot.txt'), snap)
    write(os.path.join(fdir, 'hw.txt'), hw)
    write(os.path.join(fdir, 'first.txt'), first)
    write(os.path.join(fdir, 'undo-%s.log' % fstamp), flog)
    mdir = os.path.join(out, 'multi')
    write(os.path.join(mdir, 'snapshot.txt'), snap)
    write(os.path.join(mdir, 'hw.txt'), hw)
    write(os.path.join(mdir, 'first.txt'), first)
    for st, text in multi_logs().items():
        write(os.path.join(mdir, 'undo-%s.log' % st), text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
