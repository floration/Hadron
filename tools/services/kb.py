# tools/services/kb.py — the Services page knowledge base (source). genkb.py turns it into
# modules/services/data/services.db, which is embedded in the exe as RCDATA SERVICES_KB.
#
# Rule 7 (CLAUDE.md): every entry has a `basis` for its explanation, every preset membership has a `pbasis`,
# no numbers about savings, folklore is labelled. genkb.py enforces it with the bios checks (tools/bios/genkb.py).
#
# Source ids: the [n] comments are the numbered sources in docs/pipeline/svcdrv/research/r3-documentation.md
# ("Sources"); r3 §2.2 is the per-service matrix the memberships come from. Each SOURCES key names the OS the
# page was written for, and the notes say so, because none of them is about a gaming desktop.
#
# Fields (see tools/services/README.md):
#   name, kind ('svc'|'drv'), title, expl, if_off, test, basis, basis_note,
#   levels {'bloat'|'stripped'|'minimum': (target 3|4, why)}, pbasis, pbasis_note,
#   sysok (drivers: may be in a level), never (explanation only, no preset), hw (hardware hint),
#   contested (text naming the sources on both sides with @KEYs), stock (+ stock_basis, stock_note).
#
# "No guidance" is a keep-the-default rating (DECISIONS.md 2026-11-11, proxy). The client-OS @IOT page is the
# primary source: an IoT No guidance keeps an item out of Pure bloat (Stripped at most); a @WS16 No guidance alone
# does not, but the pbasis says so. Every pbasis names a WS16 No guidance where r3 §2.2 (or kb-auditor) records one.
#
# hw hints: class:<DeviceClass> (any devnode of that class, present or seen before, keeps the item),
#           svc:<service> / self (a devnode whose function driver or filter is that service), vmguest, hvhost.

BUILD = '19045'

SOURCES = {
    'IOT': 'https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/optimize/services',            # r3 [15]
    'WS16': 'https://learn.microsoft.com/en-us/windows-server/security/windows-services/'
            'security-guidelines-for-disabling-system-services-in-windows-server',                        # r3 [12]
    'VDI': 'https://learn.microsoft.com/en-us/windows-server/remote/remote-desktop-services/'
           'rds-vdi-recommendations-2004',                                                                # r3 [14]
    # r3 [16] cited the raw.githubusercontent.com copy; the GitHub page of the same file reads in a browser (checked
    # 2026-11-11: 20 Disabled, InstallService/defragsvc/VSS Unchanged, as r3 says).
    'VDOT': 'https://github.com/The-Virtual-Desktop-Team/Virtual-Desktop-Optimization-Tool/blob/'
            'main/2009/ConfigurationFiles/Services.json',                                                 # r3 [16]
    'VDOTREADME': 'https://github.com/The-Virtual-Desktop-Team/Virtual-Desktop-Optimization-Tool',        # r3 [17]
    'RTLF': 'https://learn.microsoft.com/en-us/windows/privacy/'
            'manage-connections-from-windows-operating-system-components-to-microsoft-services',          # r3 [18]
    'PERUSER': 'https://learn.microsoft.com/en-us/windows/application-management/per-user-services-in-windows',  # r3 [13]
    'DEFCOMPAT': 'https://learn.microsoft.com/en-us/defender-endpoint/microsoft-defender-antivirus-compatibility',  # r3 [22]
    'TAMPER': 'https://learn.microsoft.com/en-au/defender-endpoint/tamper-protection-overview',           # r3 [21]
    'PROTSVC': 'https://learn.microsoft.com/en-us/windows/desktop/Services/protecting-anti-malware-services-',  # r3 [9]
    'STARTMENU': 'https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/'
                 'troubleshoot-start-menu-errors',                                                         # r3 [26]
    'WUERR': 'https://learn.microsoft.com/en-us/troubleshoot/windows-client/deployment/common-windows-update-errors',  # r3 [28]
    'STOP7B': 'https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/'
              'stop-error-7b-or-inaccessible-boot-device-troubleshooting',                                 # r3 [29]
    'REGBACK': 'https://learn.microsoft.com/en-us/troubleshoot/windows-client/deployment/'
               'system-registry-no-backed-up-regback-folder',                                              # r3 [30]
    'LOADORDER': 'https://learn.microsoft.com/en-in/windows-hardware/drivers/install/specifying-driver-load-order',  # r3 [24]
    'ADDSERVICE': 'https://learn.microsoft.com/en-au/WINDOWS-HARDWARE/DRIVERS/install/inf-addservice-directive',  # r3 [23]
    'TRIGGERS': 'https://learn.microsoft.com/en-us/windows/win32/services/service-trigger-events',         # r3 [10]
    # Driver pages added in Finalize (2026-11-11), each read before use; the KB cites only what the page says.
    'HIDKBM': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers',
    'USBHOST': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-3-0-driver-stack-architecture',
    'USBCLASSES': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes',
    'STORPORT': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/storport-driver-overview',
    'STORNVME': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/storage/nvme-features-supported-by-stornvme',
    'STORAHCI': 'https://learn.microsoft.com/en-us/windows/compatibility/storahci-replaces-msahci',
    'FLTMGR': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/filter-manager-concepts',
    'BASICDISPLAY': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver',
    'USBAUDIO2': 'https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers',
}

# How each source reads in running text. In basis and contested notes the URL follows in parentheses.
LABELS = {
    'IOT': 'Microsoft\'s IoT Enterprise services page',
    'WS16': 'Microsoft\'s Windows Server 2016 service guidance',
    'VDI': 'Microsoft\'s Windows 10 2004 VDI guide (archived)',
    'VDOT': 'Microsoft\'s VDI optimization tool (Windows 10 20H2+ virtual desktops)',
    'VDOTREADME': 'the VDI optimization tool\'s README',
    'RTLF': 'Microsoft\'s restricted-traffic baseline',
    'PERUSER': 'Microsoft\'s per-user services page',
    'DEFCOMPAT': 'Microsoft\'s Defender compatibility page',
    'TAMPER': 'Microsoft\'s Tamper Protection page',
    'PROTSVC': 'Microsoft\'s protected anti-malware services page',
    'STARTMENU': 'Microsoft\'s Start menu troubleshooting page',
    'WUERR': 'Microsoft\'s Windows Update errors page',
    'STOP7B': 'Microsoft\'s Stop 0x7B troubleshooting page',
    'REGBACK': 'Microsoft\'s RegBack page',
    'LOADORDER': 'Microsoft\'s driver load order page',
    'ADDSERVICE': 'Microsoft\'s INF AddService page',
    'TRIGGERS': 'Microsoft\'s service trigger events page',
    'HIDKBM': 'Microsoft\'s keyboard and mouse HID drivers page',
    'USBHOST': 'Microsoft\'s USB host-side drivers page',
    'USBCLASSES': 'Microsoft\'s inbox USB class drivers page',
    'STORPORT': 'Microsoft\'s Storport page',
    'STORNVME': 'Microsoft\'s StorNVMe page',
    'STORAHCI': 'Microsoft\'s StorAHCI page',
    'FLTMGR': 'Microsoft\'s Filter Manager page',
    'BASICDISPLAY': 'Microsoft\'s Basic Display Driver page',
    'USBAUDIO2': 'Microsoft\'s USB Audio 2.0 drivers page',
}

