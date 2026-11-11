# svcdrv: Analyze (stage 4)

Stage 4 (Analyze), 2026-11-11, on `module/services-svcdrv` at 34ee85c. Five analysts ran in parallel on a fresh MinGW
build under Wine 9 (fixture and fake backend only; nothing touched a real service). Selftest reproduced: 137/137 pass,
standalone and `--solo services`. Full reports and their harnesses are in `analyze/`:

| Analyst | Report | Serious | Minor |
|---|---|---|---|
| Principal Windows program developer | `analyze/a-win32.md` | 3 | 16 |
| Deep documentation researcher | `analyze/a-docs.md` | 0 | 14 |
| Bug sniffer (Wine, hostile fixtures) | `analyze/a-sniffer.md` (harness `analyze/tools/sniff.cpp`, `mkfx.py`, `mklogs.py`) | 5 | 8 |
| Logic and performance | `analyze/a-logic.md` (probe `analyze/tools/probe.cpp`) | 3 | 18 |
| Feature-by-feature vs the idea | `analyze/a-champion.md` | 0 | 16 |

Many findings overlap; the lists below are deduplicated. Verdict: **back to Build** for the 7 serious items. The
safety core held up well: the undo log, CRC and torn-record handling, crash-after-every-row recovery (36 runs), apply
lock, TI impersonation checks, and the preset invariants (no Boot-start, per-user or non-`sysok` item is ever
unblocked in a preset) all passed hostile testing.

## Serious (fix in Build, then send back to Analyze)

