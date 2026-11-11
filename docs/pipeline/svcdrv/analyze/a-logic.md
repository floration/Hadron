# svcdrv Analyze: logic and performance (analyst `logic`)

Scope: `modules/services/{plan,model,hw,store,recovery,kb}.cpp`, plus the hot paths in `module.cpp` and `backend_live.cpp`.
Commit: `34ee85c`. Method: I read the code end to end. Counterexamples were run as a scratch probe on the fake backend under Wine
(`scratchpad/an/logic/probe.cpp`, built against the unmodified module sources; no tracked file was changed). Timings are
**measured under Wine 9 on this Linux box, not on Windows**.

## Findings

| # | Rating | Where | Finding | Verified |
|---|---|---|---|---|
| S1 | **serious** | plan.cpp:793-800, 846-850; recovery.cpp `RestoreCmd`/`RestoreOnlineCmd` | `recovery\first\restore.cmd` writes `Start` for **every** item in first.txt, including Boot/System drivers, the storage floor and the boot chain. The online Default plan blocks all of those (plan.cpp:238-241); the offline script doesn't. | code; the fixture's first.txt has 21 Boot + 2 System rows that all go into the script |
| S2 | **serious** (low likelihood) | plan.cpp:359-397 `BuildUndo` | An Undo plan runs no guards. A driver that is now Boot-start (or Critical, or never-touch) shows as a drift row that is unticked **but can be ticked**, and Apply then writes it. | probe P5 |
| S3 | **serious** (latent) | plan.cpp:468-494 `DependencyCheck`; module.cpp:763-772 `TogglePlanRow` | The dependency check makes one pass that depends on row order, and it isn't re-run when the user toggles a row. A ticked row can end up disabling X while a dependent stays enabled, with no warning. | probe P1, P2 (not triggered by today's fixture and KB) |
| M1 | minor | plan.cpp:650-664, 693 | `failedLowering` only records `fail`. A dependent that is *skipped* (because its own dependent failed, or it changed or vanished) doesn't protect what it depends on, so the guard only reaches one level. | probe P3 |
| M2 | minor | plan.cpp:230-231, 259-264, 384, 421-423 | Restore exactness: `op.delayed` is computed through `Delayed()`, which is false unless start==2. A leftover `DelayedAutostart=1` under Disabled/Manual turns a restore to Auto into **Auto (delayed)**. This affects Default-first, Undo, Resume and stock. | probe P4 (first: 4/1 → 2/1, recorded 2/0; undo: 2/1, recorded 2/-) |
| M3 | minor | plan.cpp:771-791 | After **Resume**, the rows that the interrupted apply finished are no longer offered by Undo and are no longer drift-tracked (`FindUndoable` and `LastApplied` require `E`). | probe P6 |
| M4 | minor | plan.cpp:750, 755-761 | Logs are ordered by the text of their local-time stamps, and `ResolvedBy` only looks *later* in that order. A resume or undo stamped earlier (DST fall-back, a clock correction) never resolves its log, so the banner comes back. `-10` also sorts before `-2`. | probe P7 |
| M5 | minor (latent) | plan.cpp:156-158 | A preset can **raise** Disabled → Manual when the KB target is 3. genkb allows target 3; no entry uses it today. | probe P8 |
| M6 | minor | plan.cpp:236-241 | Default-first can lower a driver that is **live** System-start without `sysok`. It's blocked only when the *first* value is <2. Spec §6: "System-start drivers only with KB sysok, checked against the live start type". | code |
| M7 | minor | plan.cpp:103-107; hw.cpp:296-298 | The filter and boot-chain guards fail open. If `ReadLiveHw` fails, or the boot disk isn't found (`ok` stays true), both sets are empty and nothing in the plan or detail pane says so. Single-item driver changes then rely on the curated floor only. | code |
| M8 | minor | plan.cpp:649-667 | No guard is re-checked at execute time. `RunApply` compares only start/delayed, so ErrorControl, protection, type or instance changes between plan and apply go unnoticed (CLAUDE.md asks core/fs for an execute-time re-check; same idea). | code |
| M9 | minor | store.cpp:136; module.cpp (never read) | `UndoLog::badRecords` is never shown. A mid-file CRC failure on a `P` record silently drops that row from Undo and Resume. | code |
| M10 | minor | module.cpp:763-772 | Warnings go stale after a toggle. Golden Stripped: SSDPSRV says "upnphost depends on it and stays enabled" even after the user ticks upnphost. | golden + code |
| M11 | minor | plan.cpp:780-782 | `TiLeftRunning` only checks the newest log. Any later apply hides a TI crash marker. | code |
| M12 | minor | hw.cpp:289-294; kb.py vmguest entries | `vmguest` uses only the SMBIOS vendor strings. A KVM or cloud guest with custom strings gets hyperkbd, HyperVideo and vmic* offered as "not a VM". Use devnode evidence (`self`) as well. Not a risk on the owner's bare-metal PC. | code |
| M13 | minor | plan.cpp:304-305, 315 | `BuildSingle` has a dead duplicate branch (`target.start < 2` twice) and reads `r.driver` after `std::move(r)`. It works because the member is a bool. | code |
| F1 | minor (perf) | module.cpp:609-628 `Refilter` | The sort comparator rebuilds key strings on every comparison. Sorted by the Hadron column, that's 12,814 `CheckGuard` calls per keystroke. | Wine: **23 ms** vs **1.5 ms** with the keys computed once (755 items) |
| F2 | minor (perf) | module.cpp:705-731, 1448; plan.cpp:376, 413, 449 | SCM calls run on the UI thread. `BuildFrom` → `MarkTi` (an OpenSCManager + OpenService per row), `BuildUndo`/`BuildResume` (a full `ReadOne`, about 8-10 RPCs, per row), `BuildControl` (Dependents), and `WriteAccess` from inside paint. | Wine: live `ReadOne` ≈2.5 ms/item, so a 66-row Undo/Resume ≈170 ms freeze under Wine; Windows not measured |
| F3 | minor (perf) | backend_live.cpp:36-163 | Enumeration makes redundant round trips (details below). | Wine: 2.5 ms/item (24 items) |
| F4 | minor (perf/spec) | module.cpp:448 | CompanyName (VERSIONINFO) is read in the load pass, before the list appears. Spec §5.1 asks for a second Worker pass. | Wine: 0.9 ms/item |
| F5 | minor (perf) | module.cpp:1352-1470 | `ItemDoc` is rebuilt on every paint: CheckGuard, HwEvidence, the dependents scan and two `PaintDoc` passes. | Wine: HwEvidence 0.38 ms (3,000 devnodes, `self` miss); dependents scan 0.05 ms. It's cheap, but it could be cached per selection |