ENTRIES = []

# Ratings are quoted with each page's own label: @IOT says "Don't disable", @WS16 says "Do not disable".
NG_WS16 = '@WS16 rates it "No guidance" (Windows Server 2016), which Hadron reads as keep the default.'

LEVELS = ('bloat', 'stripped', 'minimum')

# Reusable membership notes. Each names the sources and the OS they were written for (r3 §2.1).
P_BLOAT_IOT = ('@IOT rates it "OK to disable" on Windows 10/11 IoT Enterprise (a device-maker page, not a desktop page); '
               'no Microsoft source Hadron checked says to keep it.')
P_BLOAT_IOT_VDOT = ('@IOT rates it "OK to disable" (Windows 10/11 IoT Enterprise) and @VDOT disables it '
                    'for virtual desktops; no Microsoft source Hadron checked says to keep it.')
P_XBOX = ('@IOT rates it "Should be disabled" (Windows 10/11 IoT Enterprise) and @VDOT disables it for virtual desktops; '
          'both are written for managed enterprise devices and virtual desktops, not gaming PCs.')
# Stripped: Hadron does not detect use (kb-audit F3). A hardware hint keeps the item when the hardware is present or
# was seen; with no hint, Stripped offers the row unticked and Minimum ticks it (proxy Q1).
_STRIP_HOW = (' Hadron does not detect whether you use it: with a hardware hint the item is kept when that hardware is '
              'present or was seen before; without one, Stripped offers the row unticked for you to decide and Minimum '
              'ticks it.')
P_STRIP_IOT = ('@IOT rates it "OK to disable" on Windows 10/11 IoT Enterprise, which it defines as "useful to some but '
               'not all enterprises".' + _STRIP_HOW)
P_STRIP_IOT_WS16 = ('@IOT (Windows 10/11 IoT Enterprise) and @WS16 (Windows Server 2016) both rate it "OK to disable"; '
                    '@IOT defines that as "useful to some but not all enterprises".' + _STRIP_HOW)
# Contested Minimum items (kb-audit F4): the sources split, so membership is Hadron's reasoning; the ratings themselves
# are quoted in Contested with their URLs.
P_CONTESTED = ('Microsoft sources disagree (see Contested): @VDOT disables it for virtual desktops. Hadron puts it in '
               'Minimum because Windows boots, networks and runs games without it (reasoning); whether that helps is '
               'not known: test it.')


def E(name, kind, title, expl, if_off='', test='', basis='reasoning', basis_note='', levels=None, pbasis='',
      pbasis_note='', sysok=False, never=False, hw='', contested='', stock=None, stock_basis='', stock_note='',
      builds=None, sources=''):
    ENTRIES.append(dict(name=name, kind=kind, builds=builds or [BUILD], title=title, expl=expl, if_off=if_off, test=test,
                        basis=basis, basis_note=basis_note, levels=levels or {}, pbasis=pbasis,
                        pbasis_note=pbasis_note, sysok=sysok, never=never, hw=hw, contested=contested, stock=stock,
                        stock_basis=stock_basis, stock_note=stock_note, sources=sources))


def iot(extra=''):
    return ('@IOT describes this service (Windows 10/11 IoT Enterprise services page). What breaks is Hadron\'s '
            'reasoning unless a source is named.' + (' ' + extra if extra else ''))


# =====================================================================================================
# Pure bloat removal (services). r3 §2.2: @IOT rates each OK or Should disable, and no source rates it Do not disable.

E('DiagTrack', 'svc', 'Connected User Experiences and Telemetry',
  'Collects and sends Windows diagnostic and usage data to Microsoft.',
  if_off='Diagnostic data upload stops. Feedback Hub and some troubleshooters report less; Windows keeps working.',
  test='Disable, restart, use the PC as usual for a few days; check that Windows Update still runs.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'telemetry upload, not needed for anything on the PC itself')},
  pbasis='published', pbasis_note='@IOT rates it "OK to disable" (Windows 10/11 IoT Enterprise) and @VDOT disables it; '
                                  + NG_WS16 + ' @VDI lists it as a candidate on disconnected networks.')

E('dmwappushservice', 'svc', 'Device Management WAP Push message Routing Service',
  'Routes device-management push messages, used when a PC is enrolled in Intune or another MDM.',
  if_off='MDM enrolment and pushes stop. A home PC that is not managed by an organisation is unaffected.',
  test='Disable only if this PC is not managed by work or school (Settings > Accounts > Access work or school).',
  basis='published', basis_note='@WS16 says it is required on client devices for Intune and MDM (Windows Server 2016 page).',
  levels={'stripped': (4, 'work or school device management; leave unticked if this PC is managed (Intune/MDM)')},
  pbasis='published', pbasis_note='@WS16 rates it "OK to disable" (Windows Server 2016) and names MDM and Unified Write '
                                  'Filter as its uses; @IOT rates it "No guidance" (its legend: "the default configuration of '
                                  'these services shouldn\'t be changed"), so it is not in Pure bloat removal.')

