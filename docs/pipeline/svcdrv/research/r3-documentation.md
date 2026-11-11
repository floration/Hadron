# svcdrv research 3: primary documentation, Microsoft guidance, known issues, recovery, measurements

Researcher: documentation subagent, 2026-11-11. No repo changes.
Sources are numbered [n]; the full list is at the end. "unsourced" means I looked and found no source.
Supporting data files (scratchpad):
- `/tmp/claude-0/-home-user/a20c3cbf-1292-57a5-9d76-06b15623856d/scratchpad/svcdrv/ws2016-services.tsv`: all 187 rows of Microsoft's WS2016 service guidance, parsed from the raw Learn markdown [12].
- `/tmp/claude-0/-home-user/a20c3cbf-1292-57a5-9d76-06b15623856d/scratchpad/svcdrv/dl/vdot.json`: VDOT `2009/ConfigurationFiles/Services.json` [16].
- `/tmp/claude-0/-home-user/a20c3cbf-1292-57a5-9d76-06b15623856d/scratchpad/svcdrv/dl/*.md`: raw SDK and Win32 doc markdown [1-11].

---

## 0. Findings that change the idea (read first)

1. **The TrustedInstaller toggle cannot reconfigure protected (PPL) services through the SCM.** Microsoft says: "because the system doesn't allow any non-protected process to alter the configuration of a protected service, the call to ChangeServiceConfig2 must be made by the protected service itself" [9]. It also says "other non-protected processes (and even admins) aren't able to stop the service", and that only `sc config start=Auto`, `sc qc`, `sc start`, `sc interrogate` and `sc sdshow` are allowed on an anti-malware protected service [9]. A thread impersonating the TI token is still a non-protected process, so an SCM write to a running PPL service (e.g. WinDefend) should fail. This is `reasoning` from [9]; it has not been tested. The idea's example "protected services such as WinDefend" therefore needs a different mechanism (a direct registry write) or has to be dropped. A direct registry write runs into Tamper Protection (item 2).
2. **Tamper Protection blocks Defender changes, including registry writes.** With it on, "Attempts to modify Microsoft Defender Antivirus settings through the registry are blocked", and Group Policy changes "might appear to succeed, but tamper protection blocks the changes" [21]. Microsoft also says: "Don't disable, stop, or modify any of the associated services that are used by Microsoft Defender Antivirus ... `wscsvc`, `SecurityHealthService`, `MsSense`, `Sense`, `WinDefend`, or `MsMpEng`" [22]. On Win10/11 clients, Defender goes to disabled mode **automatically** when a third-party AV is primary [22]. Recommendation: no preset touches Defender or Security Center. That is `published` guidance [22].
3. **No preset may disable `TrustedInstaller` (Windows Modules Installer).** The TI technique in `modules/runas` needs that service to start. The IoT guidance lists it as "Don't disable" [15]. A Microsoft Q&A answer says Windows "may reconfigure it to run when needed" [27]. The answer is weak: it comes from a Q&A moderator and gives no evidence.
4. **For PnP drivers, start type is not a load switch except `Disabled`.** The PnP manager "loads the drivers for the devices, *regardless* of the drivers' StartType values (except when StartType is SERVICE_DISABLED)" [24]. So in the Drivers tab, "Manual" does not mean "not loaded". The only effective preset action is Disabled, which removes the device's function.
5. **Some drivers are boot-critical even at System or Manual start.** A Microsoft troubleshooting article requires ACPI, DISK, VOLMGR, PARTMGR, VOLSNAP and VOLUME to have Start=0 [29]. It also warns that disabling a Boot/System service that still appears in an `UpperFilters`/`LowerFilters` entry, without removing the filter entry, "causes the computer to crash and generate a 0x7b Stop error" [29]. The PDC driver "shouldn't be disabled or deleted", or BrokerInfrastructure fails and the Start menu breaks [26]. So **the Drivers preset needs a filter-reference check, not just a start-type check.**
6. **Recovery is weaker than the ErrorControl docs imply.**
   - The ErrorControl docs still describe falling back to "LastKnownGood" [4][23].
   - I found no Microsoft primary source that says LKG was removed from the client boot menu in Windows 8+. Only third-party and volunteer sources say so (§4.3).
   - Since Windows 10 1803, the registry is no longer backed up to `RegBack` by default [30].
   - Microsoft's recommended registry recovery is a **System Restore point** [30], and restore points depend on VSS. VDOT leaves VSS "Unchanged" [16], but the IoT page marks VSS "OK to disable" [15]. **Presets must never disable VSS or swprv**, or the recovery path disappears (`reasoning` from [30]).
7. **There are almost no credible published numbers.** §5 shows the best available. None meet rule 7's bar for a user-facing number without strong caveats. The idea's "measure live, no predictions" stance (proxy answer 8) is the only honest option.

---

## 1. API reference (Microsoft Learn / SDK docs)

### 1.1 Enumeration: `EnumServicesStatusExW` [1]
- Needs `SC_MANAGER_ENUMERATE_SERVICE`. Use `InfoLevel` = `SC_ENUM_PROCESS_INFO` (the only defined level), which gives an `ENUM_SERVICE_STATUS_PROCESS[]` array.
- `dwServiceType` values:
  - `SERVICE_DRIVER` (0x0B) = kernel (0x01) + file-system (0x02) drivers;
  - `SERVICE_WIN32` (0x30) = own-process (0x10) + share-process (0x20).