| # | Problem | Where | Found by | Fix |
|---|---|---|---|---|
| A1 | `(` or `)` in a service name breaks restore.cmd and restore-online.cmd. `%~1` expands before cmd parses the `if errorlevel 1 ( ... )` block, so `Intel(R) ...` closes it early: syntax error, script stops after `reg load` and before `reg unload`, and a re-run blames BitLocker. Wine's cmd tolerates it, so CI can't catch it. | recovery.cpp:72-79, `:put` at 119-122, 163-166; `CmdSafeName` | win32, sniffer | Rewrite `:put` with `goto` labels and no parenthesised blocks around `%~1`, and/or reject `(` `)` in `CmdSafeName` (they then go to the README's manual section). Add a golden with a `(R)` name. Verify on real cmd.exe in CI (windows-latest: run the generated script against a test hive or an echo-only variant). |
| A2 | The first-run recovery script writes `Start` for every item in first.txt, including Boot/System drivers, the storage list, the boot chain and protected services, which the in-app "Restore first snapshot" deliberately blocks. If storage changed since first run (AHCI/RAID/VMD switch, clone to NVMe), running it from WinRE can cause 0x7B. | `FirstRestoreRows` plan.cpp:793-800, 846-850 | win32, sniffer, logic | Apply the same exclusions as `BuildDefault` (CheckGuard, never-touch, Boot/System start) and list the skipped names in README.txt. |
| A3 | Hadron can stop a TrustedInstaller it didn't start. `LiveTi::started_` is never reset and the provider lives for the session, so after one apply that started TI, a later apply while Windows is servicing logs `T started` and stops TI. START_PENDING is also counted as "Hadron started it". | ti.cpp:117-127, 180-190; module.cpp:787 | win32 | Reset `started_` in Acquire and Release; set it only when `StartServiceW` itself succeeded from STOPPED; treat START_PENDING as someone else's. Selftest: two applies, TI already running before the second. |
| A4 | Undo plans run no guard. A driver now Boot-start appears as a tickable row and Apply writes it (`0/- -> 3/-`); an item now Critical or newly never-touch is ticked by default. Undo and Resume also don't check the log's machine/build, so a forged log with valid CRCs gives a ticked `disk Boot->Disabled`, `stornvme Boot->Disabled`, `RpcSs Auto->Disabled`. | `BuildUndo` plan.cpp:358-397 | sniffer, logic | Call `CheckGuard(live)` per row and block never-touch rows, as `BuildResume` does at plan.cpp:428; refuse logs whose machine or build doesn't match. |
| A5 | An existing `%ProgramData%\Hadron\services` is trusted as is: no owner/DACL check or reset, no reparse-point check (the header comment says it sets the DACL). Standard users can create folders in ProgramData, so one could pre-create it and plant undo logs (feeds A4) or edit restore.cmd, which later runs as SYSTEM in WinRE. | `EnsureRoot` store.cpp:254-265 | win32, docs, sniffer | On every open: reject a reparse point (`FILE_FLAG_OPEN_REPARSE_POINT`), check the owner is Administrators or SYSTEM, and (re)apply the Admin/SYSTEM-only DACL with inheritance protected; refuse to run writes otherwise and say why. |
| A6 | The dependency check runs once, depends on row order, and isn't re-run when the user toggles a row. Chain aaa <- bbb <- ccc with ccc kept Manual: bbb is unticked correctly but aaa stays ticked, and Apply leaves aaa Disabled under a Manual bbb that now can't start. Real chains exist (SstpSvc <- RasMan <- RasAuto). Turning an item on is never checked against disabled dependencies. A skipped dependent at apply time doesn't protect its dependencies either. | plan.cpp:468-494; module.cpp:763-772 | logic | Iterate to a fixed point; re-run on every toggle (warnings too, see SSDPSRV/upnphost in the Stripped golden); re-check at apply time; also check enable-direction. Add the chain as a selftest. |
| A7 | "Restore first snapshot", Undo, Resume and stock write a different value than recorded when a leftover `DelayedAutostart=1` sits under a Manual or Disabled service: recorded plain Auto is written as Auto (delayed), and the plan never settles. Hadron's own single change to Manual creates that state. Reproduced here: `--dump-plan default` on the sniffer's `delayedlinger` fixture gives `Spooler Manual -> Auto (delayed) (3/1 -> 2/1)` against a recorded `2/-`. | `BuildDefault` plan.cpp:271, `BuildResume` 403, `BuildUndo`, stock | sniffer, logic | Compare and write the raw delayed value, as `BuildSingle` already does; when restoring to Auto with recorded delayed absent or 0, write 0 explicitly. |

## Minor (Build may fix now or leave for Design/Finalize; each is in the analyst's report)

Correctness and safety
- Recovery scripts: DelayedAutostart `reg add` and `reg unload` errors are unchecked; README doesn't warn that TI-only rows will likely fail in Safe Mode and maybe WinRE. (win32, docs)
- TI left running if an undo-log write or flush fails after the token is acquired. (win32, sniffer)
- `--ti on` turns the TI toggle on at launch in live mode; spec says off at every launch. Limit it to fixture mode. (docs D2)
- Auto (delayed) on a service in a load-order group: Start=2 is written before the delayed flag, which Windows may reject, leaving plain Auto. Write the flag first or don't offer delayed for grouped services. (docs D1)
- Hardware guards fail open: device-list read has no retry when the list grows, and an unfound boot disk (RST/VMD, Storage Spaces) silently disables the class-filter and boot-chain guards. Show a notice and fall back to blocking. (win32, logic M7)
- Undo records failing CRC are dropped silently; show the count. (sniffer, logic M9)
- Logs ordered by local-time stamp text: DST/clock changes and `-10` vs `-2` can pick the wrong log or bring the interrupted banner back. Use UTC and a sequence number. (sniffer, logic M4)
- An interrupted undo hides the original interrupted apply. After Resume, rows already written by the interrupted apply lose undo/drift tracking. (sniffer, logic M3)
- `TiLeftRunning` checks only the newest log. (logic M11)
- Default-first can lower a driver that is System-start now without `sysok`; a Win32 service with registry Start 0/1 isn't guarded; a preset could raise Disabled to Manual (no KB entry does yet). (logic M5, M6, sniffer)
- `WaitState` waits the full 30 s when a service falls back to STOPPED after a start. (win32)
- Stop reason `PLANNED|MAJOR_NONE|MINOR_NONE` is never exercised in CI; fall back to `ControlService` on ERROR_INVALID_PARAMETER. (win32, docs D6)
- `--data-dir` is stored with the `\\?\` prefix (README prints `D:\\?\C:\...`); folder creation fails on UNC and on a prefixed path with a missing parent. TAB in a name becomes a space in logs. (sniffer)
- `HwEvidence` returns 3 ("kept") for an unknown hint but the preset builder treats 3 as ticked; unreachable today because genkb rejects bad hints. (docs D8)
- `BuildSingle` has a dead duplicate branch and reads `r.driver` after a move. (logic M13)
- Design note says Boot- and System-start drivers are never touched; code (correctly, per spec) blocks Boot-start only. Fix the note. Note lists a `control` undo source nothing writes. (docs D4, D5)
- Spec §5.5 partly done: trigger GUIDs are read but never mapped to hardware, devnode last-arrival date not read. Record as a deviation or build it. (docs D3)

Performance (Wine numbers, not Windows)
- SCM calls on the UI thread when a plan opens (write check per row, full re-read per row for Undo/Resume): ~170 ms freeze for a 66-row Undo; a ~270-row Minimum plan is worse. Move to the Worker. Closing during load blocks because the load worker never checks cancel. (win32, logic F2)
- Filter keystroke re-sorts with keys rebuilt per comparison: 23 ms vs 1.5 ms with precomputed keys. (logic F1)
- ~10 SCM round trips per item at load; 2,500–3,000 redundant calls on a 650-item machine. Company name read before the list shows (spec asks for a second pass). Detail pane rebuilt every paint. (logic F3–F5)

UI (for Design)
- U1: stale highlighted row after a tab switch while the detail pane says "Select an item" (module.cpp:834 vs 651).
- U2 (core, every module): focus rect drawn in the accent colour on an accent-filled row, so it's invisible (core/ui/widgets.cpp:208-211). Lead follow-up.
- Tab key reaches only list, filter and detail; chips, toggle and buttons are mouse-only (not a rule break).
- Selected row can scroll off-screen after a sort; sort arrow overlaps narrow headers; the Hadron column sorts alphabetically; the saved sort is a visible-column index that shifts with width.
- Type, Account and Company drop out below ~1700 px of list width, so they're hidden at 1200x800 and in the suite. No Image path column (detail pane only).
- "Hadron" column shows the lowest level, not the selected level's target, and ignores hardware (BluetoothUserService shows "stripped" while its detail says "kept by every preset").
- "Stop TrustedInstaller" plan's Why column just says "stop". No "no screen-reader support in v1" note anywhere (suite-wide).

KB
- All 47 sampled claims match their sources; no savings numbers anywhere. Switch TAMPER, ADDSERVICE (en-au), LOADORDER (en-in) and PROTSVC (old path) to en-us canonical URLs; the VDI guide moved under /previous-versions/. SysMain's "3 restarts" is a test protocol; kb-auditor should confirm it's fine. (docs D10, D11)

## Gaps against the owner's words (not bugs; recorded for Design and the Ideas follow-ups)
- **Default can't take him back to Windows defaults.** "Windows stock" has 0 of 109 rows, and "first snapshot" on his already-tuned PC records his tuned state. Best route inside the rules: capture the 19045 stock table from a fresh Win10 22H2 VM as a measurement with its conditions, or rename the chip and say plainly it isn't Windows' default.
- **Coverage:** the KB has 88 services and 11 drivers. RpcSs, DcomLaunch, Audiosrv and nearly every driver show "Not in Hadron's KB"; presets only touch KB items, so third-party/OEM extras are never in Pure bloat, Drivers' Pure bloat is always empty, and Stripped/Minimum touch at most 7 drivers.
- **TI toggle:** honest that it doesn't cover Defender, PPL or Tamper Protection, but the idea named WinDefend as its example.
- Already deferred in 3-build.md: custom preset export/import, measured before/after RAM and process count, signer column, Event 7040 account.

## Needs Windows (CI windows-latest, a throwaway VM, or read-only on James's PC inside the project folder)
This thread couldn't reach James's PC this round (that needs a message from him to start a session), so these are open.
1. A1 on real cmd.exe: run a generated restore script with a `(R)` name (echo-only variant). On James's PC, read-only: `reg query HKLM\SYSTEM\CurrentControlSet\Services | findstr "("`.
2. TI-impersonated `ChangeServiceConfigW` past WaaSMedicSvc's admin-refusing descriptor (VM only); whether the duplicated TI token can be queried as admin.
3. `EnumServicesStatusExW` with the full type mask returning per-user templates on 19045; `ChangeServiceConfigW` on PimIndexMaintenanceSvc/UnistoreSvc/UserDataSvc templates.
4. Stop reason accepted (Stop/Start round trip on the dummy service); Auto (delayed) on a grouped dummy (add `group=` to the CI `sc.exe create`).
5. Whether `DelayedAutostart` survives `sc config start= disabled` (premise of A7) and whether the delayed query reports FALSE when the value is absent.
6. WinRE: `reg load` of `config\SYSTEM` (Learn says `.hiv`), `Select\Current` parsing, `reg add` on WaaSMedicSvc's key as SYSTEM.
7. ProgramData: can a standard user pre-create `Hadron\services` (A5).
8. James's PC, read-only, `--readonly --data-dir` in the project folder: class filters, boot storage path, LAN/Wi-Fi/BT names, VBS state, load time and plan-open freeze, high contrast.
9. The MSVC compile: the three flagged risks look fine (clang `-fms-extensions -Wall -Wextra` is clean), but the first CI run is the real test.

## Hand-off
Back to **Build** for A1–A7. When they're fixed, Build sends it to Analyze again; the re-check will rerun the sniffer
and logic probes in `analyze/tools/` against the fixes plus the selftest and goldens.

## Round 2 (a8303b7), 2026-11-11

Re-checked by the Win32, bug-sniffer and logic analysts against Build's round-2 fixes (reports `analyze/a-*-r2.md`;
new probe and random-plan fuzzer in `analyze/tools/probe.cpp`, `fuzz.cpp`, `fuzz_lower.cpp`). Selftest 178/178
standalone and `--solo services`; the 8 plan goldens unchanged.

| Item | Result |
|---|---|
| A1 restore.cmd metacharacters | **fixed**: allow-list `CmdSafeName`, one `:put` with `goto` only, every `reg add` and `reg unload` checked; hostile names go to .reg/README. CI echo-only step runs the same `:put` text on real cmd. |
| A2 first-run recovery writes boot drivers | **fixed**: 96 of 160 fixture rows, no Boot/System, storage, floor or protected items; no drivers when hardware info is missing. |
| A3 TI stopped when Hadron didn't start it | **fixed**: claim reset per apply, set only after Hadron's own start from STOPPED; TI released on every return path. |
| A4 Undo unguarded, foreign logs | **fixed**: CheckGuard per row; wrong/missing machine or build refused for Undo and Resume. |
| A5 data folder trusted as is | **fixed** for the folder: reparse check on every component, owner Administrators/SYSTEM, protected DACL re-applied. Owner check needs a Windows run. |
| A6 dependency check | **mostly fixed**: fixed-point loop, terminates on cycles, re-run on toggle with warnings, enable-direction check, skipped dependents protect. Two leftovers, below. |
| A7 delayed linger | **fixed**: all 81 Default-first combinations, plus Undo, Resume and stock. |

Store's test hooks `AcceptTokenUserAsOwner` and `InjectedReparsePoint` are set only in selftest.cpp, which is reached only
through `--selftest` and exits before the GUI; the owner hook is set only under Wine. Not reachable in a release run.
(Minor: put both behind a selftest-only build flag so it's structural.)

### Serious, round 2 (back to Build)
Rated serious for the same reason as A6: Apply can write a configuration where an enabled service depends on one
Hadron disabled.
| # | Problem | Fix |
|---|---|---|
| A8 | `MarkTi` runs after the dependency check and never re-runs it. aaa <- bbb with bbb left out because it needs TI and the toggle is off: aaa stays ticked with no warning and is disabled under an enabled bbb. | Re-run `DependencyCheck` at the end of `MarkTi` (and on toggling TI). Selftest the aaa <- bbb(TI) case. |
| A9 | Plans that both raise and lower start types (Default-first, stock, Resume) can bounce until the round limit and end in a dependency conflict with no warning: 209 of 52,177 fuzzed mixed plans (0.4%) after build, 259 after a toggle; 0 of 36,803 lower-only preset plans. A non-converging 68-row plan also takes 1.7 s on the UI thread after a toggle. Undo plans don't run the check at all. | When the round limit is hit, finish with an untick-only pass (always settles, ends consistent) and show a warning; run the check on Undo plans too. Add `fuzz.cpp`'s failing seeds as a selftest. |

### Minor, round 2
- First-run recovery judges exclusions against the state at first run: a third-party storage driver (e.g. amdsata) that was Manual then and Boot now is still written. Have `:put` skip a key whose current Start is 0 or 1. (sniffer m1, logic)
- `Control` ignores ERROR_SERVICE_ALREADY_RUNNING, so a TI start racing Windows is claimed as Hadron's; STOP_PENDING TI fails the TI rows instead of waiting. (win32)
- `ProgramData\Hadron` parent ownership isn't checked; a standard user who creates it first could swap or rename `services` (undo logs lost; a doctored restore.cmd could be run from WinRE before Hadron's next start). Require the parent owned by Administrators/SYSTEM too. (win32, sniffer m3)
- DACL re-applied to every file on each load and apply; skip when already correct. (win32)
- CI: the echo-only `:dry` always exits 0, so miss/fail branches never run on real cmd; `restore-online.cmd` isn't run in CI. The live test's data folder may fail the new owner check on the runner: watch the first CI run. (win32)
- Undo/Resume allow duplicate rows for one service; refuse such logs. Names with `( ) ' ! & %` (likely the owner's `Intel(R)` services) now need the manual regedit route in WinRE: safe, but harder. (sniffer m2, m4)
- Still open from round 1: M3 (after Resume, rows the interrupted apply wrote aren't undoable or drift-tracked), M8 (no guard re-check at apply time), M12 (VM hint from SMBIOS only), F3 (load now heavier: ~650 extra service opens for the write check before the list shows), F4 (company read before the list shows), F5 (detail pane rebuilt each paint), `FileCompany`'s unchecked `ExpandEnvironmentStringsW`, canonical en-us KB URLs, UNC data dir.
- Cost of the stabilising loop: 3x BuildPreset 2 -> 39 ms; a toggle 12 ms on a 66-row plan over 755 items (Wine).

### Hand-off
Back to **Build** for A8 and A9 (both small). Round 3 will rerun the probe and the fuzzer.
