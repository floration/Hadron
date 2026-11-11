# svcdrv Finalize: KB builder

Branch `finalize/svcdrv-kb`, from fdd5c31 (`module/services-svcdrv`). Scope: the category 6 claims that live in
`tools/services/kb.py`, the genkb number gate (kb-audit F7), the proxy Q5 expansion, and the spec doc errors from
6-review.md "Docs". Code under `modules/services/` was not touched (two other builders own it).

Result: `python3 -I tools/services/genkb.py --selftest` PASS, 0 failed (38 must-fail cases, 15 ok texts, one warning
case, and the committed KB: 265 entries, 26 fields per K line, every @KEY expanded). `genkb.py <tmp>` then `cmp`
against `modules/services/data/services.db`: identical. 265 entries; direct levels bloat 17, stripped 48, minimum 11
(unchanged); 189 explanation-only (was 23); 0 warnings.

## 1. Category 6 items in KB text
| kb-audit | Entry | What changed |
|---|---|---|
| F3 | `P_STRIP_IOT`, `P_STRIP_IOT_WS16` | "Hadron adds it only where the hardware or use is absent" is gone. The notes now quote the IoT legend ("useful to some but not all enterprises"; it said "devices") and say Hadron does not detect use: with a hardware hint the item is kept when the hardware is present or was seen; without one Stripped offers it unticked and Minimum ticks it (proxy Q1). The section comment says the same. |
| F4 | `P_CONTESTED` (SysMain, DPS, WdiSystemHost, WerSvc, DiagSvc, DusmSvc) | `pbasis` is `reasoning` now. Text: "Microsoft sources disagree (see Contested): @VDOT disables it for virtual desktops. Hadron puts it in Minimum because Windows boots, networks and runs games without it (reasoning); whether that helps is not known: test it." The ratings stay `published` with URLs in `contested`. |
| F5 | SysMain | The memory-compression/"placement" sentence is removed. The description quotes @IOT ("Maintains and improves system performance over time"). "can be negative" is now "Whether turning it off helps or hurts depends on the workload, and Hadron has no measurement either way (reasoning: it preloads for your usage pattern, so cold launches can get slower): test it." The basis note says the summary is reasoning. |
| F6 | intelppm | `basis` is `reasoning`. Text: "Disabling it is repeated online as a latency tweak (folklore: no published measurement). Hadron keeps it because it controls idle states and frequency (reasoning)." |
| F10 | XboxGipSvc | `hw='svc:xboxgip'` (the accessory's function driver) instead of the unsourced `class:XnaComposite`; why and pbasis say so. On the fixture the row is still "Kept" (the controller's devnode has service xboxgip); only the evidence text changes. |
| F13 (KB part) | WinDefend | "@PROTSVC: a protected service can only be reconfigured by itself" is now "only the protected service itself can change its configuration (setting it to Auto is among the allowed commands). Hadron's TrustedInstaller toggle is not expected to help (reasoning, not tested)." |
| F16 | RetailDemo | "Nothing else uses it." is now "Hadron knows of nothing else that uses it (reasoning)." |
| F16 | MessagingService | "On a desktop without a modem Hadron knows of nothing that uses it (reasoning)." |
| F16 | dot3svc | "@IOT says wired networks that don't enforce this authentication (dot1x) are unaffected; home routers normally don't enforce it (reasoning)." The Minimum why says the same. (The gate reads "802.1X" as a number with unit x, so the text says dot1x.) |
| F16 | CaptureService | The keep text now says how to test: disable, restart, record with OBS's "Windows 10 (1903 and up)" capture and take a Snipping Tool shot; undo if either fails. |
| F16 | SSDPSRV | "test online play" became a `test` field: note the game's NAT type (or the router's UPnP port list) before and after a restart with it disabled; undo if it gets stricter or the ports no longer appear. The UPnP-port effect is labelled reasoning. |
| F16 | RasAuto | The test moved out of the pbasis into `test`: connect the built-in Windows VPN from Settings after a restart and open something that needs it. "VPN apps with their own client are unaffected" is labelled reasoning. |
| F16 | UsoSvc | "@WS16 says Windows Update depends on it; @IOT says "Stopping UsoSvc service prevents download and installing of latest updates" and that Windows Update depends on it." (Read 2026-11-11: the IoT page does say "Windows Update (incl. WSUS) depends on this service", so kb-audit's "not depends on" is out of date; both are quoted now.) |
| F16 (optional) | LicenseManager, NgcSvc | Cite @IOT for the keep reason: Store content "doesn't function properly" without LicenseManager; reconfiguring NgcSvc "isn't recommended". NgcSvc quotes @WS16 exactly. |
| Soft claims | lfsvc, RmSvc, SEMgrSvc, the three sensor services | "a desktop rarely needs it", "a desktop rarely uses it", "Desktops rarely have NFC", "Desktops usually have no such sensors" carry "(reasoning)". |
| Quotes | every rating | Ratings are quoted with each page's own label: @IOT "Don't disable" (the KB said "Do not disable", which is @WS16's label), "OK to disable", "Should be disabled", "Already disabled", "No guidance". `NG_WS16` reads '@WS16 rates it "No guidance" (Windows Server 2016), which Hadron reads as keep the default.' dmwappushservice quotes the IoT legend for "No guidance". |
| Raw URL | `SOURCES['VDOT']` | The raw.githubusercontent.com JSON link is now the GitHub page of the same file (fetched 2026-11-11: 20 Disabled; InstallService, defragsvc and VSS Unchanged, as r3 and kb-audit recorded). |

"Two soft claims need a `reasoning` label": "a desktop rarely needs it" is done above. "Windows stops it on its own when
idle" is in `module.cpp:1515` (the TI banner), not in the KB: left to the module builders.

### Category 6 items not in KB text (not done here: module/plan/recovery code)
F1 and F2 (TI banner and Why text; Hadron-column legend, module.cpp), F8 ("Windows stock" chip, being done per Q2),
F9 (Drivers bloat note, plan.cpp:269), F11 (Company column label), F12 (Console help header), F13's UI strings
(module.cpp:1817, TI caption), F14 and F16's README A (recovery README), F15 (restore.cmd `:nowin`), and the
`--readonly` banner text.

## 2. genkb number gate (kb-audit F7)
`tools/services/genkb.py` now runs `check_quantities` on every text field and every basis, pbasis, stock and
contested note, on top of the bios unit check. It fails on:
- counts: a whole number or number word before processes, services, drivers, threads, handles, instances, tasks or
  svchosts ("40 processes", "30 services", "12 threads", "forty fewer processes", "twenty-five background services");
- sizes and durations in words ("200 megabytes", "two hundred megabytes", "two seconds", "ten percent");
- fractions ("half", "a third", "a quarter", ...).

Not counted: version and build numbers ("USB Audio 2.0 driver", "Windows Server 2016 services", "Windows 10/11
services"), "one" on its own ("one instance per user"), "a third-party", "three times" (left to the bios soft
check). A `[[shown|published|@KEY]]` marker passes. The bios soft list is now reported as `genkb: warning:` lines
(it was never passed a warnings list); warnings don't fail. Selftest: 16 new must-fail cases (one per form, plus a
count in a why, a basis note, a pbasis note and a contested note), 9 new ok texts, one warn-not-fail case. The
60-100 seed bound is now "at least 60".

