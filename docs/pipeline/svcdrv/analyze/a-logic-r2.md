# svcdrv Analyze round 2: logic and performance re-check (analyst `logic`)

Checked `module/services-svcdrv` at **a8303b7** against my round-1 findings (S1–S3, M1–M13, F1–F5). No tracked file was changed.

Method: I rebuilt the round-1 probe against the new sources and updated it for the API changes:
- `CheckGuard` now fails closed without a boot disk, so the probe's hardware info gets one;
- logs now need a machine and build, so the probe's logs carry them;
- the probe sets `Store::AcceptTokenUserAsOwner()`, the selftest's own Wine hook. Without it every `RunApply` aborts under Wine, and the "after apply" checks pass without testing anything. That was my first rerun.

I also added 11 round-2 checks and a random-plan fuzzer for `DependencyCheck` (`fuzz.cpp`, `fuzz_lower.cpp`). Timings are **measured under Wine 9, not Windows**.

Probe result: **31 checks, 3 broken**:
- M3 (2 checks): known and left, as Build said.
- R3: new, see N1 below.

## Verdicts

| Item | Round-2 id | Verdict | Evidence |
|---|---|---|---|
| S1 first-run recovery script | A2 | **Fixed** | R4: 96 rows written, 55 skipped of 160; no Boot/System value, no storage driver; with no hardware info, no drivers at all. Residual (minor): the exclusions are judged against the *first-run* state. A third-party storage driver outside `IsStorageDriver` that was Manual at first run and Boot later (e.g. `amdsata`, `secnvme`) would still be written as 3. |
| S2 Undo guards | A4 | **Fixed** | P5: the Boot-start driver row is blocked and can't be toggled; the Critical item is blocked; `LogRefusal` refuses a log from another machine or build. |
| S3 dependency check | A6 | **Mostly fixed; two residuals** (N1, N2) | See the A6 section. |
| M2 delayed flag | A7 | **Fixed** | P4: Default-first `4/1 -> 2/0`, applied result `2/0` (Auto); Undo result `2/0` against a recorded `2/-` (same effect). `SameStart`/`OpTo` compare the raw value. |
| M1 skipped dependent protects its dependencies | (A6) | **Fixed** | P3: `fail ccc`, `skip bbb`, `skip aaa`; nothing is disabled under an enabled dependent. |
| M3 Resume orphans rows from Undo and drift | — | **Not fixed (as Build states)** | P6 is still broken (`FindUndoable` and `LastApplied` require `E`). |
| M4 log ordering | — | **Fixed** | P7: a resume sorted first still resolves its log (`ResolvedBy` scans every log); a real `LoadLogs` puts `-2` before `-10`; there is a sequence number in `B`. An interrupted undo of an interrupted apply still leaves something offered (R5). |
| M5 presets raise Disabled→Manual | — | **Fixed** | P8, plus a "stricter" note. |
| M6 Default-first, live System-start driver without sysok | — | **Fixed** | plan.cpp, `BuildDefault`/stock `SysOk` check on the live start. Also new: a Win32 service with Start 0/1 is never-touch. |
| M7 guards fail open | — | **Fixed** | `CheckGuard`: drivers are never-touch when there is no hardware info or no boot disk, and `HwNote` adds a plan note. |
| M8 execute-time guard re-check | — | **Not fixed** (dropped in the consolidation, not in 4-analyze.md) | `RunApply` still compares only start/delayed before `I`. |
| M9 dropped CRC records | — | **Fixed** | `BadRecordsNote` in the Undo/Resume plans, which also covers the case where some planned rows couldn't be read. |
| M10 stale warnings after toggles | (A6) | **Fixed** | R2: after a 10-toggle sequence, warnings on ticked rows equal a fresh evaluation. |
| M11 TiLeftRunning | — | **Fixed** | Scans every log. |
| M12 `vmguest` uses only SMBIOS strings | — | **Not fixed** (not in the consolidated list) | hw.cpp unchanged. Owner's PC is bare metal. |
| M13 BuildSingle nits | — | **Fixed** | Dead branch gone; `drv` captured before the move. |
| F1 sort keys | — | **Fixed** | module.cpp:692-697 computes keys once per row. Probe: 16 ms per-comparison vs 0.9 ms precomputed. |
| F2 SCM on the UI thread | — | **Fixed for live** | Plans build on a plan worker; `WriteAccess` reads a map filled by the load worker. The fake still builds on the UI thread, which is fine. |
| F3 enumeration round trips | — | **Not fixed** | `ReadOne` is unchanged. Now *worse* at load: the load worker adds a `CanWrite` (OpenSCManager + OpenService) for every item, about 650 more SCM opens before the list appears. Minor; time it on Windows. |
| F4 Company in a second pass | — | **Not fixed** | `FileCompany` still runs in the load pass, before the list. |
| F5 detail pane per paint | — | **Partly fixed** | No SCM calls in paint any more; `ItemDoc` is still rebuilt on every paint (cheap). |

## A6 in detail

