# svcdrv Analyze round 3: A8 / A9 re-check (analyst `logic`)

Branch `module/services-svcdrv` at **c49ca9e**. No tracked file changed.

I rebuilt my own probe and fuzzers (not the selftest's built-in fuzzer) against the new sources with `-DHX_SERVICES_TEST_HOOKS=1` and the new signatures (`BuildUndo(log,b,items,c)`, `MarkTi(p,b,tiOn,items)`). Timings are **Wine 9, not Windows**.

## Verdict
- **A8: fixed.**
- **A9: fixed.**
- **No new serious findings.**
- One new minor: a TrustedInstaller start is not claimed when the wait after Hadron's own start fails; details under "Build's minor fixes".

## A8: MarkTi re-runs the dependency check
- Probe R3 (aaa ← bbb, where bbb's admin write is refused and the TI toggle is off) now **holds**. aaa is unticked with a warning instead of staying ticked.
- New probe R6 builds aaa, bbb (TI-only, depends on aaa) and ccc (depends on aaa).
  - It rebuilds the plan with TI **off/on/off/on**, as the UI does on a TI toggle change:
    - off: `ccc=1 aaa=0(warned) bbb=0`;
    - on: `aaa=1 bbb=1 ccc=1`.
  - After each rebuild it runs all 27 three-toggle sequences of per-row toggles (flip, then `DependencyCheck(row)`).
  - **0 unwarned conflicts.**

## A9: always settles; the untick-only finish
- The loop detects a repeated state or the round limit, then runs untick-only rounds. Those rounds can only untick, so they end. A plan note and per-row warnings are added.
- `BuildUndo` now runs the check.

My fuzzer `fuzz3.cpp` checks each plan with an independent end-state evaluator. A ticked row with no warning fails if it disables something that is still enabled at the end, or raises something whose dependency (or whole `+group`) ends Disabled. A ticked row *with* a warning fails unless the user toggled it or deliberately kept it ticked over the warning (`depKept`).

The fuzzer also checks two more properties:
- the check never ticks a row the builder left unticked, nor (after a toggle) a row it hadn't unticked itself;
- it never changes any row's `after` value, so it can never raise a start type.

Four seeds × four modes, 15,000 generated cases each:
- modes: mixed or lower-only directions, random or chain dependencies;
- chain mode: up to 14 items, long chains, back edges that make cycles, `+group` dependencies, blocked rows;
- 6 random toggles per plan after the build check.

| Seed | Mixed random | Lower-only random | Mixed chains | Lower-only chains |
|---|---|---|---|---|
| 12345 | 13,735 plans: 0 bad build / 0 bad toggle; 377 fallbacks | 10,341: 0 / 0; 0 fallbacks | 14,408: 0 / 0; 972 fallbacks | 12,426: 0 / 0 |
| 1 | 13,782: 0 / 0; 387 | 10,393: 0 / 0 | 14,421: 0 / 0; 993 | 12,428: 0 / 0 |
| 777 | 13,743: 0 / 0; 329 | 10,378: 0 / 0 | 14,410: 0 / 0; 924 | 12,482: 0 / 0 |
| 20261111 | 13,772: 0 / 0; 354 | 10,388: 0 / 0 | 14,432: 0 / 0; 976 | 12,507: 0 / 0 |

Over all runs:
- **Ticked by the check from nowhere: 0.**
- **`after` changed: 0.** That covers the untick-only rounds too, so the fallback never ticks a row and never raises a start type.
- Fallbacks only happen in mixed plans, never in lower-only (preset) plans.

**Mutation check.** I made the harness force-tick one warned row and not mark it as the user's. The evaluator then flags 4,700–9,500 plans per mode, so it is not vacuous.

**My round-2 fuzzers rebuilt.** On seed 12345 they gave 209 / 259 before; now `fuzz.exe` gives `disable-side 0 raise-side 0 plans 52177 build 0 toggle 0`, and `fuzz_lower.exe` gives `plans 36803 build 0 toggle 0`.

**Worst-case time on the UI thread** (one `DependencyCheck` call):

| Case | Wine time |
|---|---|
| Non-converging pair in a 68-row plan over 757 items | 1.4 ms (was 1.7 s in round 2) |
| Toggle on a 66-row Minimum plan, 755 items | 0.85 ms |
| Toggle on the fixture | 0.16 ms |
| Adversarial 400-row plan: 100 oscillating pairs plus a 200-link chain that cascades one link per round (751 items) | build check 64 ms; worst toggle out of 40 is 45 ms |
| Same 400 rows with padding items, no long cascade | 3.8 ms; worst toggle 2.9 ms |
| Worst single call in the fuzzer (≤14 items) | 3.4 ms |

## Invariants: no regressions
Probe: **32 checks, 2 broken**. Both broken checks are **M3**, which is known and left for Design/Finalize. Everything else holds:
- level nesting;
- no unblocked Boot-start driver, per-user instance or no-sysok driver in any preset;
- A2, A4 and A7 cases;
- the M1 cascade;
- M4 ordering;
- M5;
- toggle consistency and current warnings;
- first-run recovery exclusions;
- ticking every unwarned optional row on the fixture never leaves an enabled dependent of a newly disabled item.

## Build's minor fixes (spot-checked)
- **`:put` skips keys that are now Start 0/1: correct.**
  - The golden `restore.cmd` reads the current Start with `reg query /v Start`, `tokens=1,3`, matching `0x0`/`0x1`, and jumps to `:sysnow`. The skip is counted (`%SYS%`) and reported in the Done line.
  - A key with no Start value writes as before.
  - Not run on real cmd.exe here (CI only).
- **Control reports its own start: correct as far as the claim goes.** `*ownStart` is true only when `StartServiceW` succeeds; `ERROR_SERVICE_ALREADY_RUNNING` leaves it false, and `TiService` sets `started_ = own`.
  - **New minor:** if Hadron's own `StartServiceW` succeeds but the following `WaitState` fails (timeout, or the service falls back to STOPPED), `Control` returns false and `Acquire` returns before `started_ = own` runs (ti.cpp:126-129). If TrustedInstaller then does come up, Hadron started it but doesn't claim it, so it is not stopped and no `T started` marker is written. Low impact: TrustedInstaller stops itself when idle.
  - Fix: return `own` through `ownStart` even on failure (it is already set before `WaitState`), and assign `started_ = own` before checking `Control`'s result.

## Still open (unchanged, as Build lists)
- M3: after Resume, the interrupted apply's rows lose undo and drift tracking.
- M8: no guard re-check at apply time.
- M12: `vmguest` relies only on SMBIOS strings.
- F3–F5: load cost. F3 grew in round 2 with the per-item write-access check.
- The apply-time re-read of live dependents (A6, "partly").

## Files
All under `<scratch>/logic/`:
- `probe.cpp`, `probe-r3.out` (the probe and its round-3 output)
- `fuzz3.cpp`, `fuzz3_mut.cpp` (the new fuzzer and its mutation check)
- `fuzz.cpp`, `fuzz_lower.cpp` (the round-2 fuzzers)
- `worst.cpp`, `worst2.cpp` (the worst-case timing plans)