## 3. Q5 expansion: added
All 166 entries are explanation-only (`never`, no level), so no preset, plan row or tick changes. `never` gives the
same guard result as "no KB entry" did (not in presets); only the reason text changes, and the detail pane gains
the KB text and basis.

**Services (111).** Every service in `ws2016-services.tsv` with no entry that the IoT page also lists. Each basis note
names both ratings; a script checked all 111 against the IoT page copy (`iot-services.tsv`) and the tsv: 0
mismatches. Text after "@IOT says" follows the page's description; double-quoted text is verbatim from a row that
was fetched twice, or from the WS16 tsv; anything else is labelled "(reasoning)".

AxInstSV, AJRouter, AppReadiness, AppIDSvc, Appinfo, ALG, AppMgmt, AppXSvc, tzautoupdate, BITS, BFE, CertPropSvc,
ClipSVC, KeyIso, EventSystem, COMSysApp, CoreMessagingRegistrar, VaultSvc, CryptSvc, DsSvc, DcomLaunch,
DeviceAssociationService, DeviceInstall, DmEnrollmentSvc, DsmSvc, DevQueryBroker, Dhcp, TrkWks, MSDTC, Dnscache,
embeddedmode, EFS, EntAppSvc, EapHost, fdPHost, FDResPub, gpsvc, hidserv, IKEEXT, iphlpsvc, PolicyAgent, KtmRm, LSM,
diagnosticshub.standardcollector.service, AppVClient, MSiSCSI, NgcCtnrSvc, swprv, smphost, NetTcpPortSharing,
Netlogon, Netman, NcaSvc, netprofm, NlaSvc, NetSetupSvc, nsi, CscService, PerfHost, pla, PlugPlay, WPDBusEnum, Power,
wercplsupport, RasMan, SessionEnv, TermService, UmRdpService, RpcSs, RpcLocator, RemoteRegistry, RpcEptMapper,
seclogon, SstpSvc, SamSs, LanmanServer, SNMPTRAP, sppsvc, svsvc, StateRepository, StorSvc, TieringEngineService, SENS,
SystemEventsBroker, Schedule, lmhosts, TapiSrv, Themes, TimeBrokerSvc, UevAgentService, UserManager, ProfSvc, vds,
Audiosrv, AudioEndpointBuilder, WbioSrvc, Wcmsvc, WdNisSvc, WEPHOSTSVC, Wecsvc, EventLog, mpssvc, FontCache,
msiserver, Winmgmt, WpnUserService, WinRM, W32Time, WinHttpAutoProxySvc, wmiApSrv, LanmanWorkstation.
RpcSs, DcomLaunch and Audiosrv (Analyze E2) are in this list.