E('MapsBroker', 'svc', 'Downloaded Maps Manager',
  'Lets apps reach downloaded offline maps.',
  if_off='The Maps app and other apps lose access to offline maps.',
  test='Disable, then open the Maps app if you use it.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'offline maps for the Maps app')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT + ' @WS16 also rates it "OK to disable".')

E('lfsvc', 'svc', 'Geolocation Service',
  'Tracks the location of the PC and runs geofences for apps that ask for location.',
  if_off='Apps and Windows features that use location (weather, maps, Find my device, automatic time zone by '
         'location) stop getting it.',
  test='Disable, then check the apps you rely on for location.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'location for apps; desktops rarely need it (reasoning)')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT + ' @WS16 also rates it "OK to disable".')

E('RetailDemo', 'svc', 'Retail Demo Service',
  'Runs the store-display demo mode of Windows.',
  if_off='Retail demo mode is unavailable. Hadron knows of nothing else that uses it (reasoning).',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'shop-display demo mode only')},
  pbasis='published', pbasis_note=P_BLOAT_IOT)

E('WMPNetworkSvc', 'svc', 'Windows Media Player Network Sharing Service',
  'Shares Windows Media Player libraries with other players and devices on the network.',
  if_off='Windows Media Player library sharing over the network stops.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'media library sharing from the legacy Windows Media Player')},
  pbasis='published', pbasis_note=P_BLOAT_IOT)