Counts: **3 serious** (S1, S2, S3) and **18 minor** (13 logic, 5 perf).

## Invariants: held or broken

| Invariant | Result |
|---|---|
| Boot-start drivers never in a plan | **Holds for Preset, Default, Single and Resume** (P9 over the real KB and fixture, plus the code). **Broken for Undo** (S2). **Broken for the offline first-snapshot recovery script** (S1). |
| Never-touch / never-in-preset honoured through overrides | Holds: `TogglePlanRow` refuses blocked and `!canToggle` rows, and `MarkTi` makes TI rows non-toggleable while the toggle is off. Resume re-checks `neverTouch` against live. Undo doesn't (S2). Resume doesn't re-check never-preset or sysok (it inherits the original plan's checks; acceptable). |
| sysok gate for System-start drivers | Holds in presets (P9). `RunApply` requires live==before, so the gate does apply to the live start type. Default-first bypasses it (M6). Single bypasses it by spec §5.2. |
| `_LUID` instances never written | Holds: every builder skips or blocks `UserInstance`, and Undo/Resume names come from Hadron's own logs. There's no execute-time kind check (M8). |
| Dependency warnings correct | Direct dependents: yes. Group `+Group`: yes, conservatively (it warns even when other group members stay on). **Transitive: no** (S3). **Re-check after a toggle: no** (S3, M10). **Raising direction** (enabling Y while its dependency X stays Disabled, through Single, Undo or Default): **not checked at all**. Apply-time propagation is one level only (M1). |
| Levels nest (Minimum ⊇ Stripped ⊇ Bloat) | Holds: `TargetAt` is cumulative and genkb rejects softer later targets. P9 shows every row of each level is also a row of the next (services 15/56/66, drivers 0/6/7). Gates are identical across levels. |
| Default / first-snapshot restore exact | Start type: yes. Delayed flag: **no** when a stale DelayedAutostart=1 sits under a non-Auto start (M2). Absent vs 0 is treated as the same effect, as documented. |
| Undo replay order, idempotence | Order comes from `ApplyOrder` (raise rows: dependencies first), which is correct. Idempotent: rows already at `before` count as alreadySet, and a crash after `I` re-plans against live. Coverage is lost after Resume (M3). |
| CRC / parsing | Correct: torn last record (no LF) ignored; CRC over the raw body with CR stripped, so a CRLF-converted log still verifies; P rows with a bad `ParseCfg` counted. Bad records are not surfaced (M9). The snapshot has no CRC, but first.txt is read back byte for byte after writing. |

## Detail

### S1. The first-snapshot recovery script rewrites boot-critical start types (serious)
`EnsureFirstSnapshot` writes `recovery\first\` from `FirstRestoreRows(parsed)` (plan.cpp:848). `FirstRestoreRows` keeps every
non-instance item, including start 0 and 1 (plan.cpp:793-799). `RestoreCmd` then writes `Start` (and `DelayedAutostart` where it was
recorded) for each key that exists. The fixture's first.txt has 21 Boot and 2 System rows, and all of them go into the script.

The online "Restore first snapshot" plan refuses the same writes: never-touch items, `f->cfg.start < 2` ("Hadron never sets Boot
or System start"), and named never-preset items.

**How it bites:** after first run, Windows or the owner legitimately changes a boot-path start type. Examples: switching the
storage controller mode (AHCI ↔ RAID/VMD flips `storahci`/`iaStorVD` between 0 and 3), enabling Hyper-V or VBS, or installing a
vendor NVMe or RAID driver. Later something else breaks boot and he runs the first-snapshot script from WinRE. The script then
puts the old boot driver set back, which can give 0x7B on top of the original problem. It is also a ~650-key sledgehammer for
a "Hadron broke it" recovery.

**Fix:**
- Make `FirstRestoreRows` match the online Default rules: skip first start <2, the floor, the storage drivers (`IsStorageDriver`),
  the named never-preset set and the hypervisor drivers.
- Better: scope the offline first-snapshot recovery to items Hadron has ever changed. Keep it as a cumulative `recovery\hadron-all\`
  regenerated before each apply from the union of the logs' `P` rows.
- State the scope in README.txt.

### S2. Undo has no guards (serious, low likelihood)
`BuildUndo` (plan.cpp:359-397) never calls `CheckGuard`. Probe P5 used a log in which Hadron set `drvx` 3→4, after which Windows
made it Boot-start. The resulting row:
```
row: 0/- -> 3/- blocked='' canToggle=1 include=0 warn='changed since Hadron set it: now Boot (unticked; tick to restore anyway)'
```
One click ticks it, and Apply writes Start=3 to a Boot-start driver.

A second case: if the item's ErrorControl became Critical, or a Hadron update adds it to the floor, and it still sits at Hadron's
value, the row is **ticked** by default (P5b).

**Fix:** do what `BuildResume` does (plan.cpp:428-429): `Guard g = CheckGuard(live, c); if (g.neverTouch) blocked`. Also block when
`IsDriverType && live.cfg.start < 2 && u.before.start >= 2`.

### S3. DependencyCheck depends on row order and isn't re-run on toggles (serious, latent)
`DependencyCheck` reads `it->second->include` for planned dependents while it is still flipping `include` in the same loop.

Probe P1: `aaa` ← `bbb` (depends on aaa) ← `ccc` (depends on bbb, not in KB, stays Manual), with aaa and bbb both Bloat → Disabled.
- `bbb` is unticked correctly ("ccc (Manual) depends on it").
- `aaa` was processed first, when bbb still looked ticked, so it stays **ticked with no warning**.
- Apply result: `aaa=4/- bbb=3/- ccc=3/-`, so bbb is enabled on top of a disabled dependency.

Probe P2: two rows both ticked; the user unticks the dependent (all `TogglePlanRow` does is flip `include`). The dependency is
still disabled, with no warning.

Today's fixture and KB don't produce a 3-chain: a second pass changes nothing, and ticking every unwarned optional row leaves no
conflict (P9). Real Windows does have chains (SstpSvc ← RasMan ← RasAuto/RemoteAccess, SSDPSRV ← upnphost), and the KB grows at
Finalize.

**Fix:**
- Iterate `DependencyCheck` to a fixed point (loop until no row changes).
- Call it again, plus a `warning` recompute, from `TogglePlanRow`. Have `DependencyCheck` clear the warnings it owns first, which
  also fixes M10.
- In `RunApply`, refuse to disable X when any non-disabled item (live re-read) depends on X and isn't being disabled in this run.
  That covers both directions at execute time.
- Add the raising direction to the plan check: setting Y to Auto/Manual while a dependency is Disabled and not raised in the plan
  should warn.

### M1. Apply-time dependent protection reaches one level only
Probe P3 used the same chain, with `ccc`'s write not sticking (`verifyMismatch`). The result lines:
```
fail ccc: Windows did not keep this change (now Manual)
skip bbb: its dependent ccc could not be changed
ok aaa: Manual -> Disabled          <- bbb stays Manual on a disabled aaa
```
**Fix:** insert into `failedLowering` for every lowering row that ends `skip` or `fail` (plan.cpp:650, 652, 662, 693). For
"changed since the plan", insert only when the live value is now not Disabled.

### M2. The delayed flag is lost on restore to Auto
`StartCfg::Delayed()` is `start==2 && delayed==1`, so with any non-Auto start it's always false. The builders set `op.delayed`
only when `live.Delayed() != target.Delayed()`. Restoring 4/1 → Auto(not delayed) gives `op={2,-1}`, and the leftover
DelayedAutostart=1 takes effect.

Probe P4:
- Default-first: first.txt 2/0, live 4/1, plan row `4/1 -> 2/1`, result **Auto (delayed)**, reported "ok".
- Undo: before 2/-, live 4/1, result 2/1.

**Fix:** when `target.start == 2`, compare `target.Delayed()` with `live.delayed == 1` (the raw value) and set `op.delayed`
explicitly. Simplest: always pass `op.delayed = target.Delayed() ? 1 : 0` when `target.start == 2 && live.cfg.start != 2`.
Same change at plan.cpp:230, 264, 384 and 422.

On impact: the plan text does show "Auto (delayed)" in the After column, so this is visible, but the result doesn't match what
the snapshot recorded.

### M3. Resume orphans rows from Undo and drift
Log L1 is interrupted after row 1 (`D ok`). L2 = `resume:L1` finishes row 2. Then:
- `FindInterrupted` = none and `FindUndoable` = L2;
- L1 is never undoable (no `E`), and `LastApplied` skips it, so drift doesn't cover row 1.

"Undo last apply" therefore reverts only half of what the preset changed. Recovery is still possible through `recovery-<L1>.zip`
or Default-first.

**Fix:**
- Treat a log resolved by a finished resume as finished for `FindUndoable` and `LastApplied`.
- Make Undo of a `resume:X` log also include X's `D ok` rows (or offer "Undo X + its resume" as one plan).

### M4. Log ordering
`LoadLogs` sorts by `stamp` text (local time). `ResolvedBy` scans only `j > i`.

The probe used L1 = `20261027-014500` (interrupted, before the DST fall-back) and L2 = `20261027-011000` = `resume:` L1 (finished,
after it). `FindInterrupted` still returns L1. `NewStamp`'s `-N` suffixes also sort `-10` before `-2`.

**Fix:** have `ResolvedBy` scan all logs (the source names the stamp exactly), and order logs by a monotonic key (a UTC stamp in
`B`, or file creation time).

### M5–M13
- M5: add `if (s.cfg.start > t) { alreadyStricter++; continue; }`.
- M6: in Default-first, block when `IsDriverType && s.cfg.start < 2 && !(KB sysok)`.
- M7: when `!c.hw` or `hw.bootDisk.empty()`, add a plan note, and block single-item **driver** changes or require a second
  confirmation. Mitigation: boot-critical storage drivers are normally Start=0, so they're already never-touch.
- M8: before writing `I`, re-run `CheckGuard(live).neverTouch` plus `live.kind != UserInstance` (and `protect`) and skip on a hit.
- M9: surface `badRecords > 0` in the banner and notices.
- M11: scan every log for an unmatched `T started`.
- M12: combine `vmguest` with devnode presence (VMBus devnodes or `self`).
- M13: delete the dead line 305, and compute `bool drv = r.driver` before the move.

### Performance (all numbers measured under Wine 9, not Windows)
The probe used a synthetic ×5 copy of the fixture (755 items) and 3,000 synthetic devnodes:

| Operation | Wine time |
|---|---|
| `BuildPreset` ×3 (services) | 2.2 ms |
| `CheckGuard` ×755 | 1.1 ms |
| Refilter-style sort by the **Hadron column** (key recomputed per comparison) | **23 ms** (12,814 CheckGuard calls) |
| same, keys computed once | 1.0–1.5 ms |
| sort by name / filter pass (5×IContains) | 1.4–2.1 ms / 0.7–1.1 ms |
| `DependencyCheck`, 755 disable rows × 755 items | 3.7–5.4 ms |
| `ApplyOrder` 755 rows | 3–4.5 ms |
| `HwEvidence` `self` miss over 3,000 devnodes | 0.38 ms/call; 6× BuildPreset with 3,000 devnodes 8.8 ms |
| live `Enumerate` (Wine prefix, 24 services) | 61 ms = 2.5 ms/item |
| `FileCompany` | 0.9 ms/item |

**No O(n²) or worse path matters at 650–750 items.** `DependencyCheck` is O(rows × items × deps) and `RunApply`'s blocker loop is
O(rows² × deps), both a few ms. The fixes are about where work runs and about redundant work:
1. F1: precompute the sort keys once per `Refilter` (one `std::vector<std::wstring>` built before `stable_sort`). That's 15–20×
   faster for the computed columns (Hadron, Changed, Basis).
2. F2: build plans on a Worker. Or at least keep one `SC_HANDLE` for the whole plan in `MarkTi`, `BuildUndo` and `BuildResume`,
   and use a light status/config query instead of the full `ReadOne` (Undo only needs start + delayed). `WriteAccess` in paint
   should be filled by the load Worker.
3. F3: in `ReadOne`:
   - start `Config2` at 2–4 KB, since most descriptions are over 256 bytes and need a second RPC;
   - have `RegDword` return "absent" separately from "failed", and skip the `DELAYED_AUTO_START_INFO` RPC when the value is
     simply absent (the common case);
   - open the service key once for both `DelayedAutostart` and `Start`;
   - skip DESCRIPTION, TRIGGER, FAILURE and LAUNCH_PROTECTED for driver types.

   That's roughly 4–5 fewer round trips per item, about 2,500–3,000 fewer for the owner's ~650 items.
4. F4: move `FileCompany` to a second Worker pass after the list is posted (spec §5.1). Cache by path.
5. F5: cache the `ItemDoc` paragraphs per (selection, data generation) and rebuild only on a change.

## Needs Windows
1. **Load time** on the owner's PC: `HadronServices.exe --readonly --data-dir <project folder>`. Time from window to the list
   appearing, and to Company appearing (add a `Status()` with `GetTickCount64` deltas, or use ETW). Expect it to be dominated by
   ~650 × (≈10 SCM/registry calls + VERSIONINFO).
2. **Plan-open freeze:** time `OpenPlan` for Minimum (MarkTi `CanWrite` per row) and for Undo of a 60-row log (`Query` per row) on
   the UI thread.
3. **DelayedAutostart persistence** (behind M2): on a throwaway `HadronTestSvc`, set Auto-delayed, then
   `sc config HadronTestSvc start= disabled`, then `reg query ...\HadronTestSvc /v DelayedAutostart`. Expect 1 to remain. Then
   `sc config start= auto`, then `sc qc` (expect `AUTO_START (DELAYED)`). This confirms the stale flag comes back.
4. `QueryServiceConfig2(DELAYED_AUTO_START_INFO)` when the registry value is absent: confirm it reports FALSE, so the extra RPC
   in F3 can be dropped.

## Probe
- Source: `<scratch>/logic/probe.cpp`
- Binary: `<scratch>/logic/probe.exe`

It is built from the unmodified module sources, the fake backend and its own Wine prefix.
`WINEPREFIX=.../an/logic/wine WINEDEBUG=-all wine probe.exe` prints HOLDS or BROKEN per property.
Last run: 20 checks; the 13 BROKEN results are the findings above, and the 7 that HOLD are the invariants in the table.
