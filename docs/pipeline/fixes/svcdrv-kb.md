# Small fix: svcdrv KB follow-ups (fix/svcdrv-kb)

Scope: `docs/pipeline/svcdrv/7-finalize.md` "Follow-up ideas" 1-3 and `finalize/kb.md` §3b. KB content, the stock
table, the selftests that assumed "no stock data", the goldens this changes and the docs. No preset, level, tick,
plan/apply or recovery change; no `core/` change; `genkb.py` unchanged (the stock fields already existed).
**§5 "Audit" records the kb-auditor round and supersedes §1-§4 where they differ** (genkb, plan.cpp's reason text,
the stock labels, the fixture and the goldens changed in that round).

Result: 289 entries (265 before, +24); bloat 17, stripped 48, minimum 11 (unchanged); 213 explanation-only;
5 Windows-stock values. `genkb.py --selftest` PASS; regenerated `services.db` byte-identical.

All pages below were read on 2026-11-11 with a web-fetch tool that summarises the page with a model. Quotes used
as quotes in the KB were asked for verbatim; the msiserver and TimeBrokerSvc sentences were fetched a second time
and matched.

## 1. Windows stock (follow-up 1)

### What a 19045 stock table can be built from
Rule 7 allows `owner-measured` or `published` (with a URL). What exists:

| Source | Has start types? | Used? | Why |
|---|---|---|---|
| @IOT, `windows/iot/iot-enterprise/optimize/services` | Yes: a "Startup Type (Default)" value for every listed service | **No** | The page says its list is "the current version of Windows" ("Other versions of Windows 10/11 might not have the same services available") and names no build. Its values disagree with @PERUSER for the same services: cbdhsvc Manual vs "Automated (Delayed Start)", OneSyncSvc Automatic vs "Automated (Delayed Start)", WpnUserService Manual vs Automatic. It marks only three services "Automatic (Delayed Start)" (edgeupdate, StorSvc, SgrmBroker), so "Automatic" can't be told apart from delayed. Several values are not plausible for a Windows 10 client (RemoteRegistry Automatic, Audiosrv and AudioEndpointBuilder Manual, WSearch Manual), which reads like an IoT image, not a client default. Using it would put "Windows stock start type for build 19045" on values the page doesn't claim. |
| @PERUSER, `per-user-services-in-windows` | Yes: "Default start type" per template | **No** | Same "current version of Windows" wording; per-user templates only; disagrees with @IOT (above). |
| @WS16 (ws2016-services.tsv) | Yes | **No** | Server 2016 values (DECISIONS 2026-11-11, R20). |
| BITS's own page, `win32/bits/bits-startup-type` | Yes | **No** | "delayed auto-start (if there are active BITS jobs) or demand start (if there are no active jobs)": no single value to write. |
| "Ensure the Windows Firewall service is started and configured for auto start" (Services Hub) | A required setting | **No** | Written for Windows Server / AD DS, not client. |
| Third-party tables (wintips.org PDF "Windows 10 PRO", smartpcutilities.com, elevenforum) | Yes | **No** | Not Microsoft; unknown build and method (the wintips PDF is from 2021-09, before 22H2). |
| Microsoft Q&A answers | Some | **No** | Community answers, not Microsoft documentation. |
| Microsoft client troubleshooting pages that state one service's default | Yes, one service each | **Yes** | Below. |

### The five sourced rows (`STOCK` in `tools/services/kb.py`)
| Service | Stock | Source (URL) | What the page says |
|---|---|---|---|
| msiserver | Manual (`3/-`) | https://learn.microsoft.com/en-us/troubleshoot/windows-client/installing-updates-features-roles/common-windows-update-errors | "The default Startup type of the Windows Installer service is Manual." Applies to: Windows 10, Windows 11. |
| W32Time | Manual (`3/-`) | https://learn.microsoft.com/en-us/troubleshoot/windows-client/active-directory/w32time-not-start-on-workgroup | On workgroup computers "The startup value for the Windows Time service is Manual"; trigger-start is the default. Applies to all supported client and Server versions. Domain-joined: starts on a trigger, no other start type given. |
| TimeBrokerSvc | Manual (`3/-`) | https://learn.microsoft.com/en-us/troubleshoot/windows-client/system-management-components/task-scheduler-service-not-start | "check if the `Start` value is set to `3` by default. If not, modify it to `3`." Supported client versions. |
| WSearch | Auto, delayed (`2/1`) | https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/windows-search-performance-issues | "The service should have the following configuration: Status: Running; Startup Type: Automatic (Delayed Start)". An expected setting rather than the word "default"; the note says so. |
| BrokerInfrastructure | Auto (`2/0`) | https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/troubleshoot-start-menu-errors | "BrokerInfrastructure is an automatic service." (under a Windows 10 Start menu symptom). Delayed start is not mentioned; reading it as plain Automatic is labelled reasoning in the note. |

