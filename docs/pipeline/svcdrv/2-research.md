# svcdrv: Research (stage 2) and Build spec

Stage 2 (Research), 2026-11-11. Branch `module/services-svcdrv`, built on the stage 1 commit 38ee19d. Full run.

Five research reports sit in `research/`. Each is long, and section references below point into them:
- `r1-windows-apis.md`: principal Windows developer. Win32/NT APIs, privileges, OS behaviour. Written from memory; items marked `[verify]` need a Windows VM.
- `r2-architecture.md`: principal developer. Module contract, `core/` fit, files, formats, phasing.
- `r3-documentation.md`: documentation researcher. Microsoft Learn and other primary sources, each claim with a URL. 44 sources; `ws2016-services.tsv` is the parsed Server 2016 guidance.
- `r4-kernel-hardware.md`: kernel and hardware fellow. Boot load order, the never-touch set, the owner's Z790 board.
- `r5-logic-tests.md`: logic machine. Requirements R1–R49, state machines, edge cases E1–E30, failure modes and the test plan.

Where the reports disagree, this file decides. Where this file is silent, the reports are guidance, not spec.

---

## 0. What research changed in the idea

1. **TrustedInstaller doesn't unlock Defender.** Microsoft's "Protecting anti-malware services" page says only a protected
   process can change a protected service's config, and Tamper Protection blocks the registry route as well (r3 §1.9, §3.1).
   A thread impersonating TI is still an unprotected process. So the TI toggle unlocks items whose **SCM security descriptor
   or registry key DACL** refuses Administrators, such as WaaSMedicSvc's key. It does not unlock PPL services. The UI says so.
2. **The Defender/Security stack, TrustedInstaller, VSS and swprv are out of every preset** (proxy Q4). VSS is the restore-point
   path, and the owner has no backups. TrustedInstaller is what the toggle needs.
3. **"Pure bloat" has to be uncontroversial.** The five Microsoft sources contradict each other on about 10 services:
   SysMain, DPS/WDI, WerSvc, DusmSvc, Themes, TabletInputService, WpnService, wuauserv, VSS, and others (r3 §2.2). Contested
   items go only into Stripped or Minimum, never Pure bloat (proxy Q7).
4. **Drivers: a computed never-touch set, not just a list** (r4 §2). Disabling a driver still named in a class or device
   UpperFilters/LowerFilters entry kills every device of that class, which means 0x7B for disks and no keyboard at logon.
5. **Storage start values differ per PC by design.** Windows demotes unneeded boot-start storage drivers itself, so a
   "Windows stock" table must never set start values for storage drivers (r4 §1.1).
6. **No published measurement of RAM, boot or FPS gains exists** that is fit to show (r3 §5, r4 §8). The UI shows no
   predicted savings. Measured before/after needs a reboot-to-reboot protocol and is deferred (proxy Q1c).
7. **Last Known Good can't be relied on, and Safe Mode won't load a Start=4 driver.** Offline recovery from WinRE is the only
   dependable undo when Windows won't boot (r1 §6.3, r3 §4, r4 §1).

## 1. Goal

A new module `services` ("Services"): a page that lists every Win32 service and every kernel/file-system driver with live
state and start settings, explains the stock Windows ones with a basis, changes start types and service state safely, and
applies four preset levels (Default, Pure bloat removal, Stripped, Minimum) separately to the Services tab and the Drivers tab.
It also has an off-by-default TrustedInstaller toggle for writes that admin is refused. Every config change is planned,
confirmed, snapshotted, logged before it happens, undoable, and recoverable offline from WinRE.

It builds as `HadronServices.exe` standalone and inside `Hadron.exe`. `Admin::Required`.

**Builder:** `module-builder-careful`. It moves no files, but a bad write can stop Windows booting on a PC with no backups.

## 2. Scope of this run (proxy Q1)

