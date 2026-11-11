# svcdrv review: logic machine (Logic group)

Branch `module/services-svcdrv` @ b7cc215. Inputs: docs/design/services.md, docs/pipeline/svcdrv/1-idea.md, `git diff origin/main...HEAD`.
Scope traced: plan.cpp, store.cpp, recovery.cpp, model.cpp, kb.cpp, hw.cpp (plus the call sites in module.cpp and backend_live.cpp that feed them).

## How I checked
- I read every branch of CheckGuard, TargetGuard, the five plan builders, MarkTi, DependencyCheck, ApplyOrder, RunApply, LoadLogs/ResolvedBy/FindInterrupted/FindUndoable/UndoBase, ParseUndoLog, ParseSnapshot, EnsureFirstSnapshot/FirstRestoreRows, RestoreCmd/PutSub/UnicodeReg/WriteRecovery, HwEvidence and BootChain/FilterServices.
- I built `HadronServices.exe` (mingw, `-DHADRON_SERVICES_TEST_HOOKS=ON`) in my scratch dir and regenerated all 8 golden plans and the `delayedlinger` golden under Wine. **All 9 match byte for byte.** `--selftest` gives PASS, 189 checks.
- I wrote a throwaway harness (`scratchpad/logic-machine/h1.cpp`) that runs the real plan, store and recovery code against FakeBackend with a temp Store under Wine. Cases T1–T9 below are its output. Nothing touched a real SCM.

## Findings (most severe first)

### F1. Cat 2 (borderline 1): a Single change can lower a System-start driver that Hadron then can never put back
- **Where:**
  - plan.cpp:384-391 (BuildSingle allows it);
  - plan.cpp:115-118 (TargetGuard refuses the undo);
  - plan.cpp:387 (Single refuses System);
  - plan.cpp:318 (Default-first refuses it);
  - plan.cpp:1073 (`recovery\first` drops it);
  - recovery.cpp:286-289 (README F claim).
- **Case:** a System-start driver with no `sysok` and not on the floor, filters, boot chain or Critical lists (for example `rdbss`, `NetBT`, `Dfsc`, `bam`, `ahcache`, `CSC`, `Npsvctrig`). The detail pane offers Disabled (module.cpp:1203-1210, `ID_SET+4`).
- **Harness T1:** the Single plan is `[x] rdbss System -> Disabled` and the apply returns ok. Then:
  - Undo of that log: `[-] rdbss Disabled -> System — Protected: System-start driver without a curated sysok entry`;
  - Single back to System: `[-] Hadron never sets Boot or System start`;
  - Default (first snapshot): blocked, because `f->cfg.start < 2`;
  - `recovery\first`: the item is excluded (start < 2), and README F tells the user "Hadron never changes these".
- **Expected:** every change Hadron makes can be undone the way it was made (idea §7: "Undo restores the exact prior config"). Two fixes would do it:
  - BuildSingle refuses to change a live System-start driver without `sysok`, the same gate as presets and Default; or
  - TargetGuard lets an Undo restore System when the log row recorded `before.start == 1` and Hadron's own D-ok row lowered it.

  Either way README F should stop claiming "Hadron never changes" Boot/System values.
- **Actual:** the only way back is the per-apply `recovery\<stamp>\restore.cmd`, which does carry `Start=1`. The UI never points at it, and the "Undo last apply" row shows the change as Protected.

### F2. Cat 2: changes made by an interrupted apply are lost to Undo once a Resume finishes
- **Where:** plan.cpp:1039-1046 (FindUndoable skips logs without E) and plan.cpp:999-1011 (ResolvedBy counts a finished `resume:X` as resolving X).
- **Case:** apply X is interrupted after seq 1 is written. Resume R finishes the rest. The user then presses Undo.
- **Harness T2:**
  - after R: `FindInterrupted=-1 FindUndoable=1` (that is R);
  - Undo of R restores only SvcA;
  - afterwards `FindInterrupted=-1 FindUndoable=-1`, with live `SvcB 4/-` still Disabled. Nothing offers to undo it.
- **Expected:**
  - an undo of `resume:X` also undoes X's I rows (or offers X next); or
  - FindUndoable treats X+R as one undoable unit.
- **Actual:** X's completed rows are orphaned. Only Default (first snapshot) or X's offline `restore.cmd` reverts them.