None of these pages names build 19045. Each stock note says what its page applies to and "The page names no build;
Hadron uses it as the Windows 10 stock value." The plan row's fixed reason text (plan.cpp, out of scope) still reads
"Windows stock start type for build 19045"; the basis line under it carries the honest scope. A wording change
there is a small plan.cpp follow-up if the auditor wants one.

**Not sourced: every other service and every driver** stays "no data" (the detail says "no data for this item yet"
on the Services tab; Drivers keeps "no data for this build yet"). A real 19045 table needs an `owner-measured`
capture of a clean 19045 install (`sc qc` / registry export of `Start` and `DelayedAutostart` per service), as
r3 §3.3 recommends; a pipeline-VM capture doesn't count as owner-measured (DECISIONS 2026-11-11).

### Module changes for it
No code path changed: `kb.cpp` already reads the stock fields, `plan.cpp` already builds the stock plan, and the chip,
F8 label and detail line already switch on `StockKnown()` (per tab). Only the selftests that asserted "no stock
data anywhere" changed:
- `selftest.cpp`: the KB check now requires every stock value to have a published (with URL) or owner-measured
  basis and none on a storage driver; the fixture's stock plan has the WSearch row (Manual -> Auto delayed,
  `published`) on Services and is empty with a note on Drivers; R20's empty-plan text is checked on Drivers.
- `module.cpp` UI selftests: the chip is plain on Services and "(no data for this build yet)" on Drivers; Console F8
  reads "Stock" / "Stock (no data)" the same way; mini-design 9 (empty stock plan, no Apply) runs on Drivers;
  mini-design 12 checks the Drivers words, a Services item without a value ("no data for this item yet") and
  WSearch's value with "(published)".

One behaviour to know: on the Services tab the chip now drops "(no data ...)" although most services have no value;
the plan's first note says how many items have one ("2 of 109 items ..." on the fixture).

## 2. OEM entries (follow-up 2)
Board components from MSI's specification page (https://www.msi.com/Motherboard/PRO-Z790-P-WIFI/Specification):
"INTEL Z790", "Intel® 2.5Gbps LAN", "Intel® Wi-Fi 6E", "Realtek® ALC897 Codec". All entries are explanation-only
(`never`; third-party items are never in presets, DECISIONS 2026-11-11).

| Service | Basis | Source | Notes |
|---|---|---|---|
| MSI_Center_Service | reasoning | https://www.file.net/process/msi_central_service.exe.html | file.net (third-party, user-contributed) gives "a service (MSI_Center_Service)" for MSI_Central_Service.exe in MSI Center's folder, Micro-Star. Name only; no MSI page found. |
| Mystic_Light_Service | reasoning | https://www.file.net/process/mystic_light_service.exe.html | Service name and MSI Center\Mystic Light folder; name only. |
| RtkAudioUniversalService | reasoning | https://www.file.net/process/rtkauduservice64.exe.html, MSI spec page | "the service name is RtkAudioUniversalService: Realtek Audio Universal Service", driver-store folder realtekservice.inf. No Realtek page found. |
| LMS | published | https://software.intel.com/sites/manageability/AMT_Implementation_and_Reference_Guide/WordDocuments/gloss_lms.htm | Intel: "A service that runs locally in an Intel AMT device and enables local management applications to send requests." That the Windows service named LMS is it: reasoning. |
| jhi_service | published | https://www.intel.com/content/www/us/en/docs/dynamic-application-loader/developer-guide/1-0/host-interface-service.html | Intel: "serves as an intermediate layer between host applications and the Intel DAL firmware component", "commonly referred to by its engineering name: JHI", installed by the Intel ME installer. Service name link: reasoning. |

Not added (no source names the service, and a wrong name would only never show, but the text would be unchecked):
Intel Wi-Fi and Bluetooth services/drivers, the Intel LAN driver, WMIRegistrationService, Intel graphics services
(no iGPU on a KF CPU anyway), other MSI Center modules. The MSI and Realtek items say "The vendor does not document
this service" and give a test for each.