E('MessagingService', 'svc', 'MessagingService (per-user template)',
  'Text messaging (SMS) for devices with a cellular modem. A per-user service: Hadron changes the template.',
  if_off='SMS and MMS through a cellular modem stop. On a desktop without a modem Hadron knows of nothing that uses '
         'it (reasoning).',
  basis='published', basis_note=iot('@PERUSER lists it as a per-user service.'),
  levels={'bloat': (4, 'text messaging over a cellular modem')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT)

E('SmsRouter', 'svc', 'Microsoft Windows SMS Router Service',
  'Routes text messages to apps, for devices with a cellular modem.',
  if_off='SMS routing to apps stops.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'text messaging over a cellular modem')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT)

E('SEMgrSvc', 'svc', 'Payments and NFC/SE Manager',
  'Manages payments and NFC secure elements.',
  if_off='NFC tap-to-pay and secure-element payments stop. Desktops rarely have NFC (reasoning).',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'NFC payments')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT)

E('PhoneSvc', 'svc', 'Phone Service',
  'Manages the telephony state of the device (cellular and VoIP calling for apps).',
  if_off='Apps that place calls through Windows telephony lose it. Many desktop VoIP apps don\'t use it (reasoning): '
         'test your calling app.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'Windows telephony state for cellular or VoIP apps')},
  pbasis='published', pbasis_note=P_BLOAT_IOT + ' @WS16 also rates it "OK to disable".')

E('WalletService', 'svc', 'WalletService',
  'Hosts objects used by Wallet clients.',
  if_off='The old Windows Wallet features stop.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'Wallet clients')},
  pbasis='published', pbasis_note=P_BLOAT_IOT + ' @WS16 also rates it "OK to disable".')

E('icssvc', 'svc', 'Windows Mobile Hotspot Service',
  'Shares this PC\'s internet connection as a Wi-Fi or Bluetooth hotspot.',
  if_off='Settings > Mobile hotspot stops working.',
  test='Disable, then check Settings > Network > Mobile hotspot if you ever use it.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'mobile hotspot')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT + ' @WS16 also rates it "OK to disable".')

E('wisvc', 'svc', 'Windows Insider Service',
  'Infrastructure for the Windows Insider Program.',
  if_off='Joining or staying in Windows Insider builds stops working.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'Windows Insider Program only')},
  pbasis='published', pbasis_note=P_BLOAT_IOT + ' @WS16 also rates it "OK to disable".')

E('BcastDVRUserService', 'svc', 'GameDVR and Broadcast User Service (per-user template)',
  'Game recording and broadcasting through the Xbox Game Bar. A per-user service: Hadron changes the template.',
  if_off='Game Bar recording (Win+Alt+R) and its background capture stop. OBS and GPU-vendor recorders are separate.',
  test='Disable, restart, then try Win+Alt+R in a game if you use Game Bar recording.',
  basis='published', basis_note=iot('@PERUSER lists it as a per-user service.'),
  levels={'bloat': (4, 'Game Bar recording and broadcasting')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT)

E('autotimesvc', 'svc', 'Cellular Time',
  'Sets the clock from the cellular network.',
  if_off='Time from the cellular network stops. Internet time (W32Time) is a different service and keeps working.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'clock from a cellular network')},
  pbasis='published', pbasis_note=P_BLOAT_IOT_VDOT)

E('XblAuthManager', 'svc', 'Xbox Live Auth Manager',
  'Signs apps in to Xbox services.',
  if_off='The Xbox app, Game Pass PC games and games with Xbox sign-in fail to sign in. Untick it if you play through '
         'the Xbox app or Game Pass.',
  test='After disabling, launch a Game Pass or Microsoft Store game; if it cannot sign in, undo.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'Xbox sign-in; untick if you use the Xbox app or Game Pass')},
  pbasis='published', pbasis_note=P_XBOX)

E('XblGameSave', 'svc', 'Xbox Live Game Save',
  'Syncs game saves with Xbox cloud saves.',
  if_off='Cloud saves for Xbox-enabled games stop syncing; @WS16 says game save data will not upload to or download from '
         'Xbox Live.',
  test='After disabling, start a Game Pass game and check its cloud-save status.',
  basis='published', basis_note=iot('@WS16 states the save-sync effect (Windows Server 2016 page).'),
  levels={'bloat': (4, 'Xbox cloud saves; untick if you use them')},
  pbasis='published', pbasis_note=P_XBOX + ' @WS16 also rates it "Should be disabled".')

E('XboxNetApiSvc', 'svc', 'Xbox Live Networking Service',
  'Network support for the Xbox Live APIs, including the Xbox networking check in the Game Bar.',
  if_off='Xbox Live multiplayer features in some Xbox-enabled games can fail.',
  basis='published', basis_note=iot(),
  levels={'bloat': (4, 'Xbox Live networking; untick if you play Xbox-enabled online games')},
  pbasis='published', pbasis_note=P_XBOX)

# =====================================================================================================
# Stripped (services): never-seen hardware, or use Hadron can't detect (offered unticked in Stripped; Minimum ticks it).

E('Spooler', 'svc', 'Print Spooler',
  'Queues print jobs and talks to printers, including Print to PDF and OneNote printing.',
  if_off='No printing at all, including Microsoft Print to PDF.',
  test='Disable only if you never print (Print to PDF counts).',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'printing')}, hw='class:Printer',
  pbasis='published', pbasis_note='@IOT says that with it off you are not able to print (Windows 10/11 IoT Enterprise); '
                                  '@WS16 rates it "OK to disable" when the machine is not a print server. Printers are a '
                                  'hot-plug class, so an absent printer leaves the row unticked.')

E('PrintNotify', 'svc', 'Printer Extensions and Notifications',
  'Opens printer vendor dialogs and print-job notifications.',
  if_off='Vendor printer pop-ups and extensions stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'printer extensions')}, hw='class:Printer',
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('bthserv', 'svc', 'Bluetooth Support Service',
  'Discovers and pairs Bluetooth devices.',
  if_off='No Bluetooth pairing or connections: headsets, controllers and mice over Bluetooth stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'Bluetooth')}, hw='class:Bluetooth',
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('BTAGService', 'svc', 'Bluetooth Audio Gateway Service',
  'Hands-free profile for Bluetooth headsets (microphone and call audio).',
  if_off='Bluetooth headset microphones and call audio stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'Bluetooth headset calls')}, hw='class:Bluetooth',
  pbasis='published', pbasis_note=P_STRIP_IOT)

E('BthAvctpSvc', 'svc', 'AVCTP service',
  'Bluetooth audio/video remote control (play and pause buttons on headphones).',
  if_off='Media buttons on Bluetooth headphones stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'Bluetooth media buttons')}, hw='class:Bluetooth',
  pbasis='published', pbasis_note=P_STRIP_IOT)

E('BluetoothUserService', 'svc', 'Bluetooth User Support Service (per-user template)',
  'Per-user Bluetooth features. A per-user service: Hadron changes the template.',
  if_off='Per-user Bluetooth functions stop.',
  basis='published', basis_note=iot('@PERUSER lists it as a per-user service.'),
  levels={'stripped': (4, 'Bluetooth')}, hw='class:Bluetooth',
  pbasis='published', pbasis_note=P_STRIP_IOT)

E('XboxGipSvc', 'svc', 'Xbox Accessory Management Service',
  'Manages Xbox accessories such as controllers (firmware updates and configuration).',
  if_off='Xbox controller updates and the Xbox Accessories app stop; wired controllers can lose features.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'Xbox accessories; kept when a device using the xboxgip driver was ever seen')},
  hw='svc:xboxgip',
  pbasis='published', pbasis_note=P_XBOX + ' Hadron keeps it when a device whose driver is xboxgip (the Xbox accessory '
                                           'driver) is present or was seen before; which device class such accessories '
                                           'use is not sourced, so Hadron matches the driver instead.')

E('WSearch', 'svc', 'Windows Search',
  'Indexes files, mail and other content for fast search in Start, Explorer and Outlook.',
  if_off='Start and Explorer search get slow and less complete; Outlook search can break. @VDI says to test before '
         'disabling it.',
  test='Disable, restart, and search in Start and Explorer for a while before deciding.',
  basis='published', basis_note=iot('@VDI adds a test-first note (Windows 10 2004 VDI guide).'),
  levels={'stripped': (4, 'search indexing; leave unticked if you search from Start or Outlook')},
  pbasis='published', pbasis_note='@IOT rates it "OK to disable" (Windows 10/11 IoT Enterprise); @VDI lists it as a '
                                  'VDI candidate with a test-first note.')

E('CDPSvc', 'svc', 'Connected Devices Platform Service',
  'Connects to other devices: Phone Link, Nearby sharing, Bluetooth and Cast discovery.',
  if_off='Phone Link, Nearby sharing and cross-device features stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'cross-device features (Phone Link, Nearby sharing)')},
  pbasis='published', pbasis_note=P_STRIP_IOT + ' ' + NG_WS16 + ' @VDI lists it as a VDI candidate.')

E('CDPUserSvc', 'svc', 'Connected Devices Platform User Service (per-user template)',
  'Per-user part of the Connected Devices Platform. A per-user service: Hadron changes the template.',
  if_off='Phone Link, Nearby sharing and clipboard sync stop.',
  basis='published', basis_note=iot('@PERUSER lists it as a per-user service.'),
  levels={'stripped': (4, 'cross-device features (Phone Link, Nearby sharing)')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('OneSyncSvc', 'svc', 'Sync Host (per-user template)',
  'Syncs mail, contacts, calendar and other user data. A per-user service: Hadron changes the template.',
  if_off='@VDI says Mail and other UWP apps will not work properly.',
  basis='published', basis_note=iot('@VDI states the Mail effect.'),
  levels={'stripped': (4, 'Mail/Calendar/People sync; leave unticked if you use those apps')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16 + ' @VDI lists it as a VDI candidate.')

E('PimIndexMaintenanceSvc', 'svc', 'Contact Data (per-user template)',
  'Indexes contact data for fast contact search. A per-user service: Hadron changes the template.',
  if_off='Contact search in the People and Mail apps is slower or missing.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'contact indexing for the People and Mail apps')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('SharedAccess', 'svc', 'Internet Connection Sharing (ICS)',
  'Shares an internet connection with other devices; also used by Mobile hotspot and Miracast.',
  if_off='@WS16 says it is required for Wi-Fi hotspots and both ends of Miracast projection.',
  basis='published', basis_note='@WS16 states what needs it (Windows Server 2016 page). ' + iot(),
  levels={'stripped': (4, 'connection sharing, hotspot and Miracast')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('RmSvc', 'svc', 'Radio Management Service',
  'Manages radios and airplane mode.',
  if_off='Airplane mode and the radio switches in Settings stop working.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'airplane mode; desktops rarely use it (reasoning)')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16 + ' @VDOT disables it for virtual desktops.')

E('PcaSvc', 'svc', 'Program Compatibility Assistant Service',
  'Watches programs for known compatibility problems and offers fixes.',
  if_off='The compatibility prompts for old programs stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'compatibility prompts for old programs')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

for _n, _t, _d in (('SensorService', 'Sensor Service', 'Manages sensors such as light and orientation sensors.'),
                   ('SensrSvc', 'Sensor Monitoring Service', 'Watches sensors to adjust the display, e.g. brightness.'),
                   ('SensorDataService', 'Sensor Data Service', 'Delivers data from sensors to apps.')):
    E(_n, 'svc', _t, _d,
      if_off='Sensor-driven features (auto brightness, rotation) stop. Desktops usually have no such sensors '
             '(reasoning).',
      basis='published', basis_note=iot(),
      levels={'stripped': (4, 'sensors; kept when a sensor device was ever seen')}, hw='class:Sensor',
      pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('SSDPSRV', 'svc', 'SSDP Discovery',
  'Discovers UPnP devices on the network (media players, some printers and NAS boxes).',
  if_off='UPnP device discovery stops; network devices may not show up in Explorer\'s Network view. Apps or games '
         'that use Windows\' UPnP API to open router ports can lose it (reasoning): test it.',
  test='Note the NAT type a game reports (or your router\'s UPnP port list) before and after a restart with it '
       'disabled; undo if it gets stricter or the game\'s ports no longer appear.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'UPnP discovery')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('upnphost', 'svc', 'UPnP Device Host',
  'Lets this PC host UPnP devices for others to find.',
  if_off='This PC no longer acts as a UPnP device host.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'hosting UPnP devices')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('lltdsvc', 'svc', 'Link-Layer Topology Discovery Mapper',
  'Draws the network map in the old Network and Sharing Center.',
  if_off='The network map is empty.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'network map')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('QWAVE', 'svc', 'Quality Windows Audio Video Experience',
  'Network quality-of-service for audio/video streaming on home networks.',
  if_off='Apps that use qWave for streaming QoS lose it.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'home-network streaming QoS')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('stisvc', 'svc', 'Windows Image Acquisition (WIA)',
  'Image acquisition from scanners and cameras.',
  if_off='Scanning through Windows Fax and Scan or WIA apps stops.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'scanners and cameras; kept when one was ever seen')}, hw='class:Image',
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('WiaRpc', 'svc', 'Still Image Acquisition Events',
  'Launches apps when a scanner or camera event happens.',
  if_off='Scanner-button and camera-event launches stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'scanner and camera events')}, hw='class:Image',
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('FrameServer', 'svc', 'Windows Camera Frame Server',
  'Lets several apps use a camera at the same time.',
  if_off='Webcams stop working in apps that use the frame server, which includes the Camera app.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'cameras; kept when one was ever seen')}, hw='class:Camera',
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

E('ShellHWDetection', 'svc', 'Shell Hardware Detection',
  'Sends AutoPlay events when media or devices are inserted.',
  if_off='AutoPlay prompts stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'AutoPlay')},
  pbasis='published', pbasis_note=P_STRIP_IOT_WS16)

for _n, _t, _d, _w in (('ScDeviceEnum', 'Smart Card Device Enumeration Service', 'Creates software devices for smart cards.',
                        '@WS16 also rates it "OK to disable".'),
                       ('SCardSvr', 'Smart Card', 'Manages access to smart cards.',
                        '@WS16 lists it as "Already disabled" (Server 2016).'),
                       ('SCPolicySvc', 'Smart Card Removal Policy',
                        'Locks the PC when a smart card is removed, if a policy says so.', NG_WS16)):
    E(_n, 'svc', _t, _d,
      if_off='Smart card sign-in and smart card readers stop.',
      basis='published', basis_note=iot(),
      levels={'stripped': (4, 'smart cards; kept when a reader was ever seen')}, hw='class:SmartCardReader',
      pbasis='published', pbasis_note=P_STRIP_IOT + ' ' + _w)

for _n, _t in (('vmickvpexchange', 'Hyper-V Data Exchange Service'), ('vmicguestinterface', 'Hyper-V Guest Service Interface'),
               ('vmicshutdown', 'Hyper-V Guest Shutdown Service'), ('vmicheartbeat', 'Hyper-V Heartbeat Service'),
               ('vmicvmsession', 'Hyper-V PowerShell Direct Service'), ('vmicrdv', 'Hyper-V Remote Desktop Virtualization Service'),
               ('vmictimesync', 'Hyper-V Time Synchronization Service'), ('vmicvss', 'Hyper-V Volume Shadow Copy Requestor')):
    E(_n, 'svc', _t,
      'Hyper-V integration for this Windows when it runs as a virtual machine guest.',
      if_off='Only matters inside a Hyper-V virtual machine; on physical hardware nothing uses it.',
      basis='published', basis_note=iot(),
      levels={'stripped': (4, 'Hyper-V guest integration; kept when this Windows is a VM guest')}, hw='vmguest',
      pbasis='published', pbasis_note=P_STRIP_IOT + ' Kept when the firmware says this Windows is a virtual machine.',
      contested='@WS16 rates the Hyper-V integration services "Do not disable" (Windows Server 2016); @IOT rates them '
                '"OK to disable" (Windows 10/11 IoT Enterprise).')

E('HvHost', 'svc', 'HV Host Service',
  'Performance interface for the Hyper-V hypervisor on the host side.',
  if_off='Hyper-V hosting features lose it. Kept while the hypervisor runs (Hyper-V, WSL2, Memory Integrity).',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'Hyper-V host interface; kept while the hypervisor runs')}, hw='hvhost',
  pbasis='published', pbasis_note=P_STRIP_IOT,
  contested='@WS16 rates HvHost "Do not disable" (Windows Server 2016); @IOT rates it "OK to disable" (Windows 10/11 IoT '
            'Enterprise).')

E('RasAuto', 'svc', 'Remote Access Auto Connection Manager',
  'Dials a remote-access connection when a program asks for an address that needs one.',
  if_off='Automatic dial-up and VPN connections on demand stop; VPN apps with their own client are unaffected '
         '(reasoning).',
  test='If you use the built-in Windows VPN client: restart with it disabled, then connect that VPN from Settings > '
       'Network > VPN and open something that needs it; undo if it fails.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'dial-up and on-demand VPN connections')},
  pbasis='published', pbasis_note=P_STRIP_IOT + ' ' + NG_WS16)

E('RemoteAccess', 'svc', 'Routing and Remote Access',
  'Routing services for businesses on local and wide-area networks. Disabled by default on Windows 10.',
  if_off='Routing and Remote Access features stop.',
  basis='published', basis_note=iot(),
  levels={'stripped': (4, 'LAN/WAN routing for businesses')},
  pbasis='published', pbasis_note='@IOT and @WS16 list it as "Already disabled" (disabled by default).')

E('cphs', 'svc', 'Intel Content Protection HECI Service',
  'Intel graphics content protection (protected video playback) on Intel integrated graphics.',
  if_off='Protected video playback on Intel integrated graphics stops; with no Intel integrated graphics nothing uses it.',
  basis='reasoning', basis_note='Ships with the Intel graphics driver package; how it is used is inferred from that, no '
                                'published Microsoft source.',
  levels={'stripped': (4, 'Intel integrated graphics only; kept when the Intel graphics driver has a device')},
  hw='svc:igfxn',
  pbasis='reasoning', pbasis_note='The hardware it serves (Intel integrated graphics) is absent when no devnode for '
                                  'igfxn was ever seen.')

# =====================================================================================================
# Minimum (services): contested items Microsoft sources disagree on. Never the never-in-preset set.

E('SysMain', 'svc', 'SysMain (Superfetch)',
  'Preloads programs you use often into memory, from how you have used the PC. @IOT describes it only as '
  '"Maintains and improves system performance over time".',
  if_off='Whether turning it off helps or hurts depends on the workload, and Hadron has no measurement either way '
         '(reasoning: it preloads for your usage pattern, so cold launches can get slower): test it.',
  test='3 restarts each way, same drivers; time cold launches of your usual apps; capture frame times with PresentMon '
       'or CapFrameX over the same game scene; keep whichever is better.',
  basis='published', basis_note='@IOT gives the description quoted above (Windows 10/11 IoT Enterprise). How it '
                                'decides what to preload is not stated there; the summary is reasoning.',
  levels={'minimum': (4, 'memory preloading; contested, test it')},
  pbasis='reasoning', pbasis_note=P_CONTESTED + ' ' + NG_WS16,
  contested='@IOT rates SysMain "Don\'t disable" (Windows 10/11 IoT Enterprise); @VDOT disables it for virtual desktops '
            'and @VDI says to evaluate it in each environment.')

E('DPS', 'svc', 'Diagnostic Policy Service',
  'Problem detection and troubleshooting for Windows components.',
  if_off='Windows troubleshooters and problem detection stop.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'diagnostics; contested')},
  pbasis='reasoning', pbasis_note=P_CONTESTED + ' ' + NG_WS16,
  contested='@IOT rates DPS "Don\'t disable" (Windows 10/11 IoT Enterprise); @VDOT disables it for virtual desktops.')

E('WdiSystemHost', 'svc', 'Diagnostic System Host',
  'Hosts system-level diagnostics for the Diagnostic Policy Service.',
  if_off='System diagnostics stop.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'diagnostics; contested')},
  pbasis='reasoning', pbasis_note=P_CONTESTED + ' ' + NG_WS16,
  contested='@IOT rates WdiSystemHost "Don\'t disable" (Windows 10/11 IoT Enterprise); @VDOT disables it for virtual '
            'desktops.')

E('WdiServiceHost', 'svc', 'Diagnostic Service Host',
  'Hosts service-level diagnostics for the Diagnostic Policy Service.',
  if_off='Service-level problem detection for the Diagnostic Policy Service stops.',
  basis='reasoning', basis_note='From its registered Windows description. @IOT, @VDI and @VDOT do not rate it and '
                                '@WS16 rates it "No guidance"; it hosts diagnostics for DPS, which @IOT rates it "Don\'t disable", '
                                'so Hadron keeps it out of the presets.',
  never=True)

E('WerSvc', 'svc', 'Windows Error Reporting Service',
  'Collects crash and hang reports and sends them to Microsoft when allowed.',
  if_off='Crash reports stop being collected; Reliability Monitor and app crash dialogs show less.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'error reporting; contested')},
  pbasis='reasoning', pbasis_note=P_CONTESTED,
  contested='@WS16 (Windows Server 2016) rates WerSvc "Do not disable" and @IOT (Windows 10/11 IoT Enterprise) rates it "Don\'t disable"; @VDOT '
            'disables it for virtual desktops.')

E('DiagSvc', 'svc', 'Diagnostic Execution Service',
  'Runs diagnostic actions for troubleshooting.',
  if_off='Some troubleshooting actions fail.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'diagnostics; contested')},
  pbasis='reasoning', pbasis_note=P_CONTESTED,
  contested='@IOT rates DiagSvc "Don\'t disable" (Windows 10/11 IoT Enterprise); @VDOT disables it for virtual desktops.')

E('DusmSvc', 'svc', 'Data Usage',
  'Network data usage, data limits and metered-connection handling.',
  if_off='Data usage in Settings and metered-connection limits stop working.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'data usage and metered connections; contested')},
  pbasis='reasoning', pbasis_note=P_CONTESTED,
  contested='@IOT rates DusmSvc "Don\'t disable" (Windows 10/11 IoT Enterprise); @VDOT disables it for virtual desktops.')

E('UnistoreSvc', 'svc', 'User Data Storage (per-user template)',
  'Stores structured user data (contacts, calendars, messages). A per-user service: Hadron changes the template.',
  if_off='Mail, Calendar and People lose their data store.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'user data store for the Mail, Calendar and People apps')},
  pbasis='published', pbasis_note='@IOT rates it "OK to disable" (Windows 10/11 IoT Enterprise); @PERUSER warns that '
                                  'dependent apps might not work correctly and says to test changes.')

E('UserDataSvc', 'svc', 'User Data Access (per-user template)',
  'Gives apps access to structured user data. A per-user service: Hadron changes the template.',
  if_off='Apps lose access to contacts, calendars and messages.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'user data access for the Mail, Calendar and People apps')},
  pbasis='published', pbasis_note='@IOT rates it "OK to disable" (Windows 10/11 IoT Enterprise); @PERUSER warns that '
                                  'dependent apps might not work correctly and says to test changes.')

E('NcbService', 'svc', 'Network Connection Broker',
  'Brokers network connections for Store apps that need to be woken by network traffic.',
  if_off='Store apps that rely on background network triggers can miss notifications.',
  basis='published', basis_note=iot(),
  levels={'minimum': (4, 'network triggers for Store apps')},
  pbasis='published', pbasis_note='@IOT and @WS16 rate it "OK to disable" (Windows 10/11 IoT Enterprise, Windows Server '
                                  '2016).')

E('dot3svc', 'svc', 'Wired AutoConfig',
  'Wired network port authentication (dot1x), used on some company and campus networks.',
  if_off='Wired networks that require port authentication refuse the connection. @IOT says wired networks that '
         'don\'t enforce this authentication (dot1x) are unaffected; home routers normally don\'t enforce it '
         '(reasoning).',
  basis='published', basis_note=iot('@IOT notes that wired port authentication needs it.'),
  levels={'minimum': (4, 'wired port authentication (dot1x); home routers normally don\'t enforce it (reasoning)')},
  pbasis='published', pbasis_note='@IOT rates it "OK to disable" (Windows 10/11 IoT Enterprise). ' + NG_WS16)

# =====================================================================================================
# Drivers with a preset level (sysok): the hardware-absent cases. PnP loads a driver only for devices it finds, and
# a start type other than Disabled does not stop that (@LOADORDER), so Disabled is the only effective preset value.

E('igfxn', 'drv', 'Intel graphics driver',
  'Kernel driver for Intel integrated graphics.',
  if_off='Intel integrated graphics stop working. With no Intel integrated graphics device it is never loaded anyway.',
  basis='reasoning', basis_note='Service name from the Intel graphics driver package; how PnP uses it from @LOADORDER.',
  levels={'stripped': (4, 'no Intel integrated graphics device was ever seen')}, sysok=True, hw='self',
  pbasis='reasoning', pbasis_note='Hardware-absent: no devnode for this driver was ever recorded on this PC.')

E('IntcDAud', 'drv', 'Intel display audio',
  'Audio over HDMI/DisplayPort from Intel integrated graphics.',
  if_off='HDMI/DisplayPort audio from Intel integrated graphics stops. Audio through a discrete GPU uses a different '
         'driver.',
  basis='reasoning', basis_note='Service name from the Intel graphics driver package.',
  levels={'stripped': (4, 'no Intel display audio device was ever seen')}, sysok=True, hw='self',
  pbasis='reasoning', pbasis_note='Hardware-absent: no devnode for this driver was ever recorded on this PC.')

E('HyperVideo', 'drv', 'Hyper-V video',
  'Video driver for the Hyper-V synthetic display, used only inside a Hyper-V virtual machine.',
  if_off='Only a Hyper-V guest uses it.',
  basis='reasoning', basis_note='Part of the Hyper-V guest integration components.',
  levels={'stripped': (4, 'Hyper-V guest display; kept when this Windows is a VM guest')}, sysok=True, hw='vmguest',
  pbasis='reasoning', pbasis_note='Hardware-absent: physical hardware has no Hyper-V synthetic display.')

E('hyperkbd', 'drv', 'Hyper-V keyboard',
  'Keyboard driver for the Hyper-V synthetic keyboard, used only inside a Hyper-V virtual machine.',
  if_off='Only a Hyper-V guest uses it.',
  basis='reasoning', basis_note='Part of the Hyper-V guest integration components.',
  levels={'stripped': (4, 'Hyper-V guest keyboard; kept when this Windows is a VM guest')}, sysok=True, hw='vmguest',
  pbasis='reasoning', pbasis_note='Hardware-absent: physical hardware has no Hyper-V synthetic keyboard.')

E('BTHUSB', 'drv', 'Bluetooth radio USB driver',
  'Driver for USB-attached Bluetooth radios, including the radio on most Wi-Fi/Bluetooth cards.',
  if_off='The Bluetooth radio stops working.',
  basis='reasoning', basis_note='Inbox Bluetooth stack; from how PnP binds it to Bluetooth radios (@LOADORDER).',
  levels={'stripped': (4, 'no Bluetooth device was ever seen')}, sysok=True, hw='class:Bluetooth',
  pbasis='reasoning', pbasis_note='Hardware-absent only; Bluetooth is a hot-plug class, so the row is unticked even then.')

E('Serial', 'drv', 'Serial port driver',
  'Driver for built-in legacy serial (COM) ports. USB serial adapters use a different driver.',
  if_off='Built-in COM ports stop working.',
  basis='reasoning', basis_note='Inbox driver for legacy UART ports.',
  levels={'stripped': (4, 'no built-in serial port was ever seen')}, sysok=True, hw='self',
  pbasis='reasoning', pbasis_note='Hardware-absent: no devnode for this driver was ever recorded on this PC.')

E('luafv', 'drv', 'UAC file virtualization',
  'File-system filter that redirects writes from old programs that write into protected folders such as Program '
  'Files.',
  if_off='Old programs that write into Program Files or Windows without admin rights fail instead of being redirected.',
  test='Test it: run your older games and tools (especially ones that save settings next to their exe) after a restart.',
  basis='reasoning', basis_note='From how User Account Control virtualization works; no Microsoft desktop guidance found.',
  levels={'minimum': (4, 'only old programs without a manifest need it; test it')}, sysok=True,
  pbasis='reasoning', pbasis_note='Not named in the Microsoft sources Hadron checked. Programs with a UAC manifest, '
                                  '64-bit programs and programs run as administrator are never virtualized.')

# =====================================================================================================
# Explanation-only entries (never in a preset). Why each stays on, with the source.

def keep(name, title, expl, why, basis='published', note='', kind='svc'):
    E(name, kind, title, expl, if_off=why, basis=basis, basis_note=note, never=True)


keep('TabletInputService', 'Touch Keyboard and Handwriting Panel Service',
     'Touch keyboard, handwriting and some input-method (IME) features.',
     '@IOT rates it "OK to disable"; Hadron keeps it because the touch keyboard and some IMEs stop without it '
     '(reasoning), and @WS16 rates it "Do not disable" when Desktop Experience is installed.',
     note=iot('@WS16 gives the "Do not disable" rating (Windows Server 2016).'))
keep('WpnService', 'Windows Push Notifications System Service', 'Delivers push notifications to apps and the Action Center.',
     'Notifications stop. @IOT rates it "Don\'t disable".', note=iot())
keep('wlidsvc', 'Microsoft Account Sign-in Assistant', 'Signs in with a Microsoft account.',
     'Without it Windows Update stops offering feature updates, and apps that need a Microsoft account may lose '
     'functionality (@RTLF). The Xbox app and Game Pass sign in with a Microsoft account (reasoning).',
     note='@RTLF (Windows 10/11 Enterprise privacy baseline).')
keep('LicenseManager', 'Windows License Manager Service', 'Licensing for Microsoft Store apps.',
     '@IOT rates it "OK to disable" but says that without it content acquired through the Microsoft Store '
     'doesn\'t function properly; Hadron keeps it for Store apps and games. @RTLF disables it only in its privacy '
     'baseline.',
     note=iot('@RTLF disables it only in its privacy baseline.'))
keep('CaptureService', 'CaptureService (per-user template)', 'Screen capture for apps through Windows.Graphics.Capture.',
     '@IOT rates it "OK to disable"; Hadron keeps it because apps that use Windows.Graphics.Capture (e.g. OBS\'s '
     '\'Windows 10 (1903 and up)\' capture method) can lose optional capture features (reasoning). Test it: '
     'disable it, restart, then record with OBS\'s \'Windows 10 (1903 and up)\' capture and take a Snipping Tool '
     'shot; undo if either fails.',
     basis='reasoning', note='@VDI lists it as a VDI candidate and names the Windows.Graphics.Capture API; the effect on '
                              'capture tools is inferred from that.')
keep('NgcSvc', 'Microsoft Passport', 'Windows Hello and PIN sign-in keys.',
     '@IOT rates it "OK to disable" but says reconfiguring it isn\'t recommended; Hadron keeps it because PIN and '
     'Windows Hello sign-in stop without it. @WS16 says it is "Needed to sign in with PIN and Windows Hello, which '
     'Windows Server doesn\'t support".',
     note='@WS16 (Windows Server 2016 page).')
keep('defragsvc', 'Optimize drives', 'Scheduled drive optimisation, which on an SSD sends TRIM (retrim).',
     '@IOT rates it "OK to disable"; Hadron keeps it because scheduled TRIM through Optimize Drives stops without it '
     '(reasoning).',
     basis='reasoning', note='How Optimize Drives treats SSDs; @IOT gives the rating and @VDI only suggests it for '
                             'virtual desktops.')
keep('WlanSvc', 'WLAN AutoConfig', 'Wi-Fi.', '@IOT says disabling it is not recommended if the computer has a WLAN '
     'adapter.', note=iot())
keep('wuauserv', 'Windows Update', 'Windows Update.', '@IOT rates it "OK to disable"; Hadron keeps it because @RTLF says '
     'Microsoft does not recommend disabling Windows Update, and with it off Windows Update fails with error 0x80070422 '
     '(@WUERR).', note='@IOT, @RTLF and @WUERR.')
keep('UsoSvc', 'Update Orchestrator Service', 'Schedules and runs Windows Update scans and installs.',
     '@WS16 says Windows Update depends on it; @IOT says "Stopping UsoSvc service prevents download and installing '
     'of latest updates" and that Windows Update depends on it.', note=iot())
keep('WaaSMedicSvc', 'Windows Update Medic Service', 'Repairs Windows Update components.',
     '@IOT rates it "Don\'t disable". Windows may refuse this write to administrators; Hadron checks it live and marks '
     'the row.', note=iot())
keep('InstallService', 'Microsoft Store Install Service', 'Installs and updates Store apps.',
     '@IOT rates it "OK to disable"; Hadron keeps it because @VDOTREADME warns that without it the Store cannot update '
     'apps and their dependencies.',
     note='@IOT and @VDOTREADME (Microsoft VDI team tool).')
keep('TrustedInstaller', 'Windows Modules Installer', 'Installs and removes Windows updates and optional components.',
     'Windows updates and optional features stop installing, and Hadron\'s TrustedInstaller toggle needs it. @IOT rates '
     'it "Don\'t disable".', note=iot())
keep('VSS', 'Volume Shadow Copy', 'Shadow copies, which System Restore uses.',
     '@IOT rates it "OK to disable"; Hadron keeps it because System Restore needs it, and restore points are '
     'Microsoft\'s registry recovery path (@REGBACK).',
     note=iot('@REGBACK states the recovery role.'))
keep('WinDefend', 'Microsoft Defender Antivirus Service', 'Microsoft Defender Antivirus.',
     '@DEFCOMPAT says not to disable, stop or modify the Defender services. When Tamper Protection is on, it blocks '
     'registry changes to Defender Antivirus settings (@TAMPER). @PROTSVC: only the protected service itself can '
     'change its configuration (setting it to Auto is among the allowed commands). Hadron\'s TrustedInstaller '
     'toggle is not expected to help (reasoning, not tested).', note='@DEFCOMPAT, @TAMPER and @PROTSVC.')
keep('SecurityHealthService', 'Windows Security Service', 'Windows Security app and health reporting.',
     '@DEFCOMPAT names SecurityHealthService among the services not to disable.', note='@DEFCOMPAT.')
keep('wscsvc', 'Security Center', 'Tracks antivirus and firewall status.',
     '@DEFCOMPAT: without it Defender cannot detect a third-party antivirus and stays active, which is not supported.',
     note='@DEFCOMPAT.')
keep('BrokerInfrastructure', 'Background Tasks Infrastructure Service', 'Runs background tasks for apps.',
     '@STARTMENU: the Start menu is unstable when it is not running.', note='@STARTMENU.')

# Drivers, explanation only.
keep('WdFilter', 'Defender mini-filter', 'Defender Antivirus\'s file-system minifilter.',
     'Defender Antivirus\'s file-system minifilter. @DEFCOMPAT says not to disable or modify the services Defender uses; '
     'this driver is part of that stack (reasoning).',
     basis='reasoning', note='@DEFCOMPAT names the Defender services; that the minifilter belongs to the same stack is '
                             'reasoning.', kind='drv')
keep('CldFlt', 'Cloud Files filter', 'File-system filter for OneDrive Files On-Demand and other cloud files.',
     'Cloud placeholders stop opening.', basis='reasoning', note='From its filter role.', kind='drv')
keep('intelppm', 'Intel processor power driver', 'Processor power management (idle states and frequency).',
     'Disabling it is repeated online as a latency tweak (folklore: no published measurement). Hadron keeps it '
     'because it controls idle states and frequency (reasoning).',
     basis='reasoning', note='Processor power driver; Hadron\'s floor keeps it. Only the latency tweak is folklore, and '
                             'the text labels it.', kind='drv')
keep('pdc', 'Power Dependency Coordinator', 'Coordinates power states for apps and devices.',
     '@STARTMENU: it should not be disabled, or BrokerInfrastructure fails and the Start menu breaks.',
     note='@STARTMENU.', kind='drv')