### F3. Cat 2: "undo the undo" is reachable: the Resume of an interrupted undo is offered for Undo
- **Where:** plan.cpp:1042. Only a source starting with `undo:` is excluded, so `resume:<undo-log>` is not.
- **Case:** X is applied. Undo U (`undo:X`) is interrupted. Resume R (`resume:U`) finishes.
- **Harness T3:**
  - `FindUndoable=2 (source resume:20261111-110000)`;
  - the offered plan is `[x] SvcB Manual -> Disabled — undo: Hadron set it to Manual`, which re-applies X.
- **Expected:** the design says logs from an undo are never offered for undo, and an interrupted undo is never "undone" (it would re-apply X). A log whose source is `resume:U`, where U's source is `undo:`, is an undo log too.
- **Actual:** "Undo last apply" and Ctrl+Z re-apply part of the preset the user already reverted. ResolvedBy already handles this chain for resolution (line 1007-1009) but not for offering.

### F4. Cat 2 / 6: a missing first.txt is recaptured after Hadron's own changes and labelled "first"
- **Where:** plan.cpp:1087-1111.
- **Case:** first.txt is absent but undo logs exist. That happens when:
  - the first write failed (r.error is shown once, and CanApply at module.cpp:980 does not require first.txt);
  - the user removed it;
  - `--data-dir` changed.
- **Harness T8:** after a preset, EnsureFirstSnapshot gives `wrote=1 corrupt=0 note='' kind=first SvcA=4/-`.
- **Expected:**
  - the `undo-*.log` check and the "baseline (not first run)" kind, which today run only on the corrupt path (line 1103-1104, 1111), also run when the file is missing; or
  - applies are refused until first.txt exists.
- **Actual:** Default "restore first snapshot" then restores Hadron's own preset as "as recorded by the first-run snapshot" (basis `recorded`). That is a false claim (rule 7) and silently loses the real baseline. The old `recovery\first` (if any) is kept and now disagrees with first.txt.
- **Related (Cat 3):** `recovery\first` is only attempted when a new first.txt is written (line 1128). If that WriteRecovery failed, or stopped part-way and left the folder, it is never retried.

### F5. Cat 2: recognizer and adapter drivers (type 0x8, 0x4) are classed as services
- **Where:** model.cpp:66 (`IsDriverType` tests only 0x1|0x2); OnTab model.cpp:68; CheckGuard plan.cpp:163; BuildControl plan.cpp:422; backend_live Control.
- **Case:** Enumerate asks for SERVICE_TYPE_ALL (backend_live.cpp:208), which includes SERVICE_RECOGNIZER_DRIVER. `Fs_Rec` is registered as type 8.
- **Actual:**
  - such an item lists on the Services tab;
  - it skips the driver fail-closed check (no device list or boot disk), and the "drivers need sysok" preset gate;
  - BuildControl and LiveBackend::Control both let Start/Stop through (`IsDriverType` false). That breaks "drivers are never loaded or unloaded live" (idea §3).

  Fs_Rec itself is saved by the floor list and its System start. A third-party type-8 or 4 driver with Manual start is not.
- **Expected:** `IsDriverType` includes `SERVICE_ADAPTER | SERVICE_RECOGNIZER_DRIVER`, or anything that isn't SERVICE_WIN32*/user-service counts as a driver.