| Required | Result |
|---|---|
| Fixed-point loop | Present (`DependencyCheck`, plan.cpp:605-633), bounded at `2·rows+2` rounds. |
| Terminates on cycles | **Yes.** A true cycle (c1↔c2 plus an outside dependent) ends with both unticked and consistent. `ApplyOrder` terminates on a cycle. |
| Re-run on every toggle, including warnings | **Yes**: `TogglePlanRow` → `DependencyCheck(row)`, `depWarning` is recomputed, and rows the check unticked come back when the reason goes away (R2, P2). |
| Enable direction | **Yes**: a raising row warns when a dependency (or every member of a `+Group`) ends Disabled. |
| Skipped dependent protects its dependencies | **Yes** (P3). |
| Re-check at apply time | **Partly.** `RunApply` propagates failures and skips (M1), but it does **not** re-read live dependents. If another tool raises a dependent between plan and apply, the dependency is still disabled. Minor. |
| Chain aaa←bbb←ccc | Fixed (P1: both unticked; apply leaves all three Manual). |

### N1 (new, minor; a residual of A6): `MarkTi` unticks rows after the dependency check and never re-runs it
`BuildFrom` calls `MarkTi` last (module.cpp:803). A dependent that `MarkTi` excludes (it needs TrustedInstaller and the toggle is off; `include=false`, `canToggle=false`) still counted as "disabled" when the dependency check ran.

Probe R3: `aaa` ← `bbb`, both Bloat → Disabled, and the fake refuses the admin write to bbb. Result: `aaa include=1 dep=''`, `bbb include=0 canToggle=0`. Apply disables aaa while bbb stays Manual, with no warning.

It is narrow: it needs an SCM-refused dependent of a planned item. **Fix:** call `DependencyCheck(p, items)` at the end of `MarkTi`, or move `MarkTi` before the builders' check.

### N2 (new, minor): plans that both raise and lower can fail to converge and end inconsistent
A raising row Y that needs X, plus a row disabling X, makes each row's warning exist only while the other is ticked. The loop re-ticks rows "when the reason goes", so such pairs oscillate until the round bound. The state left at the bound can be inconsistent: a row stays ticked with no warning even though it disables something an enabled item needs, or raises something whose dependency stays Disabled.

Fuzz over random 2–6-item plans with random dependencies (`fuzz.cpp`, 60,000 iterations), using an independent evaluator of the final state:

| Plan kind | Plans | Inconsistent after build | Inconsistent after one toggle |
|---|---|---|---|
| Mixed raise + lower (Default-first, stock, Resume, Single) | 52,177 | **209 (0.4%)** | 259 |
| Lower only (every preset) | 36,803 | **0** | **0** |

Example (seed case 53):
- items: s0 (Disabled; deps s1, s2), s1 (Auto; deps s3), s2 (Manual; deps s0), s3 (Manual; deps s0);
- rows: s0 4→2, s1 2→4, s2 3→2, s3 3→2;
- result: `s2 3->2 include=1 dep=''` while s0 ends Disabled.

The simple pair on its own happens to end unticked and consistent (R1). Presets, the main path, are clean.

Cost: a non-converging pair inside a 68-row plan over 757 items runs to the bound in **1.7 s under Wine**, on the UI thread when it's a toggle.

**Fix:** if the loop reaches its bound without settling, finish with an untick-only pass (never re-tick) until nothing changes. The ticked set only shrinks, so it converges in at most `rows` rounds, and it ends consistent: in the last round no ticked row has a warning. Also cap the work: compute dependents once (reverse index) instead of scanning all items per row per round.

Not checked by `DependencyCheck` at all: Undo plans (`BuildUndo` never calls it). Undo raises rows back to their old values, and a row that is blocked or unticked for drift can leave a raised dependent without its dependency. Minor.

## Invariants that held in round 1: no regressions
P9 over the real KB and fixture:
- levels nest (services 15/56/66, drivers 0/6/7);
- no unblocked Boot-start driver, per-user instance, or driver without sysok in any preset plan;
- the dependency check is a fixed point on the fixture;
- ticking every optional row without a warning (and re-running the check) leaves no enabled dependent of a newly disabled item.

One apparent hit (`PimIndexMaintenanceSvc_4f2a1 → UnistoreSvc`) was my checker counting per-user instances; their templates are disabled in the same plan, so it was a false positive and the check now skips instances. TI rows still can't be toggled while the toggle is off. CRC and torn-record handling are unchanged.

## Timings (Wine 9, 755 synthetic items unless noted)

| Operation | Round 1 | Round 2 |
|---|---|---|
| 3× BuildPreset (services) | 2.2 ms | **39 ms** (fixed-point dependency check) |
| DependencyCheck, 755 disable rows × 755 items | 3.7–5.4 ms | **142 ms** (not a realistic plan) |
| Toggle on a 66-row Minimum plan, 755 items | n/a | 11.9 ms |
| Toggle on the fixture (66 rows, 151 items) | n/a | 1.65 ms |
| Non-converging pair in a 68-row plan (N2) | n/a | **1.7 s** |
| Sort by Hadron column, per-comparison vs precomputed (the UI now precomputes) | 23 / 1.5 ms | 16 / 0.9 ms |

The cost grows with rows × items × deps per round, times the number of rounds. It is fine at realistic sizes except in the non-converging case.

## Other notes
- The test-only hooks `Store::AcceptTokenUserAsOwner` and `InjectedReparsePoint` are set only in selftest.cpp (lines 447, 1363, 1376), inside `RunSelftest`. No CLI flag or other path sets them.
- An earlier fuzz crash was my harness's fault (`wmain` fell off the end without a `return`), not product code.

## Files
All under `<scratch>/logic/`:
- `probe.cpp`, `probe.exe`, `probe-r2.out` (the round-2 probe and its output)
- `probe_r1.cpp` (the round-1 probe)
- `fuzz.cpp`, `fuzz_lower.cpp` (the random-plan fuzzers)