Existence on 19045: on the Server 2016 list (1607 base) and on the current Windows 10/11 IoT Enterprise page, which r3
§2.1 calls the client service set. 18 of them are also in the fixture. An entry for a name a PC doesn't have is
never shown (the KB is looked up by service name).

**Drivers (55).** `published` only where a Microsoft Learn page read on 2026-11-11 names the driver and says what the
text says; otherwise `reasoning` with the note "From the driver's role in Windows; Hadron found no Microsoft page that
describes it."

| Group | Drivers | Basis |
|---|---|---|
| Storage | storport (@STORPORT), stornvme (@STORNVME), storahci (@STORAHCI), USBSTOR and UASPStor (@USBCLASSES) | published |
| Disk class / boot | ACPI, disk, partmgr, volmgr, volsnap, volume (@STOP7B: "make sure that it has a value that's named Start, and that it's set to 0") | published (what each does: reasoning, said in the note) |
| Disk, reasoning | mountmgr, fvevol | reasoning |
| File system | FltMgr (@FLTMGR) | published |
| File system, reasoning | Ntfs, FileInfo, Wof, bindflt | reasoning |
| NDIS/TCP | NDIS, Tcpip, AFD, NetBT, tdx, nsiproxy, mpsdrv, HTTP | reasoning |
| USB host | USBXHCI, UCX01000, USBHUB3, usbccgp (@USBHOST) | published |
| HID | HidUsb (@USBCLASSES); kbdhid, mouhid, kbdclass, mouclass, i8042prt (@HIDKBM) | published |
| Audio | usbaudio (@USBCLASSES), usbaudio2 (@USBAUDIO2) | published |
| Audio, reasoning | HDAudBus | reasoning |
| Display | BasicDisplay, BasicRender (@BASICDISPLAY) | published |
| Display, reasoning | DXGKrnl | reasoning |
| Core and security | pci, Wdf01000, CNG, KSecDD, TPM, mssmbios, Beep, Npfs, Msfs, Mup, WdBoot, WdNisDrv, hvservice | reasoning (WdBoot/WdNisDrv cite @DEFCOMPAT for the services and call the driver link reasoning) |