## 3. The rest of the KB (follow-up 3)
| Entry | Basis | Source |
|---|---|---|
| Fax (svc) | reasoning | @IOT lists it, Manual, "OK to disable", no description; not on @WS16. |
| usbhub | published | Supported USB classes (`@USBCLASSES`): "Microsoft provides the Usbhub.sys driver for managing USB hubs"; Usbhub3.sys for SuperSpeed hubs on xHCI. Which stack uses which: reasoning. |
| usbvideo | published | https://learn.microsoft.com/windows-hardware/drivers/stream/usb-video-class-driver-overview: "a Microsoft-provided AVStream minidriver that provides driver support for USB Video Class devices"; Usbvideo.sys for "a webcam or a digital camcorder". |
| usbprint | published | https://learn.microsoft.com/en-us/windows-hardware/drivers/print/usb-printing: "Usbprint.sys is the Microsoft-provided device driver for USB printers." |
| usbscan | published | `@USBCLASSES`: Usbscan.sys "manages USB digital cameras and scanners", WIA. |
| usbser | published | `@USBCLASSES`: "In Windows 10, Usbser.inf loads Usbser.sys automatically as the function driver." |
| rdbss | published | https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/the-rdbss-driver-and-library: "A file system driver (rdbss.sys) supplied with the operating system". |
| mrxsmb | published | Same page: the Microsoft SMB redirector is a non-monolithic mini-redirector (the page writes "mrxsmb sys"). |
| mrxsmb20, bowser | reasoning | No page fetched names them. bowser: "the Workstation service lists it as a dependency" is labelled reasoning. |
| wcifs, FileCrypt | reasoning | The allocated-altitudes page was searched in two parts and neither name was found; FileCrypt's text says Hadron doesn't know what it does. |
| NdisVirtualBus, WFPLWFS | reasoning | No page. |
| lltdio, rspndr | published | https://learn.microsoft.com/windows/client-management/mdm/policy-csp-admx-linklayertopologydiscovery (policy text quoted). |
| vmbus, netvsc, storvsc | reasoning | https://learn.microsoft.com/en-us/windows-hardware/drivers/network/overview-of-hyper-v describes VMBus and NetVSC but no driver file; cited in the note, basis reasoning. |

HyperVideo and hyperkbd already had entries (Stripped, hw `vmguest`); unchanged.
Still unconfirmed by name and not added: HdAudAddService, HidClass, usbport, usbehci, volmgrx, iaStorAVC, CimFS.

## 4. Goldens and checks
- `plan-services-stock.txt`: was "no data"; now one ticked row, WSearch Manual -> Auto (delayed), with its
  published basis, and the note "2 of 109 items have a Windows stock value ..." (WSearch, and BrokerInfrastructure
  already at Automatic).
- `plan-services-bloat.txt`, `-stripped.txt`, `-minimum.txt`: only the note "3 item(s) on this tab have no KB
  entry" becomes "2" (Fax now has an entry). KB wording, not behaviour.
- Unchanged: all drivers plans, `plan-services-default*.txt`, the restore scripts and README goldens.
- Screenshots that look different: every Services-tab shot's chip reads "Default: Windows stock" (no
  "(no data ...)"), and the header says "KB 289 entries". WSearch's "Windows stock: Auto (delayed) (published)"
  line is below the fold of `services-light`'s detail pane; the selftest checks it instead.
- `tests/tools/services_check.sh`: PASS (genkb selftest, byte-identical db, fixture, selftest 283/283 standalone and
  in the suite, 10 plans + delayedlinger, 17 shots). MinGW Release builds of HadronServices and Hadron, with and
  without test hooks.

Every KB diff here goes to `kb-auditor` before merge (rule 7).

## 5. Audit
kb-auditor's report is `docs/pipeline/fixes/svcdrv-kb-audit.md` (34 rows: supported 24, overstated 7, unsourced 2,
wrong 2). Everything applied, with the auditor's wording except where noted:
- **M1** (TimeBrokerSvc's stock note shipped a literal `[[3|...]]`): `genkb.py` `expand()` now renders markers to
  their shown text; `check_entry` fails if `[[` or `]]` would reach a K line; three selftest cases (marker in expl,
  basis note, stock note). The fixture gains TimeBrokerSvc at Disabled, so `plan-services-stock.txt` shows the
  rendered note ("is set to 3 by default").
- **M2**: `plan.cpp` stock row reason is "Windows stock start type from a Microsoft page (it names no build)" for a
  published value ("measured on build N" for an owner-measured one).
- **M3**: lltdio and rspndr no longer mention a network map (Windows 10 has none built in); wording as proposed.
- **s1-s6, s9**: as proposed (wcifs, rdbss/mrxsmb quote, bowser with the `sc qc` check, storvsc's own note, OEM
  "Hadron found no vendor documentation", Realtek playback "reasoning, not tested", One Dragon Center, LMS vPro
  "(reasoning)", the file.net label in kb.py and the README).
- **s7**: applied, except the auditor's "Windows Server 2012 R2 ... Windows Server 2016" fails the genkb digit gate;
  the note says "a Start menu symptom on Windows 10 and two Windows Server releases".
- **s8**: chip "Default: Windows stock (N items)", Console F8 "Stock (N)", F1 help "Plan: Default, Windows stock (only
  the N items with a sourced value)", detail "Manual (published; the page names no build)"; UI selftests updated.

