# svcdrv Analyze: `docs` (documentation researcher)

Scope: the module's Win32/NT calls checked against Microsoft Learn (fetched 2026-11-11), spec conformance against
`docs/pipeline/svcdrv/2-research.md`, and a KB spot-check against the source pages. Code at 34ee85c. I changed no files.

## Findings

| # | Rating | Where | Finding |
|---|---|---|---|
| D1 | minor (suspected) | backend_live.cpp:288-298, module.cpp:1694 | Auto (delayed) on a service in a load-order group: the docs say "A delayed auto-start service cannot be a member of a load ordering group". SetStart writes Start=2 first, then the delayed flag. If the SCM rejects the flag, the service is left Auto (not delayed). The row is logged `fail` and Undo can revert it, but the config written is not the one the user asked for. |
| D2 | minor | module.cpp:405 | `--ti on` turns the TrustedInstaller toggle on at launch in **live** mode, not only for fixtures and screenshots. Spec §5.6 says "Off at every launch". The deviation isn't recorded in 3-build.md or the design note. |
| D3 | minor | spec §5.5 vs hw.cpp | Two hardware items are silently not done, and neither is in "Not done" or the design note: (a) services are not mapped to hardware through device-interface triggers (`ifaces` is read in backend_live.cpp:137-141 but never used in `HwEvidence`); (b) the devnode last-arrival date is not read. |
| D4 | minor | plan.cpp RunControl; design note "Undo log" | Start/Stop/Restart writes only to the host log; no file goes to the data dir. The design note lists `control` as a `B` source in the undo-log format, but nothing writes it. Spec §5.2 only says "a log line", so this is a doc inaccuracy. |
| D5 | minor | docs/design/services.md "Guards" | The note says "boot- or system-start drivers" are never touched. The code (correctly, per spec §5.2) never-touches only Boot-start. System-start drivers are allowed as single items, and in presets only with `sysok`. Fix the note. |
| D6 | minor | backend_live.cpp:323-327 | The stop reason is `PLANNED\|MAJOR_NONE\|MINOR_NONE` (0x40060017 with MinGW's headers). Learn's SERVICE_CONTROL_STATUS_REASON_PARAMS page lists MINOR_NONE as 0x00060000, which looks like a doc typo, and doesn't say whether NONE/NONE is accepted. ControlServiceEx returns ERROR_INVALID_PARAMETER for an invalid reason. No required CI step runs Stop: `--live-test` only changes start type, and only the continue-on-error TI variant stops TI. If the reason is rejected, every Stop and Restart fails. It fails safe, but untested. |
| D7 | minor | store.cpp:265-273 | `EnsureRoot` accepts an existing `%ProgramData%\Hadron\services` without checking its owner, DACL or reparse attribute. ProgramData's default ACL lets standard users create subfolders, so a pre-created folder keeps a user-writable ACL. Undo logs and `restore.cmd` read from or written there would then be tamperable (`BuildUndo` applies no `CheckGuard`; the confirm screen still shows every row). Single-user PC, so low risk. Fix: on an existing folder, check the owner is BA/SY and it is not a reparse point, or re-apply the protected DACL. |
| D8 | minor | hw.cpp:123-161, plan.cpp:174-180 | `HwEvidence` returns 3 for an unknown hint, with the text "...: kept". BuildPreset treats 3 as included (ticked). It can't be reached today because genkb's `HW_RE` rejects bad hints. Return 0 instead. |
| D9 | minor | recovery.cpp `:put`, README | (a) The `DelayedAutostart` `reg add` errorlevel isn't checked (already known). (b) Rows that needed TrustedInstaller (key DACL refuses admins) will probably print FAILED in `restore-online.cmd` (admin), and possibly in WinRE (SYSTEM), and the README doesn't say so. |
| D10 | minor | KB SOURCES | Three URLs use regional locales: TAMPER and ADDSERVICE are `en-au` (and ADDSERVICE's path is upper-case), LOADORDER is `en-in`. All three load, and their canonical is `en-us`. PROTSVC uses the legacy `/windows/desktop/Services/` path (loads; canonical is `/windows/win32/services/`). VDI is archived and NOINDEX, with its canonical under `/previous-versions/`. Normalise them to the canonical en-us URLs. |
| D11 | minor | KB SysMain `test` | "3 restarts each way" passes the number heuristic because "restarts" isn't a unit. It is a test-protocol instruction, not a claimed effect, so it is acceptable under rule 7's "say test it and how". Flagged for kb-auditor's consistency only. |
| D12 | minor | module.cpp:1459-1462 | The detail pane's "Needed by" ignores `+Group` dependencies, which `DependencyCheck` does handle. |
| D13 | minor | backend_live.cpp:249-258 | EnumDependentServices has a documented 64,000-byte maximum buffer. The loop grows to `need+256` without a cap. Harmless in practice. |
| D14 | minor | model.cpp:57-64 | `SERVICE_RECOGNIZER_DRIVER` (0x8) and `SERVICE_ADAPTER` (0x4) classify as `Other` and land on the Services tab. None exist on stock 19045, so this is cosmetic. |

No serious findings from the documentation pass.

## 1. API check against Microsoft Learn

| API / use | Docs say | Code | Verdict |
|---|---|---|---|
| EnumServicesStatusExW | Needs SC_MANAGER_ENUMERATE_SERVICE. Max buffer 256K. ERROR_MORE_DATA plus a resume handle. Documented types are only WIN32/DRIVER and their parts. A service the caller can't SERVICE_QUERY_STATUS is **silently omitted**. | 0x3FF with fallbacks to SDK ALL and to WIN32\|DRIVER on ERROR_INVALID_PARAMETER; 256K cap; copies `count` entries on each MORE_DATA round | OK. Note: items hidden from admins never appear, not even as "unreadable". Templates with 0x3FF on 19045 still need Windows (spec §10). |
| QueryServiceConfigW / QueryServiceConfig2W | SERVICE_QUERY_CONFIG; 8K max; ERROR_INSUFFICIENT_BUFFER with pcbBytesNeeded; TRIGGER_INFO needs Win7+, LAUNCH_PROTECTED needs 8.1+ | grow-and-retry ×4 | OK |
| Hx trigger structs | SERVICE_TRIGGER{DWORD,DWORD,GUID*,DWORD,ptr}; INFO{DWORD,ptr,PBYTE}; ITEM{DWORD,DWORD,PBYTE}; ACTION_STOP=2; LAUNCH_PROTECTED=12 | matches | OK |
| ChangeServiceConfigW | SERVICE_CHANGE_CONFIG. BOOT/SYSTEM valid only for drivers. On a running service, changes take effect when it stops. ERROR_SERVICE_MARKED_FOR_DELETE etc. | SERVICE_NO_CHANGE plus nulls except dwStartType; never sends 0/1 (BuildSingle blocks <2) | OK |
| ChangeServiceConfig2W DELAYED_AUTO_START | Ignored unless the service is auto-start ("any service can be marked"). **Cannot be a member of a load ordering group.** No per-level rights beyond SERVICE_CHANGE_CONFIG. | written after Start | see D1 |
| ControlServiceExW | SERVICE_STOP right; dwReason must be general+major+minor; pszComment <128 chars; ERROR_DEPENDENT_SERVICES_RUNNING; blocks up to 30 s if the SCM is busy | dependents stopped first via EnumDependentServices (reverse start order, as documented); comment "Hadron Services" | OK; reason code untested (D6) |
| EnumDependentServicesW | SERVICE_ENUMERATE_DEPENDENTS; reverse start order; 64,000-byte max | matches; uncapped growth | D13 |
| StartServiceW | Disabled gives ERROR_SERVICE_DISABLED | ALREADY_RUNNING tolerated; other errors surfaced | OK |
| OpenSCManager/OpenService rights | | Reads: CONNECT(+ENUMERATE), QUERY_CONFIG\|QUERY_STATUS. CanWrite opens CHANGE_CONFIG only and closes it. | Matches spec §5.1/§5.6 |
| RegGetValueW | RRF_RT_REG_EXPAND_SZ must be combined with RRF_NOEXPAND | it is; ServiceDll is expanded later | OK |
| SetThreadToken | Token must be an impersonation token with TOKEN_IMPERSONATE; caller needs impersonate privilege. The page says nothing about a silent downgrade. ImpersonateLoggedOnUser's page now says an identification-level token makes the call **fail**. | DuplicateTokenEx(SecurityImpersonation, TokenImpersonation, TOKEN_IMPERSONATE\|QUERY\|DUPLICATE); after SetThreadToken it re-reads TokenImpersonationLevel | Defensive check is sound whichever doc is current |
| CheckTokenMembership(NULL, TI SID) | NULL uses the thread's impersonation token; TRUE only if the SID is present and SE_GROUP_ENABLED | used while impersonating | OK (the TI service SID is an enabled group in TI's token; needs Windows) |
| AdjustTokenPrivileges | succeeded and GLE != ERROR_NOT_ALL_ASSIGNED | ti.cpp:29-32 | OK (rule 4) |
| CM_Get_Device_ID_List(FILTER_NONE), CM_Locate_DevNode NORMAL then PHANTOM | lists all instance IDs; NORMAL fails for non-present, PHANTOM finds them | as documented | OK. The list can change between Size and List (CR_BUFFER_SMALL, no retry): nit. |
| IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS | only `Extents[0]` is used | spanned/striped volumes are ignored | nit; the curated storage floor backs it up |
| MoveFileExW | no COPY_ALLOWED, no REPLACE_EXISTING, WRITE_THROUGH | | OK (rule 1) |
| CreateMutexW `Global\` + WaitForSingleObject(0) | ownership is thread-affine; ReleaseMutex on the same Worker thread | RAII on the same thread | OK |
| reg load | The Learn page says the file "must have a .hiv extension" and be made by `reg save`. In practice `reg load` of `config\SYSTEM` from WinRE is common. The page lists no privilege requirement (RegLoadKey needs SeRestore+SeBackup; WinRE runs as SYSTEM). | | Docs contradict practice: needs Windows (spec §10 already lists it) |
| Per-user services (Learn) | Disable by setting the **template's** Start=4 (reg/GPP/`sc config`); instances are still created, stopped and disabled; test first. PimIndexMaintenanceSvc, UnistoreSvc, UserDataSvc and WpnUserService "can't be managed with a security template". Silent on ChangeServiceConfig for templates. | writes templates only | OK. Whether ChangeServiceConfigW accepts those four templates needs Windows; if refused, CanWrite marks them TI. |
| Protected anti-malware services (Learn) | "the system doesn't allow any non-protected process to alter the configuration of a protected service"; admins can't stop it; only `sc config start=Auto`, qc, start, interrogate and sdshow are allowed | `protect != 0` is never-preset; UI says TI doesn't help | Matches spec §0.1 |
| Tamper Protection | "Attempts to modify Microsoft Defender Antivirus settings through the registry are blocked" | UI text | OK |

## 2. Spec conformance (2-research.md)

Legend: met / partly / not met / deferred-by-spec.

**§2 scope and build order**: B1–B6 done (3-build). Deferred items (stock table for other builds, custom presets, measurement, signer) are listed in "Not done": **deferred-by-spec**. The 19045 stock table is empty: recorded.

**§3 files**: all present. No `core/` change beyond the registration: met.

**§5.1 Lists**
- EnumServicesStatusExW with SC_ENUM_PROCESS_INFO, all types, all states: met (0x3FF plus fallback).
- Per-item QueryServiceConfig plus five Config2 levels: met (backend_live.cpp:96-163).
- Read-only access rights: met.
- Runs on a Worker: met (module.cpp:425).
- `_LUID` instances folded under their template, detected by type bits: met (model.cpp:57-64, 114-).
- Drivers tab state from the SCM: met.
- Columns: partly. "Preset target for the selected level" became the lowest preset level, which the design note records. Company is read in the load Worker rather than a second pass (nit).
- Pending restart: met (module.cpp:1373).
- Detail pane:
  - registered description labelled as not Hadron's: met;
  - KB text with basis: met;
  - image, account, deps, dependents, failure actions, triggers: met (dependents ignore groups, D12);
  - current / first / stock: met;
  - protection status: met (module.cpp:1446-1455).
- Protected marker: met.

**§5.2 Single-item actions**
- Start type through the full pipeline; choices Auto, Delayed, Manual, Disabled; drivers without Delayed: met. D1 caveat.
- Triggers never removed: met.
- Start/Stop/Restart with a confirm listing dependents, dependents first, ControlServiceExW, timeout and progress: met. The "log line" goes only to the host log (D4). Drivers refused: met.

**§5.3 Presets**
- Four levels per tab: met.
- Default (a) first snapshot and (b) stock on 19045 only: met. (b) has 0 rows, recorded.
- Pure bloat is `bloat` entries only; generator forbids `contested` there: met.
- Stripped hardware/use-absent evidence:
  - hardware-hinted rows: met, plus the unticked hot-plug and use-undetectable rows (recorded);
  - device-interface-trigger mapping: not met (D3).
- Minimum: no Boot drivers, `sysok` gate, never-touch: met. The code is stricter: every driver in a level needs `sysok`.
- Plan rows show current→target, reason, pbasis, contested sources and an include toggle: met.
- Dependency warning with the row unticked: met (DependencyCheck handles `+Group`).

**§5.4 Apply/undo/recovery**
- Data dir and protected DACL on creation: met. An existing folder is not checked (D7).
- Selftest and screenshot root injected: met.
- File naming, CREATE_NEW, `-N` stamps, `recovery\first` and `recovery-first.zip`: met.
- Formats: header, UTF-8/TAB, escaping, unknown lines skipped, 16 MB cap, CRC per record: met.
- First run: atomic temp + flush + MoveFileExW, read back, machine GUID, corrupt file renamed aside: met.
- Apply steps 1–6 in order. Abort before any change if 1–3 fail. I flushed before the SCM call, D after. A per-item error continues; a log failure aborts. Dependents-first order; cancel between items: all met (plan.cpp RunApply).
- Undo restores only changed fields. "Absent" is restored as 0 with a note (recorded): partly, accepted.
- Interrupted banner with Undo/Resume; torn last record ignored: met.
- restore.cmd:
  - ASCII plus UTF-16 .reg files; `%~d0`/`%1`; SYSTEM check; no drive loop; reg load, Select\Current, query-guarded add, unload; README with WinRE, manage-bde and the Safe Mode `fltmc` variant; start types only: met;
  - DelayedAutostart errorlevel and TI rows: D9.
- Rule 6 reading: recorded.

**§5.5 Hardware**
- CfgMgr32 over present and phantom devnodes: met.
- Service, filters and parent: met.
- **Last-arrival date: not met (D3b).**
- Never-seen rule: met.
- Hot-plug classes: met.
- **Trigger mapping: not met (D3a).**
- `hw=` hints: met.
- Evidence text per row: met.
- No hard-coded owner names: met.

**§5.6 TI**
- Off at launch, never persisted: **partly** (`--ti on` in live mode, D2).
- Label: met.
- CanWrite marking; rows excluded with the toggle off: met.
- Acquire once, only with marked rows: met. Order differs from the spec (TI started first), harmless.
- Verified per call; identity in the log: met.
- Never impersonating while writing files: met (thread-local flag on every Store writer).
- Stop TI only if Hadron started it; crash marker as `T` records (recorded): met.
- Module-local; no registry fallback: met.

**§5.7 Drift**: met. Event 7040 is deferred and listed.

**§6 Guards**:
- Every never-touch class: met. The floor lists match spec and r4 groups, and hypervisor drivers only while it runs.
- Never-in-preset names, third-party/unknown, wrong build: met. PPL is also never-preset (extra).
- `sysok` checked against the live start type: met.
- `_LUID` never written: met.
- Vanished or changed rows skipped: met.
- TrustedInstaller blocked while the toggle is on: met (MarkTi and BuildSingle).

**§7 KB**: met.
- Every generator gate listed in §7 exists in genkb.py:69-176: missing basis/pbasis, unmarked number, owner text, target {3,4}, never+levels, driver without sysok, stock on storage, contested bloat.
- 99 entries.

**§9 Acceptance**:
- 1, 2, 3, 4, 5 and 6 pass under Wine per 3-build (not re-run by me).
- 7 is wired in CI but never run. It covers SetStart only, not Control (D6). The TI variant proves impersonated writes, not that TI unlocks an admin-denied descriptor: the dummy service's descriptor admits admins.
- 8: passed per 3-build.

**Silent deviations** (not in 3-build.md or the design note): D2, D3a, D3b, and the inaccurate design-note text in D4 and D5.

## 3. KB spot-check (services.db / kb.py)

I checked 47 entry claims against the live sources. These cover every Pure-bloat entry checked, most Stripped entries and all Minimum entries:
- IoT Enterprise page (updated 2026-06-24): ratings for 49 services;
- WS16, using the page plus `ws2016-services.tsv` for the part WebFetch truncates;
- VDOT Services.json, fetched raw: 23 names;
- VDI 2004;
- RTLF, WUERR, DEFCOMPAT, STARTMENU, REGBACK, PROTSVC, TAMPER, LOADORDER, ADDSERVICE.

**All checked claims match the sources.** Examples:
- **IoT ratings:**
  - DiagTrack, MapsBroker, lfsvc, RetailDemo, WMPNetworkSvc, MessagingService, SmsRouter, SEMgrSvc, PhoneSvc, WalletService, icssvc, wisvc, BcastDVRUserService and autotimesvc are rated OK to disable.
  - The Xbox trio is rated "Should be disabled".
  - SysMain, DPS, WerSvc, DusmSvc, DiagSvc, WdiSystemHost, WpnService, UsoSvc, WaaSMedicSvc and TrustedInstaller are rated "Don't disable". The KB writes "Do not disable", which is fine.
  - dmwappushservice is rated "No guidance", correctly kept out of bloat.
  - Every vmic*, HvHost, Sensor*, SmartCard*, Bluetooth*, WIA, SSDP/UPnP, lltdsvc, RasAuto, PcaSvc, QWAVE, ShellHWDetection and SharedAccess entry is rated OK to disable. RemoteAccess is "Already disabled".
- **WS16:**
  - dmwappushservice is OK to disable, with the Intune/MDM and UWF comment.
  - Spooler: "OK to disable if not a print server or a DC".
  - DiagTrack, CDPSvc, DPS, SysMain and dot3svc: No guidance.
  - WerSvc: Do not disable.
  - SharedAccess: the comment says it is needed for hotspot and Miracast.
- **VDOT** disables every entry the KB says it does. WalletService, RetailDemo, PhoneSvc, WMPNetworkSvc and wisvc aren't in VDOT, and their pbasis correctly doesn't claim it.
- **VDI:**
  - DiagTrack: "Consider disabling if on disconnected network".
  - WSearch: "Test before disabling".
  - SysMain: "should be evaluated in each environment".
- **RTLF:**
  - wlidsvc: feature updates stop on 1709+ and apps "may lose functionality".
  - LicenseManager: the baseline sets Start=4.
  - Windows Update: "we do not recommend disabling".
- **WUERR**: 0x80070422 is ERROR_SERVICE_DISABLED. The page's description says "stops working or isn't running", which is close enough to "with it off".
- **DEFCOMPAT**: "Don't disable, stop, or modify ... wscsvc, SecurityHealthService, MsSense, Sense, WinDefend, MsMpEng". wscsvc being disabled leaves Defender active, which is "not supported". WdNisSvc isn't named, and the KB doesn't claim it is.
- **STARTMENU**: BrokerInfrastructure is needed for a stable Start menu, and PDC "shouldn't be disabled or deleted".
- **REGBACK**: since 1803 there are no RegBack copies, and a restore point is recommended.

**Rule 7 numbers**: no savings or performance numbers anywhere. The numbers in text are:
- error codes;
- version and build names;
- "64-bit";
- "3 restarts" (D11).

The generator's number gate is the bios heuristic. **URLs**: every source URL resolved (D10 for locale and canonical).

## Needs Windows (exact checks, read-only or on the CI dummy service)

1. `sc.exe create HadronTestSvc binPath= "C:\Windows\System32\cmd.exe /c exit 0" start= demand group= NetworkProvider`, then `HadronServices.exe --live-test HadronTestSvc --data-dir <tmp>`. Expect the Auto (delayed) step to show whether ChangeServiceConfig2 rejects a delayed flag on a grouped service (D1). Delete the service afterwards.
2. Stop reason: on a VM, `sc start HadronTestSvc` against a real service binary, then a Hadron control-plan Stop. Or add a Stop/Start round trip to `--live-test` with a service that stays running. Confirm ControlServiceExW accepts 0x40060017 (D6).
3. TI, on a VM: does `ChangeServiceConfigW` as impersonated TI succeed on WaaSMedicSvc's descriptor where admin is denied? Read-only variant: `CanWrite` as admin vs as TI, with no write. This is spec §10.
4. `OpenThreadToken(TOKEN_QUERY, OpenAsSelf=TRUE)` on the duplicated TI token, whose default DACL comes from winlogon's token: confirm it succeeds as admin. Otherwise every TI write fails in `Impersonate`.
5. `EnumServicesStatusExW(…, 0x3FF, …)` on 19045 returns templates (type 0x50/0x60) as well as instances (spec §10).
6. `ChangeServiceConfigW` on the PimIndexMaintenanceSvc, UnistoreSvc and UserDataSvc templates as admin. The per-user page says these "can't be managed with a security template".
7. WinRE:
   - `reg load HKLM\HXOFF D:\Windows\System32\config\SYSTEM` despite the `.hiv` wording on Learn (spec §10);
   - `reg add` on `ControlSet001\Services\WaaSMedicSvc` as SYSTEM in WinRE (D9b).
8. Owner PC, read-only, in the project folder: class filters, boot storage path, LAN/Wi-Fi/BT names, VBS state (spec §10).
9. MSVC compile (builder-flagged): `std::max<size_t>`, `wcschr` const-ness, the Hx trigger structs vs an SDK that defines `SERVICE_TRIGGER*`. The structs have distinct names, so no clash is expected.