What each new driver page says, as used:
- STORPORT: "Storport (storport.sys) is a system-supplied storage port driver that's especially suitable for use with high-performance buses, such as NVMe over fibre channel (FC) or FC buses and RAID adapters."
- STORNVME: "StorNVMe (stornvme.sys) is the system-supplied storage miniport driver that provides access to high-speed NVMe devices."
- STORAHCI: "StorAHCI, a Storport miniport, supports serial advanced technology attachment (SATA) advanced host controller interface (AHCI) controllers in Windows, and replaces MSAHCI, an ATAport miniport."
- FLTMGR: "The Filter Manager (FltMgr.sys) is a system-supplied kernel-mode driver that implements and exposes functionality commonly required in file system filter drivers." "FltMgr is installed with Windows, but it becomes active only when a minifilter driver is loaded."
- USBHOST: "The xHCI driver is the USB 3.0 host controller driver."; "All I/O requests initiated by upper drivers reach the host controller extension driver before the xHCI driver."; the hub driver "Manages USB hubs and their ports" and "Enumerates devices and other hubs attached to their downstream ports"; Usbccgp "is the Microsoft-provided parent driver for composite devices."
- USBCLASSES: the class table lists Usbaudio.sys (Audio), Hidclass.sys + Hidusb.sys (HID), Usbstor.sys (Mass Storage), Uaspstor.sys (Mass Storage, subclass 06 protocol 62), Usbhub3.sys, Bthusb.sys.
- HIDKBM: KBDHID "Converts HID usages into scan codes"; MOUHID is the "HID client mapper driver for mice/touchpads"; the keyboard class driver "provides functionality for all keyboards and keypads on the system"; the mouse class driver "provides functionality for all mice and touchpads on the system"; "I8042prt, the function driver for a PS/2-style keyboard" (and mouse).
- BASICDISPLAY: "a generic display driver that ships with the Windows operating system"; the default display driver when "An IHV graphics driver isn't present" or "isn't working or is disabled"; used during an IHV driver reinstall; "BasicDisplay is always used with BasicRender, which is the system-supplied module that exposes the functionality of WARP from an adapter in the kernel."
- USBAUDIO2: "Starting with Windows 10, release 1703, a USB Audio 2.0 driver is shipped with Windows." "The driver is named: usbaudio2.sys."

`iot-services.tsv` (this folder) is the working copy of the IoT page the services were checked against; its header
says how it was made and which rows were verified word for word.

## 3b. Left out, and why
- **WS2016 names not on the IoT page**, so not shown to exist on 19045 client: Browser,
  DcpSvc, UI0Detect, KPSSVC, RSoPProv, sacsvr, tiledatamodelsvc, UALSVC, wudfsvc.