- `dwServiceState`: `SERVICE_STATE_ALL` (0x03).
- Buffer: "The maximum size of this array is 256K bytes." Call with NULL/0, which fails with `ERROR_MORE_DATA` and returns `pcbBytesNeeded`. `lpResumeHandle` continues a partial enumeration; set it to 0 on the first call.
- "If the caller does not have the SERVICE_QUERY_STATUS access right to a service, the service is silently omitted" [1][7]. **Hadron should cross-check the enumeration against `HKLM\SYSTEM\CurrentControlSet\Services` subkeys and flag any gaps** (`reasoning`).
- Per-user service types:
  - The SDK pages above document only 0x01/0x02/0x10/0x20/0x100.
  - Helge Klein reports templates as `0x50` (`SERVICE_USER_OWN_PROCESS`) or `0x60` (`SERVICE_USER_SHARE_PROCESS`), and instances as `0xd0`/`0xe0` (sometimes `0xf0` in `sc queryex`) [19].
  - He also reports: "Many tools and commands, including PowerShell's Get-Service, omit per-user service templates, returning only instances" [19].
  - Microsoft documents only `sc qc <template>` vs `sc qc <template>_<LUID>` [13].
  - Whether `EnumServicesStatusExW` with 0x30 returns templates is **unsourced**; test it on Windows.

### 1.2 Reading config: `QueryServiceConfigW` / `QUERY_SERVICE_CONFIGW` [2][3]
- Needs `SERVICE_QUERY_CONFIG`.
- Start types:
  - `SERVICE_BOOT_START` 0x0: "A device driver started by the system loader. This value is valid only for driver services."
  - `SERVICE_SYSTEM_START` 0x1: "started by the IoInitSystem function", drivers only.
  - `SERVICE_AUTO_START` 0x2.
  - `SERVICE_DEMAND_START` 0x3.
  - `SERVICE_DISABLED` 0x4: "Attempts to start the service result in the error code ERROR_SERVICE_DISABLED" [3].
- ErrorControl [3]:
  - `IGNORE` 0;
  - `NORMAL` 1 (logs, continues);
  - `SEVERE` 2 ("If the last-known-good configuration is being started, the startup operation continues. Otherwise, the system is restarted with the last-known-good configuration");
  - `CRITICAL` 3 ("If the last-known-good configuration is being started, the startup operation fails. Otherwise, the system is restarted with the last-known good configuration").
- Driver-side wording [23]: CRITICAL is set by "*Only* devices/drivers necessary for the system to boot", and failing under LKG means "run a bug-check routine". **Hadron can use ErrorControl=3 as a strong "do not touch" signal** (`reasoning` from [23]).

### 1.3 Optional config: `QueryServiceConfig2W` / `ChangeServiceConfig2W` [4][5]
- Info levels:
  - `SERVICE_CONFIG_DESCRIPTION` 1;
  - `FAILURE_ACTIONS` 2;
  - `DELAYED_AUTO_START_INFO` 3;
  - `FAILURE_ACTIONS_FLAG` 4;
  - `SERVICE_SID_INFO` 5;
  - `REQUIRED_PRIVILEGES_INFO` 6;
  - `PRESHUTDOWN_INFO` 7;
  - `TRIGGER_INFO` 8 ("not supported by the ANSI version");
  - `PREFERRED_NODE` 9;
  - `LAUNCH_PROTECTED` 12 (Windows 8.1+) [5].
- `ChangeServiceConfig2W` needs `SERVICE_CHANGE_CONFIG`, plus `SERVICE_START` if the failure actions include `SC_ACTION_RESTART`. `SC_ACTION_REBOOT` needs `SE_SHUTDOWN_NAME` [5].
- "You cannot set the SERVICE_CONFIG_FAILURE_ACTIONS value for a service that shares the service control manager's process" (services.exe) [5].
- Failure-restart trap: "A queued restart action cannot be canceled ... The service must be explicitly disabled to prevent it from restarting" [5]. **This is relevant to the stop/disable order: disable first, then stop** (`reasoning` from [5]).
- `QueryServiceConfig2W` returns `ERROR_INSUFFICIENT_BUFFER` and writes nothing to the buffer when it is too small [4].

### 1.4 `ChangeServiceConfigW` [2b]
- Needs `SERVICE_CHANGE_CONFIG`. Pass `SERVICE_NO_CHANGE` for every field that is not changing.
- "If the configuration is changed for a service that is running, with the exception of lpDisplayName, the changes do not take effect until the service is stopped."
- Errors: `ERROR_ACCESS_DENIED`, `ERROR_CIRCULAR_DEPENDENCY`, `ERROR_INVALID_SERVICE_ACCOUNT`, `ERROR_SERVICE_MARKED_FOR_DELETE`, among others. Registry errors can also surface.
- Changing `lpServiceStartName` "can cause problems" (SPNs, ACEs). **Hadron should never change the account** (`reasoning` from [2b]).

### 1.5 `SERVICE_DELAYED_AUTO_START_INFO` [6]
- `fDelayedAutostart`: "started after other auto-start services are started plus a short delay"; "ignored unless the service is an auto-start service"; "takes effect the next time the system is started".
- "There is no specific time guarantee." ServiceMain starts at `THREAD_PRIORITY_LOWEST`.
- A delayed service "cannot be a member of a load ordering group".
- So "Auto (Delayed)" in the UI = `SERVICE_AUTO_START` + `fDelayedAutostart=TRUE`. Undo must snapshot both values.

### 1.6 Trigger start [10][11]
- Triggers (Win7 / 2008 R2+) start or stop a service on device arrival, firewall port, domain join, ETW and similar events. "The specified trigger action is taken if the trigger condition is true when the system starts" [10].
- Query with `QueryServiceConfig2W(SERVICE_CONFIG_TRIGGER_INFO)` [10]. The `sc qtriggerinfo` examples show W32Time (domain join) and TabletInputService (HID usages) [10].
- Whether a trigger can start a service whose start type is `SERVICE_DISABLED` is not stated in [10] or [11]. The general rule in [3] (disabled → `ERROR_SERVICE_DISABLED`) suggests not, but that is **unsourced** for triggers specifically.
- The `SERVICE_TRIGGER` remarks [11] describe domain policy restarting BFE. Microsoft's workaround is to disable the service temporarily and then restore the original start type, which is the same "snapshot, then restore" pattern.

