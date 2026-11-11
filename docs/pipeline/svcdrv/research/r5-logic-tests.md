# svcdrv research, report 5: logic machine
Requirements, states, edge cases, failure modes, test plan. Sources read: CLAUDE.md, docs/pipeline/svcdrv/1-idea.md,
docs/PIPELINE.md, docs/ARCHITECTURE.md, docs/HISTORY.md, docs/design/runas.md, modules/runas/launch.cpp
(LaunchTrustedInstaller, lines 510-640), core/host/{cmdline.h,module.h}, the selftest/screenshot code in modules/*,
tests/tools/*.sh, .github/workflows (Linux Wine shots + windows-latest MSVC selftests).

Claims about Win32 behaviour below are tagged **[verify]** where I'm reasoning, not citing; the Windows-developer and
documentation reports should confirm or correct them. Nothing here is a measurement.

---------------------------------------------------------------------------------------------------------------------
## 0. Vocabulary
- **Item**: one SCM entry, keyed by its *service name* (case-insensitive ordinal compare, `CompareStringOrdinal(..., TRUE)`).
  Kind = `Service` (SERVICE_WIN32_*), `UserTemplate` (SERVICE_USER_SERVICE without the instance bit), `UserInstance`
  (name `<Template>_<hex LUID>`), `KernelDriver`, `FsDriver`.
- **Config** (what Hadron can write and must restore): `{ StartType ∈ {Boot,System,Auto,Manual,Disabled}, DelayedAuto: bool }`.
  Read-only context captured with it (for drift display and the recovery script, never written): ErrorControl,
  trigger count, DependOnService/DependOnGroup, account, ImagePath, Group, Tag.
- **Plan**: an ordered list of `{item, before (as read at plan time), target, reason, basis, flags}`.
- **Apply**: executing a plan; one undo-log file per apply.

## 1. Requirements (numbered, testable)

### Listing and display
- **R1** Two lists, Services and Drivers. Services = SERVICE_WIN32 | user templates; Drivers = SERVICE_DRIVER. Each item
  appears in exactly one list. Test: fixture with every type bit combination; counts match golden.
- **R2** Columns per idea feature 1; every column sorts on header click, the filter box matches name, display name and
  image path, case-insensitive; wheel + draggable scrollbar + keyboard nav + focus rect (CLAUDE.md UI). Test: screenshot
  fixture with `--sort`/`--filter`/`--select` args; selftest on sort comparator (stable, ties by name).
- **R3** Start type shown as one of Boot/System/Auto/Auto (delayed)/Manual/Disabled, plus a separate "trigger" flag.
  Delayed is shown only for Auto Win32 services (the flag is meaningless elsewhere; show it raw in the detail pane if set). [verify]
- **R4** Per-user services: the list shows the **template** as the configurable row and nests/links its live instances
  (`_LUID`). Config writes target the template only. Instances are state-only (start/stop allowed, config greyed). [verify:
  how SCM treats ChangeServiceConfig on an instance; registry `Start` on the template is the known-working path]
- **R5** Detail pane: KB description (stock items only), dependencies and dependents (both service and group deps),
  failure actions, and a row with Current / Stock(build) / Snapshot / Last applied by Hadron, each "n/a" when absent.
- **R6** Third-party/unknown items (not in the stock table for this build): no description, no preset membership,
  shown with image path, company, signer, start type. Text says "Not a stock Windows item on build N; Hadron has no
  explanation for it."
- **R7** ImagePath is shown raw and as a resolved path (`\SystemRoot\`→`%SystemRoot%`, `\??\`→ stripped,
  `system32\drivers\x.sys` relative to the Windows dir, quoted vs unquoted-with-spaces parsed best-effort). Paths over
  MAX_PATH are opened with `\\?\` for version/signer lookups (code standards). Resolution failure shows raw path + "could not
  resolve", never an error dialog.
- **R8** Every Win32 query uses the two-call size pattern and loops while `ERROR_INSUFFICIENT_BUFFER` (size can grow
  between calls). Test: fake provider returns a growing size twice.

### Read-only safety
- **R9** Listing opens the SCM with `SC_MANAGER_ENUMERATE_SERVICE|SC_MANAGER_CONNECT` and services with query rights only.
  `SERVICE_CHANGE_CONFIG`/`SERVICE_STOP`/`SERVICE_START` are requested only inside an apply/live-control worker.
  Test: code review + a fake provider that asserts the access mask per call path.
- **R10** A `--readonly` switch (and an automatic read-only mode when not elevated) disables every write path in code, not
  just in UI: the write provider is never constructed. Used for any run on the owner's PC during the pipeline (rule 5).
- **R11** A `--data-dir <dir>` override relocates snapshot/undo/recovery files (for fixtures and for owner-PC runs inside the
  project folder). Default `%LOCALAPPDATA%\Hadron\services\` (resolved with `SHGetKnownFolderPath(FOLDERID_LocalAppData)`).

### Presets and plans
- **R12** Four levels for both lists: Default (Restore snapshot | Windows stock), Bloat, Stripped, Minimum, plus custom.
  Each level is cumulative (Stripped ⊇ Bloat, Minimum ⊇ Stripped) for items it disables. Test: generator asserts subset relation.
- **R13** Preset targets resolve per item to `target | NoChange`. Unknown items → NoChange. Items absent on this PC →
  shown in a collapsed "not on this PC" group, never an error.
- **R14** **Never-touch set**, enforced in the plan builder (not only in preset data), for presets *and* per-item overrides:
  Boot-start drivers; drivers or services with ErrorControl = Critical; System-start drivers not in the curated list
  (presets only); the TrustedInstaller service while the TI toggle is on; any item on a hard deny list (RpcSs, DcomLaunch,
  RpcEptMapper, LSM, SamSs, ... list owned by the KB with basis). An override targeting one of these is refused in the plan
  with the reason shown. Test: every rule has a fixture item and a golden plan line "blocked: <reason>".
- **R15** Valid targets per kind: Boot/System only for drivers; DelayedAuto only with Auto on Win32 services; never set
  any item *to* Boot or System unless restoring a recorded prior value (undo/snapshot). Test: plan builder rejects.
- **R16** Plan = diff of live config vs target. Items already at target are listed in a collapsed "already set" count.
  Applying the same plan twice → second plan is empty and the UI says "Nothing to change" (idempotence).
- **R17** Every plan row shows: item, current → target, why (preset level / override / undo / snapshot), basis tag,
  and flags (needs TI, dependency warning, takes effect at next boot, drifted). Nothing changes until confirm (rule 1 analogue,
  idea feature 7).
- **R18** Dependency check (see §4.3): disabling X while a dependent Y (service or group member) stays Auto/Boot/System or is
  running is a **blocking conflict** in the plan unless Y is also in the plan with Disabled/Manual; no silent auto-expansion.
  The user can add the dependents with one action, which re-plans.
- **R19** Order: when lowering start (towards Disabled), dependents before dependencies; when raising (undo/restore),
  dependencies before dependents. If a dependent's change fails, its dependencies' disable is skipped ("skipped: dependent
  Y could not be changed").
- **R20** Hardware/use-gated Stripped items (printing, Bluetooth, Hyper-V...) carry the detection result in the plan row
  ("no Bluetooth radio found: SetupAPI class enumerate"), and detection failure = item excluded, not included.
- **R21** Presets change start configuration only; they do not stop services. The plan says "takes effect at next boot";
  an optional "also stop now" step is a separate live-control plan. (Open question Q1.)

### Apply, undo, recovery
- **R22** Before the first write of an apply: the first-run snapshot exists and parses (§2.4), a recovery .zip for this apply
  is written and flushed, and the undo-log header is written and flushed. Any of these failing aborts with zero writes.
- **R23** Per item: re-read live config; if it differs from the plan's `before`, skip ("changed since plan"); else write a
  BEGIN record {seq, item, before-as-read-now, target, needsTI} + CRC, `FlushFileBuffers`, then write, then re-read and
  compare (verify), then write DONE{seq, result, after} + flush. Then next item.
- **R24** Writes use the SCM API (`ChangeServiceConfigW` start type, `ChangeServiceConfig2W` delayed-auto). Direct registry
  writes are used only for the case SCM can't serve (user-service templates [verify]) and are marked "takes effect at next
  boot". (Q2 for the lead if the Windows report disagrees.)
- **R25** Undo-log writes and recovery-zip writes always run with impersonation reverted (as the Hadron admin token), so files
  are owned by and readable by the user.
- **R26** Cancel is honoured between items only, never inside one. After cancel: CANCELLED record, summary of done/remaining,
  offer Undo for what was done.
- **R27** `CanClose()` returns false while an apply, undo or live-control worker runs; closing shows "Applying (n/m); Cancel
  first". `Shutdown()` cancels and joins.
- **R28** Undo restores the exact `before` config for every DONE item of that apply (and every BEGIN-without-DONE item whose
  live config ≠ before), in reverse order respecting R19. Undo is itself an apply with its own log (so it is crash-safe and
  undoable).
- **R29** Undo of an item whose live config ≠ what Hadron wrote (drift, or a later apply) is **off by default** in the undo
  plan, shown as "changed since Hadron set it: now X", with a checkbox to include it.
- **R30** Undo history is a stack; undoing an older apply that overlaps items with a newer one shows the overlap and
  defaults those items to excluded.
- **R31** Startup (page create): scan the log folder; any log without END/CANCELLED → blocking banner "An apply did not finish"
  with **Undo** and **Resume** (rule 1 wording), plus "Show details". Nothing else writable until resolved; read-only browsing works.
- **R32** Resume re-plans the remaining items against live state (they may have changed, the build may have changed after a
  reboot); it is a new plan with confirm, not a blind replay.
- **R33** Recovery zip (rule 6): `<data-dir>\recovery\Hadron-services-recovery-<stamp>.zip` containing a `.bat` and a `.reg`
  that restore every item in the apply (and the full snapshot as a second option) to its prior Start/DelayedAutostart on an
  offline SYSTEM hive from WinRE. Script rules in §4.9. The UI shows the zip path after every apply and offers "Copy to…"
  (Save dialog; e.g. a USB stick), because `%LOCALAPPDATA%` is awkward to reach from WinRE.
- **R34** Only one apply/undo/snapshot-capture at a time per machine across the suite and every standalone: a named mutex
  `Global\Hadron.services.write` held for the whole operation, plus an exclusive (no-share) handle on the active log file.
  A second instance shows "Another Hadron is changing services" and stays read-only until it is released.
- **R35** Drift: an item has drift if Hadron's last DONE target for it ≠ live config. Checked on activation, on manual refresh
  and after every apply. Shown as a column/badge and a filter "Drifted". Never auto-reapplied.

### TrustedInstaller toggle
- **R36** Toggle off by default, not persisted across launches (each launch starts off). Turning it on shows what it does and
  which plan items need it.
- **R37** TI is used only for items whose admin write returned `ERROR_ACCESS_DENIED` (or that are pre-flagged as protected in
  the KB) and only for the write call; impersonation is per write, RAII, `RevertToSelf` checked, and verified by
  `OpenThreadToken` → `ERROR_NO_TOKEN` afterwards.
- **R38** With the toggle off, an access-denied item fails with "needs TrustedInstaller (toggle is off)" and the apply continues.
- **R39** TI service is left as found: record its state before; if Hadron started it, stop it after the apply (and after a
  crash, offer to on next start, §2.3). If it was Disabled, Hadron does not change its start type; the TI items fail.
- **R40** Privilege checks per rule 4: `AdjustTokenPrivileges` success **and** `GetLastError() != ERROR_NOT_ALL_ASSIGNED`;
  each failure names the privilege.

### Live control (services only)
- **R41** Start/Stop/Restart buttons enabled from `dwControlsAccepted` and current state; drivers have none.
- **R42** Stop with running dependents → show them and offer a plan that stops them first; never pass a "stop dependents"
  shortcut silently.
- **R43** Pending states wait on `dwWaitHint`/checkpoint with an overall cap (e.g. 30 s, cancellable), on a Worker; timeout is
  reported with the last state seen. Restart where stop succeeds and start fails says "left stopped".

### Honesty and measurement (rule 7)
- **R44** Every stock-table entry, preset membership and description carries a basis; the generator fails a tier-1 entry
  without one and any number without `owner-measured`/`published`. The UI shows the basis per row and in the plan.
- **R45** Before/after: RAM in use (`GlobalMemoryStatusEx`: total−avail), process count, running-service count; each sample has a
  timestamp, uptime and "measured on this PC, single sample". The before sample is stored in the apply log so an after sample
  taken after the next reboot can be paired. No predicted savings anywhere. Deltas are labelled "difference between two single
  samples, not an average".

### Build, platform
- **R46** Stock table keyed by build (CurrentBuild + edition family; 19045 = Win10 22H2 first). On a build with no table:
  "Windows stock" and all presets except "Restore snapshot" are disabled with the reason; listing and live control still work.
- **R47** If not elevated (`TokenElevation` false, e.g. Wine), the page is read-only with a banner; no relaunch (rule 4).
- **R48** CI grep stays clean (rule 2); add `NtSetSystemEnvironmentValue`-style guard is unaffected. Add a module-local check:
  no `SHFileOperation`, no `MOVEFILE_COPY_ALLOWED` (not needed here) and no `DeleteService`/`CreateService` in the source.

### Import/export
- **R49** Custom preset text format: versioned header, one item per line, `name<TAB>target[<TAB>delayed]`, `#` comments,
  UTF-8 with BOM, escapes for tab/newline/backslash. Parser rejects unknown versions, invalid targets (R15), never-touch items
  (R14, reported per line, rest still imported), duplicate names (last wins? no: reject the duplicate with line number).
  Round-trip write→parse is identity. Import via Browse (UIPI: no drag-drop only).

## 2. State machines

### 2.1 Per-item config state
Values held per item: `Live`, `Stock?` (table for this build), `Snap?` (first-run snapshot), `Target?` (selected preset /
override), `Applied?` (last DONE target by Hadron, from logs).

Derived flags (independent, all can co-exist):
| Flag | Condition |
|---|---|
| Unknown | no Stock entry for this build |
| New since snapshot | Snap absent, snapshot exists |
| Gone | in Snap/Applied/plan but not enumerable now |
| Modified (vs snapshot) | Snap present ∧ Live ≠ Snap |
| Non-stock | Stock present ∧ Live ≠ Stock |
| Pending | Target present ∧ Target ≠ Live ∧ not blocked |
| Blocked | Target present ∧ (R14 ∨ R15 ∨ unresolved R18 conflict) |
| Drift | Applied present ∧ Live ≠ Applied |
| Needs TI | admin write denied before, or KB marks protected |

Transitions of `Live` that Hadron causes: `Live=A` → (apply DONE, verified) → `Live=T, Applied=T`. Transitions not caused by
Hadron (Windows Update, WaaSMedic, another tool, user in services.msc) show up only as Drift on the next read.
Gone→present again (driver reinstalled) keeps history keyed by name.

### 2.2 Apply pipeline (one worker; UI only posts/receives messages; no dialogs on the worker)
```
Idle
 └─ build plan (worker, read-only) ─► PlanReady ─[user edits]─► PlanReady
      PlanReady ─[Cancel]─► Idle
      PlanReady ─[Confirm]─► Preflight
Preflight (worker):
   acquire Global mutex ──fail──► Idle ("another instance")
   ensure snapshot valid ──fail──► Idle (no writes)
   write recovery zip + flush ──fail──► Idle (no writes)
   create log, HEADER{ver, plan hash, machine GUID, build, boot id, pid, TI on?, before-measure} + flush ──fail──► Idle
   if TI needed & on: TI acquire (2.3) ──fail──► continue; TI items will fail individually
 ─► Item(i)
Item(i):
   cancel requested? ─► Cancelling
   re-read live ── gone ─► record SKIP(vanished) ─► next
               ── ≠ plan.before ─► SKIP(changed since plan) ─► next
               ── dependent skipped/failed (R19) ─► SKIP(dependency) ─► next
   BEGIN{seq,item,before,target} + FlushFileBuffers ──flush fail──► Abort (fatal: no log, no writes)
   write (admin) ── ACCESS_DENIED ∧ TI ready ─► write under TI impersonation
                 ── error ─► FAIL{seq, err text} + flush ─► next
   verify (re-read) ── ≠ target ─► FAIL-VERIFY{seq, observed} + flush ─► next   (item may be partially changed: undo covers it)
   DONE{seq, after} + flush ─► next
 next: i+1 < n ─► Item(i+1) ; else ─► Finish
Cancelling ─► CANCELLED{done k of n} + flush ─► Finish
Finish: TI release (2.3); END{counts} + flush; release mutex; re-read all; post summary
Summary (UI): done / skipped / failed with error text; "Undo this apply"; recovery zip path; drift recheck.
Abort (fatal log failure): stop immediately, release TI & mutex, summary says log incomplete → on next start handled by 2.2-recovery.
```
Partial failure policy: per-item errors never stop the run (they are independent changes); fatal = log write/flush failure,
SCM manager handle lost, mutex abandoned. Partial writes inside one item (start type written, delayed flag failed) are
FAIL-VERIFY and covered by undo because BEGIN holds the full before config.

**Crash/power loss recovery (on page create):**
```
scan logs ─► for each log without END/CANCELLED:
   parse records; last record torn (CRC/line incomplete) ─► drop it (BEGIN is flushed *before* the write, so a torn BEGIN means
                                                            the write never started)
   classify items: DONE (changed), FAIL (unchanged or partial), BEGIN-only (unknown: re-read live; == before → not changed;
                   == target → changed; other → "changed by something else")
   TI marker: TI_STARTED_BY_US without TI_STOPPED ─► include "stop TrustedInstaller" in the offer
   ─► banner: Undo (plan: restore before for changed items) | Resume (re-plan remaining items vs live) | Details
   Either choice runs a new apply with its own log; on its END the old log gets RESOLVED{by log id} appended and flushed.
   Log from another machine GUID or a different build: show it, warn, default to Details only.
```

### 2.3 TrustedInstaller toggle lifecycle
```
Off (default each launch) ─[user on + confirm]─► Armed
Armed ─[user off]─► Off
Armed ─[apply needs TI]─► Acquiring:
   query TI service (state, start type, PID)
     start type Disabled ─► Failed("TrustedInstaller service is disabled; Hadron won't change it")
     Stopped ─► StartService ─fail─► Failed(err) ; log TI_STARTED_BY_US + flush ; wait Running (cap 10 s) ─timeout─► Failed
     Running ─► (prior=Running)
   enable SeDebugPrivilege (thread) ─not assigned─► Failed(names it)
   SYSTEM via winlogon in our session (as runas does) + SeTcb ─fail─► Failed
   open TI PID; re-query service PID, must equal ─else─► retry once ─► Failed("TI restarted during acquire")
   duplicate as **impersonation** token (runas makes a primary one; this module needs TokenImpersonation) ─► Ready
Ready: per write: ImpersonateLoggedOnUser/SetThreadToken ─► write ─► RevertToSelf (checked) ─► Ready
Release (end of apply, cancel, abort, Shutdown):
   close token; if TI_STARTED_BY_US: ControlService STOP, wait Stopped (cap 30 s), log TI_STOPPED + flush
      stop fails/times out ─► summary warning "TrustedInstaller left running (it normally stops itself when idle [verify])"
   ─► Armed
Failed ─► items needing TI fail individually ("TI unavailable: <reason>"); Release still runs ─► Armed
Crash while Ready ─► token dies with the process; on next start TI_STARTED_BY_US w/o TI_STOPPED ─► offer stop (2.2 recovery)
```
Invariants: never write log/zip files while impersonating (R25); the UI thread never impersonates; TI token never outlives the apply.

### 2.4 First-run snapshot capture
```
Page create ─► data-dir/snapshot.txt exists?
  no:
    logs exist in data-dir? ─yes─► Warn: "Hadron changed services before but has no snapshot"; offer capture as
                                    'baseline (after Hadron changes)' explicitly labelled, never as first-run
    ─no─► Capture (worker, under the Global mutex): enumerate all items + configs (+ machine GUID, build, UBR, time)
          ─► write snapshot.txt.tmp ─► FlushFileBuffers ─► MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) ─► read back + parse
          ─► compare to in-memory ─ mismatch ─► delete nothing; mark "snapshot unverified", block applies
          ─► Captured (also write snapshot recovery zip)
          enumeration error for some items ─► record them as "unreadable" in the snapshot; snapshot still valid for the rest
  yes ─► parse ── ok, same machine GUID ─► Ready
               ── ok, other GUID ─► "snapshot is from another PC": Restore snapshot disabled
               ── corrupt ─► block applies; offer "Show file" and "Capture new baseline" (labelled not-first-run). Never
                             overwrite silently; keep the corrupt file (rename to .corrupt-<stamp>, never delete).
```
The first-run snapshot is write-once: Hadron never overwrites it. A newer "baseline" is a separate file.
Feature update (build changed since snapshot): snapshot items that no longer exist are listed as Gone; new ones have no Snap;
Restore snapshot only touches items present in both.

## 3. Edge cases (and the required behaviour)
| # | Case | Behaviour |
|---|---|---|
| E1 | Per-user instance `CDPUserSvc_4f2a1` | Key by template `CDPUserSvc`; strip suffix only when the item has the instance type bit (not by regex alone: third-party names can contain `_hex`). Snapshot/presets/logs never store instance names. Instances change every logon. |
| E2 | Item vanishes between plan and apply | SKIP(vanished); summary lists it; not a failure. |
| E3 | Item appears between plan and apply | Not in plan → untouched. Next plan includes it if a preset names it. |
| E4 | Disable X with Auto dependent Y | Blocking conflict (R18); offer "also disable Y". |
| E5 | Group dependency (`+NetworkProvider`, driver groups) | Expand group membership via each item's `Group` value; same conflict rule. |
| E6 | Auto service already depending on a Disabled one (pre-existing) | Shown as a warning row in the list ("will fail to start"), not caused by Hadron; plan doesn't fix it unless asked. |
| E7 | ERROR_ACCESS_DENIED | TI off → FAIL "needs TrustedInstaller"; TI on → retry under TI; still denied → FAIL "denied even as TrustedInstaller (protected process / Tamper Protection?) [verify]". |
| E8 | Delayed-auto + triggers | Snapshot both; changing Auto→Manual leaves triggers (trigger-start services may still start: plan note "has start triggers; Manual does not stop trigger starts, Disabled does" [verify]). Restoring Auto(delayed) writes start type then delayed flag; verify both. |
| E9 | Boot-start driver | Never in presets, overrides refused (R14). Undo/snapshot restore *to* Boot allowed only if the before value was Boot. |
| E10 | ErrorControl Critical | Never touched (R14). |
| E11 | Unknown third-party (anti-cheat, RGB, OEM) | No preset membership; override allowed except R14; plan basis "user override". |
| E12 | Build differences | Stock table per build; no table → R46. Item in table but not on PC → "not on this PC". |
| E13 | Windows reverts a change | Drift (R35); KB note for known self-repairers (WaaSMedicSvc, wuauserv, UsoSvc) with basis. |
| E14 | Preset on an already-modified system | Plan is diff vs live; "already set" collapsed; undo returns to pre-apply live, not to snapshot (says so). |
| E15 | Plan applied twice | Second plan empty → "Nothing to change"; no log written. |
| E16 | Two Hadron instances (suite + standalone) | R34 mutex; reader instance refreshes after the writer releases (drift recheck). |
| E17 | Unelevated (Wine) | R47; also SCM under Wine may lack Config2 info / drivers → show "n/a", never crash. |
| E18 | Very long ImagePath / args | R7, R8; list ellipsizes, detail pane wraps; recovery script doesn't need ImagePath. |
| E19 | Non-ASCII names, localized display names, `@dll,-id` indirect strings | Wide strings throughout; snapshot/export UTF-8 BOM; recovery `.reg` is UTF-16LE with BOM ("Windows Registry Editor Version 5.00"); `.bat` stays ASCII and only loads/imports/unloads (no service names in the .bat). |
| E20 | Service marked for delete (`ERROR_SERVICE_MARKED_FOR_DELETE`) | FAIL with text; shown as "pending delete" in list. |
| E21 | Invalid target for kind (Boot on Win32) | Rejected at plan (R15); if SCM still returns ERROR_INVALID_PARAMETER, FAIL. |
| E22 | Hadron's own needs (TI service when toggle on, Winmgmt if used, etc.) | In never-touch list while needed. Minimum must keep what Hadron needs to run (idea). |
| E23 | Elevated as a different admin (over-the-shoulder) | `%LOCALAPPDATA%` is that account's: a second account would see "first run" on a modified PC. Mitigation: store machine GUID; also check HKLM marker? (Q3: per-machine data dir e.g. `%ProgramData%\Hadron\services` vs idea's LOCALAPPDATA.) |
| E24 | Snapshot copied from another PC / restored image | machine GUID mismatch → R-snapshot rules (2.4). |
| E25 | Clock change / DST | Stamps are UTC + monotonically increasing sequence; file names use UTC stamp + counter. |
| E26 | Disk full while writing log | Fatal, before any write of that item (flush fails) → Abort. |
| E27 | Item renamed by a driver update (same image, new name) | Treated as Gone + New; no fuzzy matching. |
| E28 | Reboot needed for effect | Every row "at next boot"; drift check after reboot compares Applied vs Live. |
| E29 | Safe Mode boot (`SafeBoot` registry) | Page shows a banner; applies allowed only for Undo/Restore (simplest safe recovery path). |
| E30 | Service config changed by GPO (`HKLM\SOFTWARE\Policies`) or security template | Drift may be Policy reapplying; note it as a possible cause, no claim. |

## 4. Failure modes and required UI responses
| Failure | Detection | UI response |
|---|---|---|
| Not elevated | TokenElevation | Read-only banner, writes disabled with reason (R47) |
| SCM open fails | OpenSCManagerW | Full-page error with ErrorText, Retry button |
| Enumerate partial | per-item Query* fails | Row shows "unreadable: <err>", rest works |
| Snapshot missing/corrupt/foreign | 2.4 | Blocking banner for applies; listing works |
| Recovery zip write fails | Preflight | Apply refused, error text, path |
| Log create/flush fails | Preflight / per item | Refused / Abort; summary "log incomplete" |
| Mutex held | Preflight | "Another Hadron is changing services"; Retry |
| Item changed since plan | Item(i) | Summary row "skipped: changed since plan", offer re-plan |
| Access denied | write | "needs TrustedInstaller" or "denied as TI" |
| Verify mismatch | re-read | "Windows did not keep this change (now X)" + included in Undo offer |
| TI acquire fails | 2.3 | Banner in summary naming step and error; TI items fail |
| TI not stopped | 2.3 Release | Warning line, not an error loop |
| Cancel | user | Summary + Undo offer |
| Crash/power loss | 2.2 recovery | Blocking banner Undo / Resume / Details |
| Unbootable after reboot | (outside Hadron) | Recovery zip; instructions shown at every apply summary and in the zip README |
| Import parse errors | parser | Per-line list; valid lines imported only after the user confirms |
| Worker timeout (pending state) | R43 | Last state shown, Cancel available |
All errors show `sys::ErrorText(GetLastError())`; never open a dialog from a worker (post to the HWND, the UI opens it).

### 4.9 Recovery script rules (generation is pure, so it is golden-tested)
- Zip contains `README.txt`, `restore.bat`, `restore-apply-<stamp>.reg`, `restore-snapshot.reg`.
- `restore.bat`: ASCII only, CRLF; checks admin with `fltmc` (rule 4; in WinRE it is SYSTEM anyway [verify that fltmc exists
  in WinRE; fallback: skip the check there]); finds the offline Windows by scanning drive letters C..Z for
  `\Windows\System32\config\SYSTEM` and skipping `X:` (WinRE); lists candidates and asks which if more than one; `reg load
  HKLM\HADRON_OFF <path>`; reads `HKLM\HADRON_OFF\Select\Current` and refuses unless it equals the control set number baked into
  the .reg (recorded at generation); `reg import`; `reg unload`; prints each step and stops on the first error. Never runs
  `bcdedit`, never deletes anything.
- `.reg` writes only `Start` and `DelayedAutostart` (DWORD) under `HKEY_LOCAL_MACHINE\HADRON_OFF\ControlSet00N\Services\<name>`
  for items whose recorded before ≠ target; deleting a value that did not exist before → write "-" only for DelayedAutostart if
  it was absent [verify that absence = 0].
- Never contains user-service instances; templates only.
- Test: golden byte-compare of the four files for a fixture apply; a Python checker parses the .reg and asserts each value.

## 5. Test plan

### 5.1 Architecture for testability
- `IServiceProvider` (read: Enumerate, QueryConfig, QueryConfig2, QueryStatus, Dependents, MachineGuid, Build) and
  `IServiceWriter` (SetStart, SetDelayed, Start, Stop) interfaces; `ScmProvider`/`ScmWriter` (real), `FakeProvider`
  (loaded from a fixture text file, same format as the snapshot plus status/deps) with **fault injection**: deny item N,
  return wrong value on verify, vanish item between plan and apply, change item between plan and apply, grow buffer size,
  fail flush after K records, "crash" after BEGIN of item K (stop executing, leave log open).
- `ITiSource` with a fake that succeeds/fails at each step and records start/stop calls.
- Log writer over an injectable file sink (real file in selftest; failing sink for fault tests).
- Pure modules: preset resolver, plan builder/diff, dependency checker, orderer, snapshot/export parser+writer, log parser,
  recovery generator, drift calculator, ImagePath normaliser, `_LUID` template mapper. No Win32 beyond strings.

### 5.2 `--selftest` (headless, runs on Linux/Wine and windows-latest; pattern of bios/diskmap: `checks/failed` count, exit 1 on fail)
1. Preset resolution: each level on fixture A (Win10 19045 stock-ish set) → golden target list; subset relation R12; unknown items NoChange.
2. Never-touch: Boot driver, Critical ErrorControl, uncurated System driver, deny list, TI service with toggle on → blocked with reason.
3. Valid targets R15 (all type × target combinations, table-driven).
4. Plan diff: already-modified fixture → only differences; idempotence (apply on fake, re-plan → empty).
5. Dependency: chain A←B←C, group deps, Auto-on-Disabled warning, ordering for lower/raise, skip-dependency cascade on failure.
6. Snapshot round trip: write→parse identity incl. non-ASCII, tabs/newlines escaped, 32 K-char ImagePath; corrupt inputs
   (truncated, bad version, bad CRC, duplicate names) rejected with line numbers.
7. Export/import round trip and rejection cases (R49).
8. Log: per-record CRC; torn last record ignored; classification DONE/FAIL/BEGIN-only with live re-read; RESOLVED marking.
9. Apply engine on the fake: success, deny w/o TI, deny with fake TI, verify mismatch, vanish, changed-since-plan, cancel after
   k, flush failure (abort with zero further writes), crash after BEGIN k → recovery offers Undo/Resume with exact item sets;
   undo restores byte-identical before-configs; undo-of-undo; overlap stack rule R30; drift-excluded default R29.
10. TI lifecycle on fake: was-running (no stop), was-stopped (start+stop), disabled (fail, no start-type change), stop failure
    warning, crash marker offered on next start; invariant: no log write while "impersonating" (fake asserts).
11. Recovery generator: golden compare of zip members; .bat is pure ASCII + CRLF; no `net session`; contains `fltmc`.
12. `_LUID` mapping: instance bit + suffix → template; third-party `Foo_1a2b` without instance bit stays itself.
13. ImagePath normaliser table (quoted, unquoted with spaces, `\SystemRoot\`, `\??\`, `system32\drivers\`, args, >MAX_PATH).
14. KB generator: missing basis on tier-1 fails; number without measured/published basis fails (rule 7).
15. Source scan (CI, grep): no `SetFirmwareEnvironmentVariable`, `NtSetSystemEnvironmentValue`, `DeleteService`, `CreateService`,
    `SHFileOperation`, `net session`.

### 5.3 Golden outputs, `tests/golden/services/`
- `fixture-a-plan-<level>.txt` (plan text renderer, one per level, both lists), `fixture-a-plan-conflicts.txt`.
- `snapshot-fixture-a.txt`, `export-custom.txt`.
- `recovery/` (README.txt, restore.bat, two .reg files) for a fixed stamp/control set (the generator takes stamp as input).
- `recovery-offer-crash-k3.txt` (rendered recovery banner model).
- Screenshots (fixed 1280x800 @96 DPI): see 5.4.
Fixtures in `tests/fixtures/make_services_fixture.py` (deterministic; includes non-ASCII names, a long ImagePath, a user
template + two instances, Boot/System drivers, a Critical one, a dependency chain, a group dep, a protected service flagged
needs-TI, a third-party driver, a pending-delete item).

### 5.4 `--screenshot` fixtures (Linux Wine + windows-latest, like diskmap/bios)
Args to add via RunCli/`rest`: `--fixture <dir>`, `--tab services|drivers`, `--preset <level>`, `--select <name>`, `--filter`,
`--sort`, `--view plan|summary|recovery`, `--ti on`. Shots: both tabs × Baseline/Console × dark/light; detail pane of a stock
item (basis visible) and a third-party item; plan view with a blocked dependency row; summary with mixed done/skip/fail;
crash-recovery banner; read-only (unelevated) banner; drift badge; empty/no-stock-table-for-build state; suite (`--module services`)
and standalone. All from fixtures, never live SCM (determinism and safety).

### 5.5 Real Windows only (VM or CI runner, **never the owner's PC**)
- windows-latest CI (Server 2022, not Win10 22H2: different service set) can run: selftest, screenshots, **read-only** live
  enumeration smoke test (`--readonly --data-dir`), snapshot capture into a temp dir. Optionally a disposable test service
  created by the CI script (not by Hadron) and changed by Hadron's apply engine end-to-end, then removed by the script.
- Throwaway Win10 22H2 VM (snapshot the VM first): apply each preset → reboot → check boot, network, a game launcher,
  Hadron runs → undo → reboot → full config equals pre-apply snapshot (script diff). Crash test: kill Hadron (`taskkill /F`)
  and hard power-off mid-apply → restart → Undo and Resume both. TI: WinDefend/WaaSMedicSvc change with toggle on, TI
  was-stopped/was-running variants, Tamper Protection on/off. Recovery zip: deliberately break boot (disable a required
  storage/filesystem driver on the VM), boot WinRE, run restore.bat, confirm boot. Drift: run Windows Update / WaaSMedic and
  observe drift badge. Per-user templates across logoff/logon. Two instances concurrently.
- Owner's PC (pipeline stages only): `--readonly --data-dir <project folder>` listing and screenshots only. No apply, undo,
  TI write, live start/stop, or recovery-script run without his explicit OK for that specific action (rule 5; idea Risks).

## 6. Open questions for the lead
- **Q1** Should presets also stop services now, or config-only + "at next boot" (my recommendation, R21)? Affects R45 pairing.
- **Q2** SCM API vs registry: confirm per-user templates need registry `Start`; everything else via SCM (R24).
- **Q3** Data dir per-user (`%LOCALAPPDATA%`, idea) vs per-machine (`%ProgramData%`): services are machine state; E23.
- **Q4** Is a confirm-only (no undo log) acceptable for live start/stop/restart, given the idea's "every change shows a plan"?
  I recommend a one-row plan + confirm and a log line, but no undo (a stop is not configuration).
- **Q5** TI impersonation helper: if it moves from `modules/runas` to `core/sys`, it's a core API change (lead only).