- **Fax**: in the fixture and on the IoT page, but not in ws2016-services.tsv (outside Q5's list) and the IoT page
  gives it no description. Follow-up.
- **Drivers I could not confirm by name on 19045**: HdAudAddService (the HD Audio function driver's service name),
  HidClass (may not be a service of its own), usbport, usbehci, volmgrx, iaStorAVC and other vendor storage, CimFS.
- **Drivers left for a follow-up** (exist, but no time to write them well): usbhub (USB 2), usbvideo, usbprint,
  usbscan, usbser, wcifs, FileCrypt, rdbss, mrxsmb, mrxsmb20, bowser, NdisVirtualBus, WFPLWFS, rspndr, lltdio,
  vmbus and the other Hyper-V drivers.
- **No source** for "safe to disable" exists for any driver (r3 §2.3), so none of the new driver text recommends
  anything; it says what the driver does and what stops without it.

## 4. Docs
- `docs/design/services.md`:
  - Files table: `backend_fake.cpp`; the golden row lists all 13 goldens.
  - Snapshot format: deps and interfaces are `;`-joined. Meta keys are `kind`, `created`, `build`, `machine`, plus
    `source` on pre-apply snapshots. `created` is local time in first.txt and the UTC stamp in snapshots. store.h's
    `hadron` key is never written.
  - Undo log: `P`'s literal `start` field; `ti|admin` is the planned identity in `P` and `I`, not a verified one;
    `D` on `ok` carries the identity the write ran as.
  - Write path: the plain `ControlService` fallback on `ERROR_INVALID_PARAMETER`.
  - Hardware evidence: how DECISIONS' "never hot-plug classes" maps onto the code (never ticked by default; 2-research
    Q9 "never auto-removed"). DECISIONS itself is unchanged.
  - Hadron column (old :232): the order `protected`, `—`, `no preset`, `kept`, else the first preset that lists it,
    and that this level is not a promise of a change.
  - New "Command line" section: `--fixture`, `--tab`, `--select`, `--plan` (incl. `undo`, `undo-last`, `resume`,
    `ti-stop`), `--focus`, `--sort`, `--keys`, `--ti on`, `--data-dir`, `--readonly` (and what it hides without
    `--data-dir`), `--selftest --update-golden --golden`, `--dump-plan` (incl. `stock`), `--live-test`. It also says
    how to regenerate goldens.
- `docs/ARCHITECTURE.md`: `%ProgramData%\Hadron\services\` with its own rules; `tools/` now also holds the services
  KB gate.
- `tools/services/README.md`: the selftest counts, the `keep`/`info`/`drv` helpers, the warnings, the
  count/size/fraction gate and what it doesn't count, the new driver sources, no upper entry bound.
- Not fixed here, because the code is outside my scope: `store.h`'s comments (`hadron` meta key, "identity-verified",
  `E` without `done|cancelled`).

## 5. Goldens and checks this branch changes (the lead regenerates at merge)
Built with MinGW (`-DHADRON_SERVICES_TEST_HOOKS=ON`); no C++ was changed. `--selftest` under Wine: 189 checks, 7
failed, all expected:
- **`KB seed has 60-100 entries (265)`**: `modules/services/selftest.cpp:466` checks `n >= 60 && n <= 100`. It
  needs to become `n >= 60` (genkb's bound now). I couldn't change selftest.cpp (another builder's file). Until then
  `services_check.sh` fails at the selftest step.
- `plan-services-bloat.txt`, `plan-services-stripped.txt`, `plan-services-minimum.txt`: the pbasis lines (quoted
  ratings, the GitHub VDOT URL, new Stripped and contested notes, contested `basis: reasoning`), the why lines
  (lfsvc, RmSvc, dot3svc, XboxGipSvc), XboxGipSvc's evidence (now "(service xboxgip)" instead of "(class
  XnaComposite)"; it stays Kept), and the note "21 item(s) ... have no KB entry" becomes "3". Diffs: 34, 140 and
  174 changed lines.
- `plan-drivers-bloat.txt`, `plan-drivers-stripped.txt`, `plan-drivers-minimum.txt`: only the note "31 item(s) ...
  have no KB entry" becomes "7".
- Unchanged: `plan-services-default.txt`, `plan-drivers-default.txt`, `plan-services-default-delayedlinger.txt`,
  `restore.cmd`, `restore-dryrun.cmd`, `first-restore.cmd`, `first-README.txt` (they don't read KB text).
- Screenshots that will look different: any detail pane for an item that gained text, e.g. `services-detail-protected`
  (RpcSs) and `services-drivers`/Console details; and the plan renders that show pbasis text
  (`services-plan-stripped`, `services-console-plan`, `services-ti-on`).

Every KB diff here goes to `kb-auditor` before merge (rule 7).

## 6. kb-audit-2 should-fix items (S1-S15)
All applied with the auditor's rewordings; none had a source that said otherwise.
- S1 storport: no "most"; StorAHCI is a miniport, and Microsoft encourages vendors to use Storport.
- S2 USBXHCI: the USB 2 ports clause is conditional and labelled reasoning.
- S3 BasicDisplay: "while a new graphics driver is being installed (from removing the old one to installing the new one)".
- S4 hvservice: a support driver; the boot loader starts the hypervisor; if_off "may fail (reasoning, not tested)" and Hadron never changes it while the hypervisor runs.
- S5 HDAudBus: true on either bus: devices on Microsoft's HD Audio bus stop; Intel Smart Sound boards may run onboard audio on Intel's own bus driver; Device Manager's Driver details show which.
- S6 reasoning drivers: "Reasoning from the driver's role in Windows; Hadron cites no Microsoft page for it." (no search claimed).
- S7 iphlpsvc: no Xbox page was read, so the unsourced wording: "reported to need Teredo (not stated on either page)", with a test.
- S8 TPM: anti-cheat clause marked "(not sourced)".
- S9 Appinfo: the UAC prompt when Hadron is started from a normal desktop.
- S10 TermService: the effect first, then @IOT's advice.
- S11 WpnUserService: @IOT's rating is for IoT Enterprise, "the closer of the two to a desktop".
- S12 gate: the count heuristics are replaced by a digit allow-list (`DIGIT_ALLOW` plus identifiers that start with a letter); word checks stay and gain "one <unit>", any size word, `apps`/`programs`, "of the", stacked modifiers and "points". 12 new must-fail cases (the auditor's 11 texts; "one gigabyte" and "a gigabyte" are two). The only KB digit it caught was SysMain's test "3 restarts", now "Restart three times".
- S13 KtmRm: "any MSDTC transaction that involves a kernel resource manager fails".
- S14 usbaudio cites @USBAUDIO2 for "USB Audio 1.0"; USBSTOR says "that don't use UAS" instead of "(Bulk-Only)".
- S15 XboxGipSvc: the pbasis says the driver name is unsourced and a Bluetooth-connected controller doesn't keep the row.

M1 (goldens) and the selftest.cpp:466 bound are the lead's at merge. The plan goldens will now also differ by the
XboxGipSvc pbasis sentence and the new SysMain test text (SysMain's test is not in plan dumps; only pbasis is).