### 1.7 `ControlServiceExW` [7]
- Stop needs `SERVICE_STOP`. Errors include `ERROR_DEPENDENT_SERVICES_RUNNING`, `ERROR_SERVICE_CANNOT_ACCEPT_CTRL` and `ERROR_SERVICE_REQUEST_TIMEOUT`.
- "a call to ControlServiceEx blocks for 30 seconds if any service is busy handling a control code". **Run it on a Worker thread, never the UI thread** (CLAUDE.md rule; `published` basis for the blocking [7]).
- "Drivers do not accept control codes other than SERVICE_CONTROL_STOP and SERVICE_CONTROL_INTERROGATE". This is consistent with the proxy decision of no live driver unload.
- Start timeout: "If service does not update its status within 80 seconds, plus the last wait hint, the service control manager determines that the service has stopped responding" [8].

### 1.8 Security and access rights [7a]
- SCM: Administrators get `SC_MANAGER_ALL_ACCESS`; local authenticated users get CONNECT, ENUMERATE, QUERY_LOCK_STATUS and READ.
- Default service DACL: Administrators get `SERVICE_ALL_ACCESS` (plus DELETE, READ_CONTROL, WRITE_DAC, WRITE_OWNER).
- These are **defaults set by CreateService**. Many Windows services ship with tighter DACLs. That is why admin sometimes gets ACCESS_DENIED, and why TI is needed. Which services have TI-only DACLs is **unsourced** in Microsoft docs; read them at runtime with `QueryServiceObjectSecurity`.
- "Granting certain access rights to untrusted users (such as SERVICE_CHANGE_CONFIG or SERVICE_STOP) ... possibly allow them to run applications under the LocalSystem account" [7a].

### 1.9 Protected (anti-malware) services [9]
See §0.1. Other details:
- They were introduced in Windows 8.1 and need an ELAM driver plus signing.
- Admins cannot inject threads or write their memory.
- Debug check: `_EPROCESS.Protection.Type == 1`.
- Hadron can read `SERVICE_CONFIG_LAUNCH_PROTECTED` with `QueryServiceConfig2W` to **detect** a protected service and grey out its controls with an explanation (`reasoning` from [4][9]).

### 1.10 Per-user services [13]
- "When a user signs in to Windows, the OS creates per-user services ... When the user signs out, these services are stopped and deleted."
- Templates live in `HKLM\SYSTEM\CurrentControlSet\Services`. Instances are named `<name>_<LUID>`.
- "Templates aren't shown in the Services console."
- Disable: set the template `Start=4`, and "Windows creates the per-user service in a stopped and disabled state". To prevent creation entirely, set the template's `UserServiceFlags=0` [13]. Deleting the value or setting it to 3 restores the default [19].
- Microsoft's list (24 templates): AarSvc, BluetoothUserService, CaptureService, cbdhsvc, CloudBackupRestoreSvc, CDPUserSvc, ConsentUxUserSvc, PimIndexMaintenanceSvc, CredentialEnrollmentManagerUserSvc, DeviceAssociationBrokerSvc, DevicePickerUserSvc, DevicesFlowUserSvc, BcastDVRUserService, MessagingService, NPSMSvc, P9RdrService, PenService, PrintWorkflowUserSvc, OneSyncSvc, UdkUserSvc, UserDataSvc, UnistoreSvc, webthreatdefusersvc, WpnUserService [13].
- Warnings:
  - "Carefully test any changes";
  - "if you disable the per-user service, there might be dependent apps that don't work correctly";
  - "Don't directly edit the registry unless there's no other alternative" [13].
- **Hadron implication:** a preset acts on the *template*, and the plan should say "takes effect at next sign-in" (`published` [13]).

### 1.11 Driver start types (WDK) [23][24]
- 0 BOOT: "must be used for drivers of devices required for loading the operating system".
- 1 SYSTEM: PnP drivers that do detection at init.
- 2 AUTO: "should never be used in the INF files for WDM or PnP device drivers".
- 3 DEMAND: PnP drivers, loaded when the device is enumerated.
- 4 DISABLED: "can be used to temporarily disable the driver services for a device" [23].
- Load phases: boot start (all 0) → system start (PnP devnode drivers with 1 or 3, then the remaining 1s) → auto start [23][24].
- See §0.4 on PnP drivers ignoring StartType unless Disabled.

---

## 2. Microsoft's own guidance on which services can be disabled

### 2.1 The five Microsoft sources and how strong each is