**In this run** (the owner's words): both lists with state and start settings; explanations for stock Windows items; live
start/stop/restart for services; start-type changes; all **four** preset levels on both tabs; the TI toggle.

**Deferred to follow-up ideas** (proxy additions in 1-idea.md, revised by the proxy and logged in DECISIONS.md):
- the "Windows stock" table for builds other than Win10 22H2 (19045);
- custom presets with a plain-text export/import file. The plan's per-row include toggle covers overrides;
- measured before/after RAM and process count;
- the Authenticode signer column. Show the company from VERSIONINFO, labelled as the file's own claim.

Status signals for the follow-ups go into `3-build.md`'s "Not done" list so Push can file them as ideas.

### Build order (one commit or more per step; each step fixture-tested before the next)

| Step | Delivers |
|---|---|
| B1 | Read-only page: Backend (live + fake), Services and Drivers tabs, list, filter, sort, keyboard, detail pane, refresh, `--screenshot`, `--selftest` |
| B2 | First-run snapshot plus recovery folder and zip, written the first time live data loads |
| B3 | KB generator `tools/services/` and the KB seed, through `kb-auditor` |
| B4 | Single-item writes: start type (plan + snapshot + undo log + recovery), and start/stop/restart (confirm + log). Undo last, interrupted Undo/Resume, apply lock, `--live-test` |
| B5 | Preset engine with all four levels, guards and hardware detection, plus Default (restore first snapshot, and Windows stock on 19045) |
| B6 | TI toggle (module-local helper) |

Build's "simple working version" (PIPELINE.md stage 3) is **B1–B5 with the KB seed**. B6 may land in Build or be finished in
Finalize. Either way 3-build.md says which.

## 3. Files

**In scope (create):**
```
modules/services/
  module.cpp          UI only. Own view class "Hadron.services.View" (as diskmap/runas/bios), tabs, lists, filter,
                      detail pane, in-view plan screen, TI switch, Workers, WM_MODULE_BASE+n messages.
  standalone.cpp      wWinMain -> RunStandalone(MakeServicesModule()); intercepts --live-test (like runas --launch-test).
  model.h/.cpp        Pure. SvcItem, StartCfg {start 0-4, delayedAuto, hasTriggers}, names for start/state/type,
                      filter and sort keys, image-path normalisation (\SystemRoot\, \??\, quotes, args, svchost -k).
  backend.h           Backend interface (r2 §6). Not named IServiceProvider (COM clash).
  backend_live.cpp    The ONLY file that calls SCM/registry write APIs. Write methods are inert until Arm().
  backend_fake.cpp    Fixture-backed; writes mutate memory; fault injection (deny, vanish, change, verify mismatch,
                      flush failure, crash after BEGIN k).
  hw.h/.cpp           Hardware evidence for Stripped/Minimum: CfgMgr32 over all devnodes incl. non-present,
                      DEVPKEY_Device_Service, Upper/LowerFilters (device + class), parent chain of the boot disk.
                      Live part behind the Backend; pure part testable from a fixture dump.
  store.h/.cpp        Data dir, text formats (snapshot, undo log), CREATE_NEW + FlushFileBuffers writer, apply lock.
  plan.h/.cpp         Pure plan building (single, preset, undo, resume) with every guard (§6), plus the apply engine
                      (runs on a Worker; talks only to Backend + store).
  ti.h/.cpp           Module-local TrustedInstaller impersonation helper (§7). No core change.
  kb.h/.cpp           Parser for the embedded services.db.
  recovery.h/.cpp     restore.cmd, README.txt text generation, stored-only zip writer (module-local; r2 G6).
  svc_compat.h        SDK constants MinGW lacks (SERVICE_USER_SERVICE etc.), module-local, guarded with #ifndef.
  data/services.db    Generated by tools/services/genkb.py, committed, byte-pinned in CI.
  resources/services.rc   SERVICES_KB RCDATA (bios pattern).
tools/services/kb.py, genkb.py, README.md      KB source, generator with rule-7 gate (imports bios checks; bios untouched).
tests/fixtures/services/                       snapshot.txt, first.txt, hw.txt, interrupted/, plus make_services_fixture.py.
tests/golden/services/                         restore.cmd, plan dumps per level per tab, snapshot round trip, crash-offer model.
tests/tools/services_check.sh                  Wine selftest + screenshots (bios_check.sh pattern).
docs/design/services.md                        Short design note (the builder writes it from this spec).
```

**In scope (edit):** `CMakeLists.txt` (`hadron_add_module(services ...)`, link advapi32, version, shell32, ole32, cfgmgr32,
setupapi; add `mod_services` to `Hadron`); `core/host/registry.h` and `registry.cpp` (one `MakeServicesModule()` line each,
after bios; a host registration, not an API change); `.github/workflows/ci.yml` (genkb selftest and `cmp`, services selftest
and screenshots in the Wine and MSVC jobs, the source guard in §9, and the `--live-test` step on windows-latest).

**Out of scope (must not touch):** everything else under `core/` (no API changes: no `core/sys/token.h`, no `win_compat.h`
edit, no `ui::Confirm`, no shared zip writer); `modules/runas` (read it, don't edit or link it); `modules/bios` and
`tools/bios/genkb.py` (import from it, don't change it); other modules; `CLAUDE.md`; `docs/PIPELINE.md`.

## 4. Core headers to use

`core/host/module.h`, `run.h`, `registry.h`, `cmdline.h`; `core/sys/sys.h` (IsElevated, ErrorText, ToExtended/ToShell, ModulePath),
`worker.h`, `settings.h` (remembered tab, sort, filter only; **never** the TI switch), `log.h`; `core/ui/theme.h`, `draw.h`,
`widgets.h` (ListView ×3: services, drivers, plan; SearchBox; DetailPane; ScrollBar).

`core/` gaps are filled module-locally, as diskmap, runas and bios do: buttons, chips and tabs; the switch; the zip writer;
the data-dir helper; re-parsing `GetCommandLineW()` for module CLI flags. r2 §0 lists docs that claim helpers that don't exist.
Don't assume `ui::Confirm()` or a core zip writer.

## 5. Behaviour

### 5.1 Lists (R1–R12 in r5)
- **Enumerate** with `EnumServicesStatusExW(SC_ENUM_PROCESS_INFO, SERVICE_TYPE_ALL, SERVICE_STATE_ALL)`. Then per item:
  `QueryServiceConfigW`, and `QueryServiceConfig2W` for DESCRIPTION, DELAYED_AUTO_START_INFO, TRIGGER_INFO, FAILURE_ACTIONS
  and LAUNCH_PROTECTED. Open the SCM with `SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT` and each service with
  `SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS` only. Reads never request write access. This runs on a Worker (about 650 items).
- **Services tab:** Win32 own/share-process services and per-user **templates**. `_LUID` instances are shown folded under their
  template, read-only; Hadron writes only templates (r1 §1.3). Detect them by the type bits, not the name alone.
- **Drivers tab:** `SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER`. State comes from the SCM's `SERVICE_RUNNING`
  (loaded). No `NtQuerySystemInformation`.
- **Columns:** Name, Display name, State, Start (Boot/System/Auto/Auto (delayed)/Manual/Disabled, with "+trigger" marked),
  Type, Account (services) or Group (drivers), Company (VERSIONINFO, second Worker pass), Changed (vs first snapshot),
  Preset target for the selected level, Basis. Every column sorts by clicking its header. A filter box, wheel and draggable
  scrollbar, keyboard navigation, focus rects (CLAUDE.md UI rules).
- **Pending reboot:** a registry `Start` that differs from the SCM's cached value shows "pending restart" (r1 §0.1).
- **Detail pane:**
  - the description from the service's registration, labelled "From the service's registration (not Hadron)" (proxy Q6);
  - then Hadron's KB text (what it does, what breaks if it's off, test-it note) with its **basis** shown;
  - image path, account, dependencies and dependents, failure actions, triggers;
  - current vs first-snapshot vs Windows-stock start type;
  - protection status: PPL, Tamper Protection or key DACL, and whether TI would help.
- **Protected marker:** items in the §6 never-touch or never-in-preset sets show "Protected: not in presets" with the reason.

### 5.2 Single-item actions
- **Start type** (services; drivers except Boot-start and never-touch): goes through the full plan, confirm, snapshot,
  log and recovery pipeline (§5.4). Choices are Auto, Auto (delayed), Manual and Disabled. Triggers are never removed;
  Disabled already overrides trigger start (r1 §0.6).
- **Start/Stop/Restart** (services only, proxy 1 and Q8): a confirm that lists the dependents that will stop, then a log line.
  No undo entry. Stop runs dependents first. Use `ControlServiceExW` with `SERVICE_CONTROL_STOP` and a timeout with progress.
  Not for drivers (no live load/unload, ever).

### 5.3 Presets (proxy Q1, Q2, Q4, Q7, Q9)
The same four levels on each tab, applied per tab. A preset changes **start type only** and takes effect at the next restart.
The UI says "Restart to apply". Hadron never reboots. There is no live stop cascade.

| Level | Membership |
|---|---|
| **Default** | Two choices. (a) **Restore first snapshot**: start types exactly as `first.txt` recorded. (b) **Windows stock (Win10 22H2 / 19045 only)**: per-item stock start type from the KB where the KB has one with a basis. Never for storage drivers (§0.5); items with no stock value are left alone. On other builds only (a) is offered, with the reason shown. |
| **Pure bloat removal** | KB entries tagged `bloat`: items **no** Microsoft source says to keep. Telemetry, Xbox, consumer extras, plus OEM extras only where the KB has a matching entry. Expect the Drivers tab to be near-empty at this level, and say so. |
| **Stripped** | Bloat plus KB entries tagged `stripped`: printing, Bluetooth, search, sync, remote access, Hyper-V/VM guest pieces and contested items. **Only where the hardware or use is absent**, per §5.5, with the evidence shown per row. |
| **Minimum** | Stripped plus KB entries tagged `minimum`: only what's needed to boot, network, play games and run Hadron stays on. Includes contested services, except the never-in-preset set. Drivers: no Boot-start drivers ever, System-start only where the KB says `sysok`, and never anything in the computed never-touch set. |

Each plan row shows the item, current → target, the level's reason and its **pbasis**. Contested rows say "contested" and link
the sources on both sides. Rows have an include toggle (Space or click) for per-item overrides. A dependency conflict
(disabling X while a not-disabled item depends on X) produces a warning, and the row is unticked by default.

### 5.4 Apply, undo, recovery (r2 §5, r5 §2.2, proxy Q5, Q10)
**Data dir:** `%ProgramData%\Hadron\services\` (proxy Q10). On creation, set its DACL to Administrators and SYSTEM full
control, inheritable, protected. Crash dumps stay in `%LOCALAPPDATA%` (CLAUDE.md). In selftest and screenshot mode the root
is injected (a temp dir or the fixture dir), and ProgramData is never touched.

**Files** (all `CREATE_NEW`; never overwritten; `<stamp>` = `yyyyMMdd-HHmmss` with a `-N` suffix on collision):
`first.txt`, `snap-<stamp>.txt`, `undo-<stamp>.log`, `recovery\<stamp>\{restore.cmd,README.txt}` and `recovery-<stamp>.zip`,
plus `recovery\first\...` and `recovery-first.zip`. The formats are in r2 §5: header `HXSVC\t1\t<kind>`, UTF-8, TAB-separated,
bios escaping, unknown lines skipped, 16 MB cap, and a CRC per undo-log record (r5).

**First run:** the first time live data loads with no `first.txt`, write it atomically (temp + flush + `MoveFileExW` without
`MOVEFILE_COPY_ALLOWED`; or `CREATE_NEW` direct), read it back, and record the machine GUID. A corrupt `first.txt` is renamed
aside, never overwritten.

**Apply** (Worker; after the user confirms the in-view plan):
1. Take the lock: named mutex `Global\Hadron.services.apply`, plus the log opened with share mode 0.
2. Write `snap-<stamp>.txt` and flush.
3. Write the recovery folder and zip, and flush. **If anything in 1–3 fails, abort and change nothing.**
4. Create the undo log: header, `B`, every `P` row; flush.
5. For each row, in dependency order (dependents before what they depend on, for disables):
   re-read; skip-and-log if it changed since the plan or vanished; write `I` and flush; call SCM (as TI if the row is marked
   and the toggle is on); re-read; write `D` and flush. A per-item error doesn't stop the run. A log write or flush failure
   aborts it.
6. Write `E` and flush. Restore the TI service state. Post the summary.

Cancel is honoured between items only. Undo and Resume are new plans with their own confirm and log. Undo restores only the
fields Hadron changed, and tells "value absent" from "value is 0" (r1 §0.7).

**Startup:** a log with `B` and no `E` shows a banner: "An apply was interrupted (<stamp>): Undo / Resume" (r2 §5). A torn
last record is ignored, because `I` is flushed before the write.

**restore.cmd** (pure generator, golden-tested; r1 §6.3, r2 §5, r3 §4, r4 §1):
- ASCII only, plus `.reg` (UTF-16) files for non-ASCII names.
- The target is `%~d0` (the script lives on the Windows volume) or `%1`. Refuse if `<drive>\Windows\System32\config\SYSTEM`
  is missing. Never loop over drive letters; the owner has a second Windows install.
- `reg load HKLM\HXOFF`, read `Select\Current`, `reg query`-guarded `reg add` of Start and DelayedAutostart per row (never
  create keys), then `reg unload`.
- README.txt covers: boot to WinRE → Command Prompt, BitLocker unlock first (`manage-bde -unlock`), and the exact command.
  A second variant for Safe Mode checks admin with `fltmc` (rule 4), never `net session`.
- Only start types are restored offline.

**Rule 6 reading** (logged): rule 6 governs scripts delivered to the owner as downloads. The recovery script Hadron writes on
his own disk at runtime must run from WinRE, which can't unzip, so it's written as a plain folder **and** as a .zip. If a later
stage reads rule 6 as covering local runtime files, the fallback is the zip plus the exact typed commands shown in the UI and
in README.txt beside the zip.

### 5.5 Hardware evidence for Stripped (proxy Q9, r4 §7)
- Enumerate all devnodes, present and non-present, with CfgMgr32. For each, read the service, the device and class filters,
  the parent chain and the last-arrival date.
- A driver or service is "hardware absent" only if **no devnode has ever referenced it**.
- Hot-plug classes are never auto-removed for being absent today: USB storage, Bluetooth, game controllers/HID, printers,
  USB audio, imaging.
- Services are mapped to hardware through device-interface triggers (`SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL`) and KB
  `hw=` hints.
- The plan row shows the evidence ("no devnode for class Bluetooth ever seen").
- On the owner's 14700KF, iGPU (`igfx*`) and Intel display audio (`IntcDAud`) are the clean absent examples. Expect the
  Wi-Fi, Bluetooth and LAN names to differ per driver version, so never hard-code them (r4 §3).

### 5.6 TrustedInstaller toggle (rule 4, proxy 2, Q3; r1 §3.4, §5; r5 §2.3)
- Off at every launch. Never persisted in settings. Its label says what it unlocks (key and SCM DACL refusals) and what it
  doesn't (Defender, PPL, Tamper Protection).
- **Plan marking:** `CanWrite` opens the service with `SERVICE_CHANGE_CONFIG` as admin and changes nothing. An access-denied
  result marks the row "needs TrustedInstaller". With the toggle off, those rows are excluded and the reason is shown.
- The apply Worker takes the TI token once per apply, and only if marked rows exist:
  1. enable SeDebugPrivilege on the thread (checking `ERROR_NOT_ALL_ASSIGNED`);
  2. impersonate SYSTEM from winlogon in this session;
  3. start TrustedInstaller if it's stopped, remembering that it did;
  4. `OpenProcessToken` on it, then `DuplicateTokenEx(SecurityImpersonation, TokenImpersonation)`.
- **Each marked SCM call** is wrapped in an RAII `SetThreadToken` swap. After setting, verify the impersonation level is
  `SecurityImpersonation` and the token has the TI SID group (r1 §0.5: it silently downgrades to identification otherwise).
  Log the identity in the undo log.
- **Never impersonate while writing Hadron's own files** (logs, snapshots, zip).
- Afterwards, stop TrustedInstaller only if Hadron started it. A crash marker in the data dir makes the next launch offer
  to stop it.
- The helper is **module-local** in `modules/services/ti.cpp`, written fresh from the technique in `modules/runas/launch.cpp`
  (impersonation-only: no SeTcb, no session move). Consolidating it into `core/sys/token.h` is a lead follow-up. runas's
  missing TI-state restore is also logged as a follow-up for the lead, not fixed here.
- No registry-write fallback for SCM-refused items in this run (r1 §3.5's `SeRestorePrivilege` + `REG_OPTION_BACKUP_RESTORE`
  route is noted for a follow-up). Never take ownership of a key or rewrite its DACL.

### 5.7 Drift
Compare live config with the last applied state (the newest log with `E`). Mismatches show "Changed by Windows or another
tool since Hadron applied", plus the event 7040 account where available. Never re-apply automatically. A refresh re-reads.

## 6. Guards (pure, in plan.cpp; each has a selftest case)

**Never touch** (no preset, no single-item change; the item shows as Protected with the reason):
- Start=0 (Boot) drivers;
- any service named in a class or device UpperFilters/LowerFilters value;
- the function driver of any devnode on the boot disk's parent chain;
- ErrorControl=3 (Critical);
- the curated floor from r4 §2: the ACPI/PCI/PDC stack, storage, the volume and FS stack incl. FltMgr and Wof, crypto/KMDF,
  pipes/mailslots/Mup, networking/WFP/BFE, USB/HID input, display, intelppm/processr/Power, TPM/tbs, mssmbios and MEI;
  hypervisor drivers while the hypervisor is running;
- RpcSs, RpcEptMapper, DcomLaunch, LSM, SamSs, the Plug and Play service, BrokerInfrastructure, StateRepository,
  CoreMessagingRegistrar, ProfSvc, Winmgmt, EventLog, CryptSvc, AudioEndpointBuilder, Audiosrv, Dhcp, Dnscache, nsi.

**Never in a preset (single item allowed where Windows allows):** WinDefend, WdNisSvc, WdNisDrv, WdFilter, WdBoot, Sense,
SecurityHealthService, wscsvc, TrustedInstaller, VSS, swprv; the filters FileInfo, FileCrypt, bindflt, wcifs and CldFlt; any
third-party or unknown item (no KB entry for this build); any item whose KB `builds` doesn't list the running build.

**Also enforced:**
- System-start drivers only with KB `sysok`, checked against the **live** start type;
- a dependency check (§5.3);
- per-user `_LUID` instances are never written;
- an item that vanished or changed since the plan is skipped;
- the plan never contains the TrustedInstaller service while the toggle is on.

## 7. KB (rule 7)
- Source `tools/services/kb.py`, generator `genkb.py` → `modules/services/data/services.db` (UTF-8 TSV, bios escaping),
  embedded as RCDATA. Fields per r2 §4, plus `stock` (19045 start type, optional; forbidden on storage drivers), `hw` (hardware
  hint) and `contested` (sources on both sides).
- **Every entry is tier 1 and needs `basis`.** Every entry with a preset level also needs `pbasis` for the membership claim.
  The generator fails on:
  - a missing basis or pbasis;
  - an unmarked number;
  - owner-specific text;
  - a target outside {3, 4};
  - `never` entries with levels;
  - a driver in a level without `sysok`;
  - `stock` on a storage driver;
  - a `bloat` entry that is `contested`.
- Bases: `published` (URL) mostly from r3's Microsoft sources (IoT Enterprise services page first, then the VDI guide/VDOT,
  the Restricted Traffic baseline and the Server 2016 guidance, each labelled with the OS it was written for); `reasoning`;
  `folklore` labelled "folklore (repeated, no evidence)". No numbers about savings anywhere.
- **Seed size for Build:** enough entries to populate every level on the Services tab and a few Drivers rows (the iGPU/display
  audio absent cases): roughly 60–100 entries. More is Finalize's job. kb-auditor audits the KB diff before Build hands off.
  r4 notes some Microsoft Learn URLs were built from the site's URL pattern; kb-auditor must open every URL.

## 8. Approved deviations and decisions (logged in DECISIONS.md, tag `proxy` unless noted)
1. All four preset levels in this run; stock table 19045 only; custom-preset file, before/after measurement and signer deferred (proxy Q1).
2. Presets change start type only, effective after restart; no live stop cascade (Q2).
3. Module-local TI helper; core consolidation is a lead follow-up (Q3).
4. Defender/Security stack, TrustedInstaller, VSS and swprv never in presets (Q4).
5. Recovery written as a plain folder plus a .zip; rule 6 read as covering downloads (Q5, research reading).
6. The registered Windows description is shown for every item, labelled as not Hadron's claim (Q6).
7. Contested services never in Pure bloat; Stripped/Minimum only (Q7).
8. Start/stop/restart get confirm + log, no undo entry (Q8).
9. Stripped removes only never-seen hardware; hot-plug classes never auto-removed (Q9).
10. Data dir `%ProgramData%\Hadron\services\` with an admin/SYSTEM-only DACL (Q10).
11. In-view plan screen instead of a modal dialog (research: no `ui::Confirm` exists; it also renders in `--screenshot`).

## 9. Acceptance checks
1. `cmake` configure and build with MinGW: `HadronServices.exe` and `Hadron.exe` both build; MSVC compile in CI.
2. `python3 tools/services/genkb.py --selftest` passes; regenerating `services.db` matches the committed file byte for byte.
3. `wine HadronServices.exe --selftest` exits 0, covering the r2 §6 list (pure text, KB, every §6 guard, plan → apply →
   undo round trip on the fake backend, crash after BEGIN k → Resume and → Undo, TI marking, the restore.cmd golden, the
   zip parse-back, hardware evidence from `hw.txt`, and a read-only live enumerate with more than 0 items). It never
   touches real services or ProgramData.
4. Golden plans: `--fixture tests/fixtures/services --dump-plan <level> --tab <services|drivers>` matches
   `tests/golden/services/plan-<tab>-<level>.txt` for all 8 combinations.
5. Screenshots under Wine (Baseline, dark), uploaded by CI: services tab, drivers tab, a detail pane on a KB item and on a
   protected item, a plan screen for Stripped, the interrupted-apply banner, the TI toggle on with marked rows. Fixture-only.
6. CI source guard fails on `DeleteService`, `CreateService`, `RegDeleteKey`, `RegDeleteTree`, `SetFirmwareEnvironmentVariable`
   or `NtSetSystemEnvironmentValue` under `modules/services/`.
7. windows-latest `--live-test HadronTestSvc`: the CI script creates a throwaway service with `sc.exe`; Hadron sets its
   start type, verifies, undoes and verifies; the script then deletes it. A TI variant is `continue-on-error`.
8. kb-auditor passes the KB diff with no open findings.
9. **Never** apply anything to the owner's real services in this pipeline (rule 5, idea risk). On his PC, only
   `--readonly --data-dir <project folder>`, or the `--live-test` dummy service with his OK for that action.

## 10. Verify on Windows before relying on them (from r1 and r3; Analyze tracks these)
- Whether a TI-impersonated `ChangeServiceConfigW` passes the SCM descriptor check (thread token used by the RPC).
- Whether `EnumServicesStatusExW` returns per-user templates with `SERVICE_TYPE_ALL` on 19045.
- `reg load` of the extension-less `SYSTEM` hive from WinRE.
- The class filter lists on the owner's PC (Volume, Disk, Keyboard, Mouse), his boot storage path (NVMe vs VMD/RST),
  LAN/Wi-Fi/BT service names and VBS state. **Read-only check, inside the project folder.**
- Every Microsoft Learn URL in the KB opens.

## 11. What to return (Build)
`3-build.md` (what it does, how and why, which of B1–B6 are done, the "Not done" list with the deferred items), the commit
hash, the screenshot paths under the shared `pipeline/svcdrv/`, the selftest output and the kb-auditor result.