Goldens changed in this round, and why:
- `plan-services-stock.txt`: new reason text (M2); the TimeBrokerSvc row with its rendered note (M1); the note
  becomes "3 of 110 items".
- `plan-services-default.txt`, `plan-services-default-delayedlinger.txt` ("already set" +1), `first-restore.cmd`,
  `first-README.txt` (one more value line): only because the fixture has one more service (TimeBrokerSvc).
- `services.db`: the wording above and the rendered TimeBrokerSvc note.

Found while doing M1, then fixed on the lead's go-ahead (DECISIONS 2026-11-11, `proxy (Small fixes)`): the stock
plan blocked every item whose KB entry is `never` ("Protected: not in presets. Hadron's KB keeps it out of every
preset"), and every explanation-only entry is `never`, so of the five stock values only WSearch could be applied.
- `plan.h` `Guard` gains `kbNever` (set only by the KB `never` branch of `CheckGuard`). In the Windows-stock plan only,
  a row is blocked by `neverTouch` or by `neverPreset` without `kbNever`: never-touch items, protected services and
  the named never-preset list block as before. Presets and Default as first seen are unchanged (their goldens match).
- BrokerInfrastructure is on the floor list (never-touch), so its stock row stays blocked; on the fixture it is
  already at Automatic.
- Selftests: TimeBrokerSvc on the fixture is a ticked row Disabled -> Manual; with an in-memory KB whose entries
  are all `never`, TimeBrokerSvc is ticked while RpcSs (floor), SecurityHealthService (protected) and
  TrustedInstaller (named never-preset) stay blocked; no preset offers TimeBrokerSvc and Default as first seen
  leaves it.
- `plan-services-stock.txt`: the TimeBrokerSvc row is now `[x]` (2 ticked, 0 kept).

## 6. Review
The reviewer found no category 1-4 issues. The small ones, all applied:
- **F1**: on 19045, a tab with no stock value (Drivers) no longer says "no data for this build": the plan says
  "Hadron's KB has no Windows stock value for any item on this tab: nothing will be changed." and the chip and
  detail say "(no data on this tab)". Other builds keep the build sentence. R20 and the UI selftests updated;
  `plan-drivers-stock.txt` regenerated.
- **F2**: genkb fails a published stock note that cites a non-learn.microsoft.com page or lacks "names no build"
  (two must-fail cases). WSearch's note now says "the page names no build".
- **F3**: the chip reads "Default: Windows stock (N sourced)".
- **N1**: genkb fails `stock` on a driver without `sysok` (one must-fail case).

## 7. Stock from tables and from this PC's manifests (fix/svcdrv-stock)
Owner's call (DECISIONS 2026-11-11): rule 7 counts a linkable third-party table as `published`, and the stock values
come first from the PC's own WinSxS manifests, cross-checked with the tables.

### Sources
| Key | Table | Version it states |
|---|---|---|
| `@TBLWINTIPS` | wintips.org, "Default Startup Type of Services on Windows 10 PRO OS", https://www.wintips.org/wp-content/uploads/2021/09/DefaultServicesWin10.pdf | Windows 10 Pro; no version or build (published 2021-09) |
| `@TBLTWEAKHOUND` | TweakHound, "Windows 10 Default Services v20H2", https://tweakhound.com/?p=18808 | Windows 10 20H2, build 19042.685 (23 Dec 2020) |

Neither names 19045/22H2, so every table note says the value "names no build". They are independent captures: they
differ on six services, and each has rows the other lacks. Both were read with WebFetch on 2026-11-11 and
transcribed by display name; names were mapped to service names through the IoT page, the fixture and the KB
titles. Unmapped rows (ignored): Local Profile Assistant Service, Microsoft (R) Diagnostics Hub Standard Collector
Service, Microsoft Update Health Service, Wi-Fi Direct Services Connection Manager Service, Windows Mixed Reality
OpenXR Service, WPF Font Cache. A page I could not fetch (winhelponline) is not used.

### Counts (289 KB entries)
- **Sourced: 183.** 178 from two agreeing tables (`stock_src` `table`), 5 from Microsoft pages (`microsoft`,
  unchanged: msiserver, W32Time, TimeBrokerSvc, WSearch, BrokerInfrastructure).
- **Sources disagree: 5** (shown in the detail pane, never applied): BITS (Manual / Automatic delayed),
  DeviceAssociationService (Manual / Automatic, both trigger), WlanSvc (Automatic / Manual), wscsvc (Automatic /
  Automatic delayed), wuauserv (Automatic / Manual, both trigger). W32Time also differs between the tables
  (Automatic delayed / Manual trigger) but keeps its Microsoft value.
- **None: 101.** 84 drivers (neither table lists drivers, so no driver gets a value) and 17 services the tables
  don't list (per-user templates, OEM and third-party entries).
- Follow-up: 40 services both tables agree on have no KB entry yet (AssignedAccessManagerSvc, BDESVC, camsvc,
  DispBrokerDesktopSvc, DisplayEnhancementService, DoSvc, edgeupdate, edgeupdatem, fhsvc, GraphicsPerfSvc,
  IpxlatCfgSvc, LxpSvc, MicrosoftEdgeElevationService, NaturalAuthentication, NcdAutoSetup, p2pimsvc, p2psvc,
  PeerDistSvc, perceptionsimulation, PNRPAutoReg, PNRPsvc, PushToInstall, SDRSVC, Sense, SgrmBroker,
  SharedRealitySvc, shpamsvc, spectrum, ssh-agent, TokenBroker, TroubleshootingSvc, VacSvc, WarpJITSvc, wbengine,
  Wcncsvc, WebClient, WManSvc, workfolderssvc, WpcMonSvc, WwanSvc). On a PC the manifests cover them.

### genkb
- K lines have 27 fields; the last is `stock_src` (`microsoft`, `table`, `disagree` or empty).
- Five new must-fail cases (70 in all): a non-Microsoft note without "third-party table"; a table value from one
  table; a table note without "names no build"; `stock_disagree` naming one source; `stock_disagree` with a value.

### This PC's WinSxS manifests (`modules/services/sxsstock.cpp`)
- Method from the read-only probe on the owner's PC (19045.2965 Pro): each manifest is "DCM\x01" + an msdelta
  delta against wcp.dll's type-614 resource; 18,883 manifests decompressed with no failure in about 18 s.
- Precedence: edition file override, then serviceData start or the Services\X registry value, then a
  configuration-setting default (low confidence: applied only when the KB's value agrees).
- Cross-check, per item: manifest and KB agree, or manifest only: applied (`owner-measured`, "read from this PC's
  WinSxS component manifests, Windows build N.UBR (edition)"). KB only: applied as before. Disagree, tables
  disagree, setting default alone with no agreeing table, or on the runtime list (WlanSvc, stisvc, camsvc, IKEEXT,
  PolicyAgent, iphlpsvc, ssh-agent, WinHttpAutoProxySvc, BITS, WinRM, dcsvc, dmwappushservice): shown, not applied.
- The probe's list of manifest-vs-stock candidates (Audiosrv, AudioEndpointBuilder, IKEEXT, PolicyAgent, camsvc,
  stisvc, WlanSvc, ssh-agent, iphlpsvc, BITS, dcsvc, dmwappushservice, WinRM, MSDTC) came from the probe session's
  knowledge, not a source; the module doesn't hard-code them as stock. Audiosrv/AudioEndpointBuilder resolve through
  the edition file; the rest are either on the runtime list or caught by the cross-check.
- Cache: `sxs-<build>.<UBR>-<EditionID>.txt` in the data folder, written once after `EnsureRoot` (A1).
- Tests: hand-written XML fixtures (`tests/fixtures/services/sxs/`, one per layer, plus an edition file) through the
  fake backend's reader, and scanner, edition, conflict, cache and cross-check cases in the selftest. Live
  decompression is not run under Wine; it is checked on the owner's PC from the project folder.

### Goldens changed
- `plan-services-stock.txt`: 6 applied rows (DiagTrack and XblGameSave from the tables; OneSyncSvc and PcaSvc from
  the fixture's manifests; TimeBrokerSvc and WSearch from Microsoft pages), 4 shown-not-applied rows (DPS setting
  default only, SysMain manifest vs tables, stisvc and dmwappushservice runtime-set), 93 of 110 with a value.
- `plan-drivers-stock.txt`: the manifests note, and "Hadron has no Windows stock value for any item on this tab".