| Key | Source | OS scope | Strength as a `published` basis for a gaming desktop |
|---|---|---|---|
| WS16 | Security guidelines for disabling system services, WS2016 with Desktop Experience [12] | Server 2016 only. "As of Windows Server 2019, the configurations in this article are actually the default settings" | Medium. Wrong OS, and its "OK" ratings assume no end user (e.g. Audiosrv, wlidsvc, NgcSvc are OK *because* it's a server). It has an explicit category system (Should / OK / Do not / No guidance / Already disabled), and its **"Do not disable"** ratings are useful negatives. |
| IoT | Services on Windows IoT Enterprise [15] | **Windows 10 / 11 IoT Enterprise, LTSC 2021** (same service set as client) | **Strongest client-OS source.** Same categories, aimed at device makers "disabling services that aren't needed for a specific device scenario". "OK to disable" here means "useful to some but not all", so it is **not** a statement that a desktop keeps working. Note: "Other versions of Windows 10/11 might not have the same services available." |
| VDI | Optimizing Windows 10 2004 for VDI [14] | Win10 2004 (19041), VDI, **archived** | Medium-weak. "some services that may be considered to disable in virtual desktop environments", "suggested optimization settings only", "intentionally avoids touching system services ... that affect security". Its strongest line: "some services are not in the list because they cannot be disabled in a supported manner." |
| VDOT | Virtual-Desktop-Optimization-Tool `2009/Services.json` [16][17] | Win10 20H2+ / Win11, VDI/AVD. Repo superseded by WDOT [17] | Medium-weak. Built by Microsoft's VDI team with "Input ... from the Microsoft Windows and Azure Virtual Desktop product groups", but "It is important to test the optimization settings in each respective environment" [17]. Exactly 20 services marked Disabled (§2.3). |
| RTLF | Restricted Traffic Limited Functionality Baseline [18] | Win10 Enterprise 1607+, Win11 Enterprise | Strong, but narrow: privacy-traffic only. Sets `Start=4` for **LicenseManager** and **wlidsvc** only. Warns that wlidsvc off means "Windows Update will no longer offer feature updates to devices running Windows 10 1709 or higher and Windows 11"; also "we do not recommend disabling" Windows Update, root-cert update or Defender. |
| DEF | Defender AV compatibility [22] | Win10/11 | Strong, negative: do not touch Defender or Security Center services (§0.2). |
| START | Start menu troubleshooting [26] | Win10/11 | Strong, negative: BrokerInfrastructure must be Auto; the PDC driver must not be disabled. |

The 1-idea's "Windows 10 VDI optimization guidance" is VDI [14] plus VDOT [16]. The 2016 server guidance is WS16 [12]. I could not find a separate Microsoft "safe to disable on Windows 10/11 client" page beyond IoT [15]. **unsourced:** any Microsoft list aimed at consumer or gaming desktops.

### 2.2 Cross-reference matrix (desktop-relevant services)

Legend:
- **S** = Should be disabled; **OK** = OK to disable; **DND** = Do not disable; **NG** = No guidance; **AD** = Already disabled (on that OS);
- **L** = listed as a VDI candidate; **D** = VDOT `VDIState: Disabled`; **U** = VDOT Unchanged; **4** = RTLF sets Start=4;
- `-` = not mentioned.

| Service | WS16 [12] | IoT [15] | VDI [14] | VDOT [16] | Other | Proposed preset tier (`reasoning`) |
|---|---|---|---|---|---|---|
| XblAuthManager | **S** | **S** | L | D | | Bloat, but the owner games: Xbox app / Game Pass sign-in needs it (reasoning) → opt-in |
| XblGameSave | **S** ("game save data will not upload to or download from Xbox Live") | **S** | L | D | | Same as above |
| XboxNetApiSvc | - | **S** | L | D | | Same as above |
| XboxGipSvc (Xbox Accessory Mgmt) | - | **S** | L | D | | Caution: Xbox controllers (reasoning) |
| BcastDVRUserService (template) | - | OK | L | D | | Bloat |
| DiagTrack | NG | OK | L ("Consider disabling if on disconnected network") | D | | Bloat (telemetry) |
| dmwappushservice | OK ("required on client devices for Intune, MDM") | NG | - | - | | Bloat |
| MapsBroker | OK | OK | L | D | | Bloat |
| lfsvc | OK | OK | L | D | | Bloat (breaks location apps) |
| RetailDemo | - | OK | - | - | | Bloat |
| WMPNetworkSvc | - | OK | - | - | | Bloat |
| MessagingService / SmsRouter / SEMgrSvc / autotimesvc | - | OK | L | D | | Bloat (cellular / NFC hardware absent) |
| PhoneSvc / WalletService | OK | OK | - | - | | Bloat |
| icssvc (mobile hotspot) | OK | OK | L | D | | Bloat |
| SharedAccess (ICS) | OK ("Required for ... WiFi hotspots and both ends of Miracast") | OK | - | - | | Stripped |
| RmSvc | OK | OK | - | D | | Stripped (airplane mode) |
| wisvc (Insider) | OK | OK | - | - | | Bloat |
| PcaSvc | OK | OK | - | - | | Stripped |
| MapsBroker, lfsvc, SensorService, SensrSvc, SensorDataService | OK | OK | - | - | | Stripped if no sensors (detect) |
| Spooler / PrintNotify | OK if not print server | OK ("you aren't able to print") | - | - | | Stripped if no printer |
| bthserv / BTAGService / BthAvctpSvc / BluetoothUserService | OK | OK | - | - | | Stripped if no BT radio (detect) |
| SSDPSRV / upnphost / lltdsvc / NcbService / QWAVE | OK | OK | - | - | | Stripped |
| stisvc / WiaRpc | OK | OK | - | - | | Stripped if no scanner/camera |
| FrameServer | OK | OK | - | - | | Stripped if no camera |
| ShellHWDetection | OK | OK | - | - | | Stripped (kills AutoPlay) |
| ScDeviceEnum / SCardSvr / SCPolicySvc | OK / AD / NG | OK | - | - | | Stripped if no smart card |
| CaptureService (template) | - | OK | L ("Windows.Graphics.Capture API") | - | | **Not** Bloat: breaks WGC capture used by Snipping Tool / OBS WGC mode (reasoning) |
| CDPSvc / CDPUserSvc | NG / OK | OK | L | - | | Stripped (Phone Link, cross-device) |
| OneSyncSvc / PimIndexMaintenanceSvc / UnistoreSvc / UserDataSvc | OK (templates) | OK | L ("Mail and other UWP apps will not work properly") | - | | Stripped |
| NgcSvc / NgcCtnrSvc | OK (server: "Windows Hello, which Windows Server doesn't support") | OK | - | - | | **Never** on a desktop with PIN sign-in (reasoning from WS16 comment) |
| wlidsvc | OK (server) | OK | - | - | **4** RTLF, with feature-update warning | Minimum only, with warning |
| LicenseManager | NG | OK | - | - | **4** RTLF | Minimum only (Store licensing) |
| WSearch | AD (server) | OK | L ("Test before disabling") | - | | Stripped (breaks Start/Explorer search) |
| defragsvc | NG | OK | L (VDI only) | U | | Never (SSD TRIM via Optimize Drives: reasoning) |
| SysMain | NG | **DND** | L ("evaluate in each environment") | D | | **Conflict**: no preset; test-it item (§5) |
| DPS / WdiSystemHost / WdiServiceHost | NG | **DND** (DPS, WdiSystemHost) | L | D | | **Conflict**: Minimum only, if at all |
| WerSvc | **DND** | **DND** | L | D | | **Conflict**: two DND vs VDI-only disable; leave |
| DiagSvc | - | **DND** | L | D | | Conflict; leave |
| DusmSvc | - | **DND** | L | D | | Conflict; leave (metered networks) |
| TabletInputService | **DND** ("if Desktop Experience is installed") | OK | - | - | | Conflict; leave (touch keyboard, IME: reasoning) |
| Themes | **DND** ("Can't set accessibility themes") | OK | - | - | | Leave |
| WpnService / WpnUserService | OK ("Needed for live tiles and other features") | **DND** | - | - | | Leave (notifications) |
| Audiosrv / AudioEndpointBuilder | OK (server) | OK | - | - | | **Never** (a gaming PC needs audio) |
| WlanSvc | - | OK ("isn't recommended if your computer has a WLAN adapter") | - | - | | Never if a WLAN adapter is present (owner's board has Wi-Fi) |
| HvHost / vmic* | **DND** | OK | - | - | | Stripped only if no Hyper-V / VBS (detect) |
| wuauserv | NG | OK | - | - | RTLF: "we do not recommend disabling" WU | Never in presets |
| UsoSvc | **DND** ("Windows Update ... depends on this service") | **DND** | L (with test caveat) | - | | Never |
| WaaSMedicSvc | - | **DND** | - | - | | Never (and admin can't, §3.1) |
| InstallService | - | OK | L (non-persistent VDI only) | U ("should not be disabled") [17] | | Never |
| TrustedInstaller | NG | **DND** | - | - | | Never (needed by the TI toggle) |
| BrokerInfrastructure | NG | **DND** | - | - | START: must be Automatic | Never |
| StateRepository / AppReadiness / SystemEventsBroker / TimeBrokerSvc | NG / DND / DND / DND | DND | - | - | | Never |
| tiledatamodelsvc | **DND** ("Start menu breaks") | - | - | - | | Never (may not exist on 22H2: unsourced) |
| WinDefend / WdNisSvc / Sense / wscsvc / SecurityHealthService / SgrmBroker | NG | **DND** | - | - | DEF: "Don't disable, stop, or modify" | Never |
| VSS / swprv | NG | OK | L (warns: "shadow copies will be unavailable") | U | | **Never** (System Restore is MS's recovery path [30]) |
| Power | NG | - | L (VMs only) | - | | Never on physical hardware |
| BFE / mpssvc / Dhcp / Dnscache / nsi / RpcSs / RpcEptMapper / DcomLaunch / LSM / ProfSvc / Schedule / EventLog / CryptSvc / Winmgmt / PlugPlay | NG or DND | DND or NG | - | - | | Never (boot, network, logon) |

Observations:
- Only **4 services** (the four Xbox ones) carry a **Should be disabled** rating in any Microsoft source [12][15], and those ratings assume enterprise use, not a gamer.
- Microsoft's sources **contradict each other** on SysMain, DPS/WDI, WerSvc, DiagSvc, DusmSvc, TabletInputService, Themes, WpnService, wuauserv and VSS. Rule 7 forces Hadron to show the conflict, not pick a side silently.
- VDOT's full Disabled set (20 services) [16]: autotimesvc, BcastDVRUserService, DiagSvc, DiagTrack, DPS, DusmSvc, icssvc, lfsvc, MapsBroker, MessagingService, RmSvc, SEMgrSvc, SmsRouter, SysMain, WdiSystemHost, WerSvc, XblAuthManager, XblGameSave, XboxGipSvc, XboxNetApiSvc. Unchanged: InstallService, defragsvc, VSS.

### 2.3 Microsoft guidance on drivers
No Microsoft page lists drivers that are safe to disable (**unsourced**). The only published Microsoft driver constraints I found:
- ACPI, DISK, VOLMGR, PARTMGR, VOLSNAP, VOLUME must be Start=0 [29].
- Disabling a 0/1 service that is still in an Upper/LowerFilters entry → 0x7B [29].
- PDC must not be disabled [26].
- Drivers with ErrorControl=3 are by definition "necessary for the system to boot" [23].

Everything else in a driver preset would rest on `reasoning` (hardware absent, e.g. a disabled Bluetooth / Hyper-V / printer stack), or on `folklore`.

---

## 3. Known issues

### 3.1 Services that Windows reverts or protects
- **WaaSMedicSvc:**
  - Its own description string is "Enables remediation and protection of Windows Update components". That string is visible in services.msc, but I found no Learn page that documents it (**unsourced** on Learn).
  - Third-party reports say its registry key denies admins write access (e.g. [32]): `folklore`/community.
  - Whether it re-enables wuauserv/UsoSvc is widely reported but **no Microsoft primary source found**. Microsoft's "How Windows Update works" does not mention Medic [33].
- **wuauserv reverting:** forum reports (e.g. [34]) give no mechanism.
- **TrustedInstaller reverting:** a Microsoft Q&A moderator says "scheduled tasks or update processes may reconfigure it" [27]. Weak.
- **Defender:**
  - Not reverted as such, but protected: see §0.1–0.2 [9][21][22].
  - Community: `sc stop WinDefend` fails with access denied while Defender is primary [35].
  - Admins cannot change Defender via the registry when Tamper Protection is on [21].
- **Design implication:** the idea's "detect and show drift" is the right approach. Hadron can't claim a cause unless one is published, so label drift as observed, not explained.

### 3.2 Breakage from disabled services (published)
- **Start menu:**
  - BrokerInfrastructure not running → Start menu unstable; PDC driver disabled → same [26].
  - WS16: tiledatamodelsvc "Start menu breaks" [12].
  - VDOT forum: a Win11 23H2 user reported Start crashes after VDOT (community) [20].
- **Store:**
  - VDOT README: disabling InstallService means "the Store components in Windows cannot update UWP apps and dependencies, which can lead to vulnerabilities" [17].
  - RTLF: removing wlidsvc breaks "Many apps and system components that depend on Microsoft Account authentication" [18].
- **Windows Update:**
  - UsoSvc: "Windows Update (incl. WSUS) depends on this service" [12][15].
  - Error 0x80070422 = ERROR_SERVICE_DISABLED when the WU service isn't running [28].
  - wlidsvc off → no feature updates [18].
- **Wi-Fi:** "Disabling WLANSVC isn't recommended if your computer has a WLAN adapter" [15]. Wired 802.1X needs dot3svc [15].
- **Hotspot / Miracast:** SharedAccess is "Required for clients used as WiFi hotspots and both ends of Miracast projection" [12].
- **Search:** "Test before disabling this service" (WSearch) [14].
- **Mail / People / Calendar:** OneSyncSvc off → "Mail and other UWP apps will not work properly" [14].
- **Defender vs. third-party AV:** disabling the Security Center service means Defender "can't detect non-Microsoft antivirus installations and will stay Active ... not supported" [22].
- **Boot:**
  - Filter-driver mismatch → 0x7B [29].
  - Boot drivers must be Start=0 [29].
- **Snip & Sketch broke after disabling Xbox items:** one community report on the VDOT thread [20]. Weak.

### 3.3 Windows 10 22H2 vs Windows 11 23H2/24H2 service lists
- Win10 22H2 is the last Win10 release, and support ended 2025-10-14 [36].
- Microsoft publishes **no per-build service inventory** (**unsourced**).
- Microsoft's per-user list includes items that are not on Win10, e.g. `webthreatdefusersvc` (Web Threat Defense, a Win11 feature) and `CloudBackupRestoreSvc` [13]. Microsoft only says "Other versions of Windows 10/11 might not have the same services available" [13][15].
- Helge Klein's template list was taken on Win10 20H2 [19].
- **Recommendation:** key the stock table by build. Generate it by enumerating a clean install of each target build (Win10 22H2 19045 first) in a VM, recording Start, DelayedAutostart, Type, ErrorControl and triggers. Basis: `owner-measured` / `measured on clean VM, build X`, not `published`.

---

## 4. Recovery

### 4.1 WinRE
- WinRE starts automatically after "Two consecutive failed attempts to start Windows", two unexpected shutdowns within two minutes of boot completion, or two consecutive reboots within two minutes of boot completion, among other triggers [37].
- It provides "Automatic repair and other troubleshooting tools", "Point-in-time restore" (restore points) and push-button reset [37].
- With BitLocker, "encrypted files won't be accessible unless the user has the key" [37]. **The recovery .zip instructions must say: have the BitLocker recovery key first, if BitLocker is on** (`reasoning` from [37]).
- Command Prompt: the system volume's drive letter is found with `bcdedit` (the `osdevice` line), "For example, D:" [38]. **The recovery script must not assume `C:`** (`published` [38]).

### 4.2 Offline registry editing
- Regedit: select HKLM → File → Load Hive → `<OSdrive>:\Windows\System32\config\SYSTEM`, name it `OfflineSystemHive` [29].
- Read `Select\Default` and open `ControlSet00<n>` accordingly [29]. (Microsoft's own text has a typo, naming the hive both `OfflineSystemHive` and `OfflineHive` [29].)
- Microsoft's advanced boot article edits `ControlSet001\Services\...` directly [31].
- `reg load` syntax: `reg load <keyname> <filename>`, returning 0 on success and 1 on failure [39]. The page claims the file "must have a .hiv extension" and must come from `reg save` [39]. That contradicts [29] and [31], which load the extension-less `SYSTEM` hive. Practice (unsourced) is that `reg load HKLM\Offline D:\Windows\System32\config\SYSTEM` works. **Test it in WinRE before shipping.**
- **Script design (reasoning):**
  - resolve the OS drive by looking for `\Windows\System32\config\SYSTEM` on C..Z;
  - `reg load`;
  - read `Select\Current` or `Default`;
  - write the snapshot's Start values into `ControlSet00n\Services\<name>\Start`;
  - `reg unload`.
  - Admin checks don't apply in WinRE (it runs as SYSTEM), and CLAUDE.md's `fltmc` rule is for the online case.
  - Ship it in a .zip (rule 6).

### 4.3 `Select` and control sets
- `Select\Current` "Identifies the control set from which the CurrentControlSet subkey is derived"; `Default` is "the default control set"; `Failed` is the set "last rejected and replaced with a LastKnownGood control set"; `LastKnownGood` is "the last control set that successfully started the system" [40]. (NT4-era KB mirror. Old, but these value meanings are what [29] still relies on.)
- **Offline there is no `CurrentControlSet`.** It is a runtime link, which is why [29] reads `Select\Default` (`published` [29]).

### 4.4 Last Known Good in Windows 8+
- The ErrorControl docs still describe LKG fallback [3][23].
- On client Windows 8+ the LKG boot option is not offered by default. Re-enabling it needs `...\Session Manager\Configuration Manager\BackupCount=1` and `...\Configuration Manager\LastKnownGood\Enabled=1` [41] (ITmedia @IT; third-party, `folklore`-grade until tested).
- Microsoft Q&A volunteers say LKG "has been removed" / "deprecated in Windows 10" [42]. These are not Microsoft staff.
- **I found no Microsoft primary statement of the removal (unsourced).**
- Separately, and **published**: the registry is not backed up to `RegBack` since Win10 1803; "Microsoft recommends that you use a system restore point"; `EnablePeriodicBackup=1` restores the old behaviour [30].
- **Recommendation:**
  - The Hadron snapshot plus offline script is the primary recovery path.
  - Optionally create a System Restore point before a preset. That needs VSS and System Protection enabled, which is outside the scope of this research.
  - Never rely on LKG.

---

## 5. Published numbers on RAM / boot savings

| Claim | Source | Conditions | Strength |
|---|---|---|---|
| VDOT let LoginVSI gain "over 100 users" | Microsoft Tech Community announcement [20] | Partner's internal lab, Win10 2004 single-session; no baseline user count, no host spec | Weak: vendor-reported, unspecified baseline; not per-service; VDI density, not desktop RAM |
| SysMain off: restart 42.83 s → 35.18 s; ClipChamp launch 3.14 s → 8.48 s (worse); cached RAM about 100 MB lower; "little to no change" on a 16 GB PC | Windows Latest, 2026-10-26 [43] | 4 GB Win11 VM; build, CPU, storage and run count not stated | Weak: single runs, VM, missing details. It does show the effect can be **negative** (app launch got slower) |
| Idle RAM about 4 GB stock → 2 GB AtlasOS | How-To Geek [44] | Probably a VirtualBox VM; "right after the first boot"; no method | Very weak: whole-OS mod (not services alone), no method |
| Removing the WMP package frees "about 60 MB disk space" | Microsoft VDI guide [14] | No conditions | Disk, not RAM; not a service |
| VDOT thread/handle counts dropped "noticeably" | [20] | No figures | Not a number |
| Microsoft IoT: disabling services can "reduce resource overhead" | [15] | No figures | Qualitative only |

**Conclusion:** I found **no** published, controlled measurement of RAM or boot savings from disabling services on a physical Win10/11 desktop. Under rule 7, Hadron should:
- show only live before/after measurements on the owner's PC, labelled as measured, with conditions (build, uptime since boot, same idle wait);
- give SysMain and similar items a "test it" note: e.g. reboot ×3 each way, measure boot with the Event Log `Microsoft-Windows-Diagnostics-Performance/Operational` event 100 BootTime (**unsourced** here; verify), and measure a cold app launch.

---

## 6. Suggested basis labels for the KB (summary)
- `published` (Microsoft, client OS): the IoT ratings [15], with the caveat "for device makers; OK = useful to some". The per-user mechanics [13]. The Defender, Start and Update negatives [12][15][22][26][28].
- `published` (Microsoft, other OS): WS16 [12] and VDI/VDOT [14][16]. The UI must show the OS and scenario ("Server 2016", "VDI").
- `reasoning`: hardware-absent rules (no BT radio → BT stack); "Xbox services needed by Game Pass"; "CaptureService used by OBS WGC"; VSS needed for restore points.
- `folklore`: WaaSMedic reverting services; LKG re-enable keys; "disabling X saves RAM" with no measurement.

---

## Sources
1. EnumServicesStatusExW: https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexw (raw: https://raw.githubusercontent.com/MicrosoftDocs/sdk-api/docs/sdk-api-src/content/winsvc/nf-winsvc-enumservicesstatusexw.md)
2. QueryServiceConfigW: https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-queryserviceconfigw
2b. ChangeServiceConfigW: https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-changeserviceconfigw
3. QUERY_SERVICE_CONFIGW: https://learn.microsoft.com/windows/win32/api/winsvc/ns-winsvc-query_service_configw (raw sdk-api `ns-winsvc-query_service_configw.md`)
4. QueryServiceConfig2W: https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-queryserviceconfig2w (raw sdk-api)
5. ChangeServiceConfig2W: https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-changeserviceconfig2w
6. SERVICE_DELAYED_AUTO_START_INFO: https://learn.microsoft.com/windows/win32/api/winsvc/ns-winsvc-service_delayed_auto_start_info (raw sdk-api)
7. ControlServiceExW: https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-controlserviceexw (raw sdk-api)
7a. Service Security and Access Rights: https://learn.microsoft.com/en-us/windows/win32/services/service-security-and-access-rights (raw: https://raw.githubusercontent.com/MicrosoftDocs/win32/docs/desktop-src/Services/service-security-and-access-rights.md)
8. Service Startup: https://learn.microsoft.com/en-us/windows/win32/services/service-startup (raw win32 `service-startup.md`)
9. Protecting anti-malware services: https://learn.microsoft.com/en-us/windows/desktop/Services/protecting-anti-malware-services- (raw win32 `protecting-anti-malware-services-.md`)
10. Service Trigger Events: https://learn.microsoft.com/en-us/windows/win32/services/service-trigger-events (raw win32 `service-trigger-events.md`)
11. SERVICE_TRIGGER: https://learn.microsoft.com/windows/win32/api/winsvc/ns-winsvc-service_trigger (raw sdk-api)
12. Guidance on disabling system services on Windows Server 2016 with Desktop Experience: https://learn.microsoft.com/en-us/windows-server/security/windows-services/security-guidelines-for-disabling-system-services-in-windows-server (raw: https://raw.githubusercontent.com/MicrosoftDocs/windowsserverdocs/main/WindowsServerDocs/security/windows-services/security-guidelines-for-disabling-system-services-in-windows-server.md, ms.date 08/22/2024); original blog: https://techcommunity.microsoft.com/t5/microsoft-security-baselines/guidance-on-disabling-system-services-on-windows-server-2016/ba-p/701058
13. Per-user services in Windows: https://learn.microsoft.com/en-us/windows/application-management/per-user-services-in-windows
14. Optimizing Windows 10, version 2004 for VDI: https://learn.microsoft.com/en-us/windows-server/remote/remote-desktop-services/rds-vdi-recommendations-2004
15. Services on Windows IoT Enterprise (optimize): https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/optimize/services
16. VDOT Services.json: https://raw.githubusercontent.com/The-Virtual-Desktop-Team/Virtual-Desktop-Optimization-Tool/main/2009/ConfigurationFiles/Services.json
17. VDOT README (migration to WDOT, InstallService warning, testing caveat): https://github.com/The-Virtual-Desktop-Team/Virtual-Desktop-Optimization-Tool (raw README.md); WDOT: https://github.com/The-Virtual-Desktop-Team/Windows-Desktop-Optimization-Tool
18. Manage connections from Windows 10/11 components to Microsoft services (RTLF baseline): https://learn.microsoft.com/en-us/windows/privacy/manage-connections-from-windows-operating-system-components-to-microsoft-services
19. Helge Klein, Per-User Services in Windows: https://helgeklein.com/blog/per-user-services-in-windows-info-and-configuration/
20. Microsoft Tech Community, (A)VD Optimization Tool announcement and replies: https://techcommunity.microsoft.com/t5/azure-virtual-desktop/azure-virtual-desktop-optimization-tool-now-available/m-p/1558614
21. Tamper protection overview: https://learn.microsoft.com/en-au/defender-endpoint/tamper-protection-overview ; FAQ: https://learn.microsoft.com/en-au/defender-endpoint/faqs-on-tamper-protection
22. Microsoft Defender Antivirus compatibility: https://learn.microsoft.com/en-us/defender-endpoint/microsoft-defender-antivirus-compatibility
23. INF AddService directive: https://learn.microsoft.com/en-au/WINDOWS-HARDWARE/DRIVERS/install/inf-addservice-directive
24. Specifying driver load order: https://learn.microsoft.com/en-in/windows-hardware/drivers/install/specifying-driver-load-order
26. Start menu troubleshooting guidance: https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/troubleshoot-start-menu-errors
27. Microsoft Q&A, Windows Module Installer re-enables itself: https://learn.microsoft.com/en-gb/answers/questions/2237995/windows-module-installer-kept-enabled-itself-after
28. Windows Update common errors (0x80070422): https://learn.microsoft.com/en-us/troubleshoot/windows-client/deployment/common-windows-update-errors
29. Stop error 7B / inaccessible boot device troubleshooting: https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/stop-error-7b-or-inaccessible-boot-device-troubleshooting
30. Registry no longer backed up to RegBack from Win10 1803: https://learn.microsoft.com/en-us/troubleshoot/windows-client/deployment/system-registry-no-backed-up-regback-folder
31. Advanced troubleshooting for Windows boot problems: https://learn.microsoft.com/en-us/windows/client-management/advanced-troubleshooting-boot-problems
32. privacylearn (privacy.sexy-derived) WaaSMedicSvc page, community: https://privacylearn.com/windows/privacy-over-security/disable-automatic-updates/disable-windows-update-services/disable-windows-update-medic-service-waasmedicsvc
33. How Windows Update works: https://learn.microsoft.com/en-us/windows/deployment/update/how-windows-update-works
34. ElevenForum, Windows Update service reverts: https://www.elevenforum.com/t/windows-update-service-disabled-then-it-reverts-back-to-default-settings-automatically.4158/
35. Core Technologies, WinDefend service: https://www.coretechnologies.com/blog/windows-services/windefend/
36. Windows 10 22H2 end of support: https://learn.microsoft.com/en-us/lifecycle/announcements/windows-10-22h2-end-of-support-update
37. Windows RE technical reference: https://learn.microsoft.com/en-au/windows-hardware/manufacture/desktop/windows-recovery-environment--windows-re--technical-reference?view=windows-11
38. Use WinRE to troubleshoot startup issues: https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/use-winre-to-troubleshoot-startup-issue
39. reg load: https://learn.microsoft.com/windows-server/administration/windows-commands/reg-load
40. KB Q102984, ControlSet\Select subkey entries (mirror): https://techshelps.github.io/MSDN/KB/Source/winnt/q102984.htm
41. ITmedia @IT, Last Known Good removed from Windows (JP): https://atmarkit.itmedia.co.jp/ait/articles/2111/10/news005.html
42. Microsoft Q&A, "Restore a previous Known Good configuration" (volunteer answers): https://learn.microsoft.com/en-us/answers/questions/3183902/restore-a-previous-known-good-configuration
43. Windows Latest, SysMain benchmark (2026-10-26): https://www.windowslatest.com/2026/10/26/everyone-says-disabling-sysmain-speeds-up-windows-11-so-i-benchmarked-it-on-two-pcs-k4l9/
44. How-To Geek, lean Windows 11 (AtlasOS): https://www.howtogeek.com/i-tried-using-a-sane-and-lean-version-of-windows-11-heres-what-surprised-me/