### F6. Cat 2 (low; defence in depth): the restore.cmd `rem` title passes CR/LF through
- **Where:** recovery.cpp:62-72 (`Rem` keeps control characters) and :134. UnicodeReg :301 writes the key name raw.
- **Case:** a Single plan title is `"Change start type: " + s.name`. A service name with LF (key names may hold any character but `\`) ends the `rem` line.
- **Harness T4:** the title `Evil\r\nreg add HKLM\HXOFF\x /f` produces an executable line 3, `reg add HKLM\HXOFF\x /f`, in restore.cmd. A name containing `]\r\n[...]` does the same in the .reg files.
- **Precondition:** an admin-installed service with a hostile name. That is not a privilege boundary, but the script runs as SYSTEM in WinRE against the offline hive.
- **Fix:** map every character below 0x20 to a space in `Rem`/`A`, and refuse (or list only in README) names with control characters or `]` in UnicodeReg.

### F7. Cat 2: at apply time, a failed lowering protects dependencies named directly but not group dependencies
- **Where:** plan.cpp:878-887. `o.deps` is compared by name, so `+Group` entries are ignored.
- **Case:** B depends on `+G1`, A is G1's only member, and both are in a preset to Disabled. B's write fails (access denied).
- **Harness T6:** `fail SvcB: Access is denied.` then `ok SvcA: Manual -> Disabled`. Live: A 4/-, B 3/- (B can no longer start).
- **Expected:** the same protection DepWarning gives at plan time (groupDependents, plan.cpp:597-599). When a dependent naming `+group` stays enabled, skip lowering a member of that group (at least the last member).
- Rare on stock Windows, but the code claims the M1 invariant.

### F8. Cat 2 (low): Undo and Resume rows only pass the never-touch guards, not the guards of the plan that wrote the log
- **Where:** plan.cpp:110-120.
- **Harness T7:** a CRC-valid, machine-matched log with source `preset:minimum:drivers` and one P row for a Manual third-party driver (no KB entry, no sysok). Resume gives `[x] thirdparty Manual -> Disabled`, ticked.
- **Context:** a preset could never have produced that row. Forging needs admin write access to the protected data dir, so this is defence in depth.
- **Suggestion:** for Resume of a `preset:` source, also require KB + ForBuild + (driver ⇒ sysok). Undo only raises back to recorded values, so it is lower risk.

### F9. Cat 2 (low): a forged `order` overflows
- **Where:** plan.cpp:798 (`l.order + 1`) and store.cpp:144 (`_wtoi`, no range check).
- **Harness T5:** a log with `order=2147483647` makes `order+1` signed overflow (UB in practice, wraps negative). The next apply gets order 1, so every later real log sorts *before* the forged one.
- **Fix:** clamp the order on parse (for example 0..1e6) and treat out-of-range values as a bad record.

### F10. Cat 3 (low): the undo-log parse is O(n²), and the size cap is the only bound
- **Where:** store.cpp:106-109 and :153/:156. `UndoLog::Row()` is a linear search per I/D record.
- **Case:** a 16 MB log of about 500k P rows followed by I/D rows means about 2.5·10¹¹ compares on the load worker. The page never finishes loading. (BuildUndo then also makes one SCM Query per row.)
- **Precondition:** admin write access or corruption.
- **Fix:** index rows by seq, cap rows at something like 10k (more than any real plan has), and drop duplicate seq.

### F11. Cat 2 (low): the offline first-run restore doesn't clear a DelayedAutostart that Hadron added later
- **Where:** plan.cpp:1077. `writeDelayed = s.cfg.delayed >= 0`, so an absent value is never written.
- **Case:** first.txt records `2/-` (plain Auto). Later a Single sets Auto (delayed), which writes `DelayedAutostart=1`.
- **Harness T9:** the row is `SvcA 2/- writeDelayed=0`. `restore.cmd` writes Start=2 only, so the service stays Auto (delayed).
- **Expected:** what the in-app Default does (A7: write 0 when the recorded value is absent and the target is Auto). For a Start=2 row, write DelayedAutostart 0 when the recorded value is absent, as UnicodeReg and README already do for writeDelayed rows.

### F12. Cat 2 (low): when the machine GUID is unreadable, every change can be applied but never undone
- **Where:** RunApply (plan.cpp:832) writes `env.machine` even when it is empty. LogRefusal (plan.cpp:123-126) then refuses every Undo or Resume.
- **Case:** `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` can't be read. CanApply doesn't check for this.
- **Expected:** refuse to apply (or warn in the plan) when the machine GUID or build is empty, because the undo log it writes can never be used.

### F13. Cat 4 (low): `vmguest` evidence ignores devnodes
- **Where:** hw.cpp:124-127, :300-303.
- **Case:**
  - `HyperVideo`/`hyperkbd`/`vmic*` decide only on firmware vendor strings (Microsoft+Virtual, VMware, QEMU, innotek, Xen, Parallels);
  - a Hyper-V guest with a custom SMBIOS, or an unrecognised cloud VM, gets them ticked for Disabled at Stripped. The guest keeps booting but loses its console display and keyboard;
  - a VMBus devnode using HyperVideo would be direct evidence and is not consulted.
- **Suggestion:** also return 0 (kept) when any devnode, present or phantom, names the item's service (the `self` test).

### F14. Cat 4 (nits)
- The README instructions are unquoted (recovery.cpp:236, :247). With a `--data-dir` that contains spaces, `D:\My Data\...\restore.cmd D:` fails as written. With a data dir on another drive or a UNC path, the "D:"+folder path is wrong. `restore.cmd` itself is fine: it only uses `%~d0`/`%1`.
- The snapshot format has no end marker or count (store.cpp:46-97). A first.txt cut at a line boundary parses as complete, and the missing items become "new since the first-run snapshot: left alone". The atomic write and read-back make this unlikely.
- `recovery-first.zip` gets a garbage DOS date from the stamp "first" (recovery.cpp:401-403). Cosmetic.
- BootDiskId uses only `Extents[0]` (hw.cpp:215). A Windows volume spanning disks protects only the first disk's chain. Boot-start (0) storage drivers stay protected regardless.
- design note: snapshot deps are `;`-joined (store.cpp:37), not `/`-joined as services.md says.

## Case matrix: what holds (traced, and harness or golden where noted)

| Item class | Presets (Bloat/Stripped/Minimum, both tabs) | Default first / stock | Single | Undo / Resume |
|---|---|---|---|---|
| Boot-start driver | blocked (CheckGuard first test; row is shown, never applied) | blocked | blocked | blocked (CheckGuard on live + target 0 refused) |
| System-start driver, `sysok` + build | lowered where the KB names it | lowering blocked unless sysok; System target blocked | allowed | may restore System |
| System-start driver, no `sysok` | blocked (`driver && !sysok`) | blocked | **allowed: F1** | **blocked: F1** |
| any driver while device list or boot disk unknown | blocked (fail closed) | blocked | blocked | blocked |
| floor / filter / boot chain / Critical | blocked | blocked | blocked | blocked |
| type 0x4/0x8 driver | **treated as a service: F5** | same | same | same |
| PPL / named never-preset (Defender, TI, VSS, swprv, FS filters) | blocked | blocked | allowed (TI blocked while toggle on) | allowed (by design) |
| service with dependents | DependencyCheck unticks; apply-time M1 skip | same | same | same; **group gap at apply: F7** |
| trigger-start | KB targets are 4 only, so triggers stop too; Single notes Manual ≠ Disabled | — | note shown | — |
| delayed-auto | `2/1 -> 4/1`; restore writes Start only, flag kept, so correct | raw delayed compared/written (A7, golden matches) | flag written before Start | raw compare; absent restored as 0 |
| per-user template / instance | template changed; instances folded; instance rows refused | same | instance blocked | instance blocked |
| not in KB / third-party | excluded, counted in a note | restored (first) / excluded (stock) | allowed | allowed (**F8** for forged preset resumes) |
| already at target / stricter | counted, never raised | counted | "Nothing to change" | counted |
| drift (Windows reverted) | — | — | — | the row is unticked with a warning; the list shows `*` (SameEffect) |

Other things that hold:
- **DependencyCheck:** it settles, and the not-settled path finishes with untick-only rounds. The goldens show the expected unticks (NcbService←CDPSvc, UnistoreSvc←PimIndexMaintenanceSvc, Fax/upnphost chains).
- **ApplyOrder:** lowering runs dependents-first, raising runs dependencies-first.
- **RunApply ordering is right:**
  - the lock comes first, then the snapshot, the recovery folder and zip, B/P, then per row: I flushed → write → re-read → D flushed, then E;
  - a failure in steps 1–3 changes nothing;
  - TI is released on every return path, the crash simulation aside;
  - Hadron's own files are refused while impersonating.
- **Undo log:**
  - a torn last record (no LF) and CRC failures are counted and ignored;
  - an E with a bad CRC leaves the log interrupted;
  - I-without-D rows are undone, and resumed as alreadySet when the write happened;
  - a foreign machine or build is refused.
- **Interrupted undo:** Undo restarts from X (UndoBase). Resume finishes from U. A finished undo:X or resume:U resolves both, and the depth-bounded recursion survives forged cycles.
- **restore.cmd:**
  - it targets only `%~d0`/`%1` and refuses without `Windows\System32\config\SYSTEM`;
  - it reads ControlSet from `Select\Current` (`set /a` turns `0x1` into 1, padded to 3 digits);
  - it skips missing keys and keys that are now Start 0/1, checks every `reg add`, and checks the unload;
  - it has no `( )` block near names, and the CmdSafeName allow-list keeps `% ! ^ & | < > ( ) " '` out;
  - it never deletes or creates a key; it only writes `Start`/`DelayedAutostart` values.
- **Custom-preset import/export:** not in this diff (deferred by the 2026-11-11 research proxy entry in DECISIONS.md), so there is no import parser or injection surface to review.
