# svcdrv Finalize round 2: what each finding got

Branch `finalize/svcdrv-round2` from 95519bd. Checks: `tests/tools/services_check.sh` (hooks build) passes: genkb
selftest, `services.db` cmp, fixture cmp, both selftests (282 checks, 0 failed), 10 plans + delayedlinger match the
goldens, all screenshots. The plain Release build's `--selftest --fixture tests/fixtures/services` runs to the end
(282 checks, 57 failed: Wine can't give a folder an admin owner, so every data-folder check fails, as the selftest's
`[info]` line says), with no crash. DiskMap, BIOS and RunAs renders (both layouts, dark and light, suite) are
pixel-identical to 95519bd (`compare -metric AE` = 0 on all 18). Renders: `/mnt/project-files/pipeline/svcdrv/finalize/round2/`.

| Item | Result | Where | Test or render |
|---|---|---|---|
| mini-analyze A1 (ancestor can be renamed) | Fixed: every folder above the data folder, from the volume root, must be owned by Administrators/SYSTEM/TI and its DACL must give no one else DELETE, FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER (inherit-only ACEs skipped); missing parents created `O:BA` | store.cpp:376 (AncestorProblem), :454 (RenameAccessProblem), :509 | selftest "A1" SDDL cases + EnsureRoot under a `renamable` folder (Windows only; Wine keeps no DACL) |
| mini-analyze A2 (Dismiss loses Undo; dismissed resume) | Fixed: a dismissed log stays undoable; a `dismiss:` of a resume settles the apply it resumed; a Dismiss pressed during a load is kept and recorded by the next load | plan.cpp:1205, 1265, 1310; module.cpp:800 | selftest "mini-analyze A2 (run 1)", "(run 2)"; "ui mini-analyze A2"; renders after-owed1-*, after-owed2-list |
| mini-analyze A3 (first-N rewritten after upgrade; truncated zip) | Fixed: recovery\first-N written only for a first.txt of this PC and build (note otherwise); completeness parses the zip and compares CRC and bytes | plan.cpp:1547, 1566 | selftest "mini-analyze A3 (run 3)", "A3: a truncated zip" |
| mini-analyze A4 (stale TI state resolves a live claim) | Fixed: under the apply lock TI is queried again and logs re-read before a claim is closed | plan.cpp:1469-1505 | selftest "mini-analyze A4" |
| mini-analyze A5 (Cancel turns cleanup into failures) | Fixed: TI stop and R13 restarts are sent without the cancel flag and waited for at most 10 s; "requested, not confirmed" when sent but unconfirmed | backend.h:30; plan.cpp:1149; ti.cpp:154 | selftest "mini-analyze A5" (x2), R13 restarts with cancelOnStop |
| mini-analyze A6 (first.txt from another PC passes R12) | Fixed: RunApply reads first.txt under the lock and refuses another machine GUID before any write | plan.cpp:910 | selftest "mini-analyze A6" |
| mini-analyze A7 (result Undo opens previous apply) | Fixed: Undo in the result banner only when the undoable log is this result's | module.cpp:303 | "ui mini-analyze A7" |
| mini-analyze A8 (unreadable start type shown as fact) | Fixed: "unknown" in cells, "unknown (could not be read)" in the detail, no start buttons | module.cpp:2800 | "ui mini-analyze A8" |
| Fixture run 1 (dismissed interrupted + undo-last) | Ran on a temp copy: before, undo-last opened nothing; now it opens that apply | selftest.cpp:2040- | "mini-analyze A2 (run 1)"; before/after-owed1-*.png |
| Fixture run 2 (dismiss of a resume) | Ran: before, A's banner came back; now it is settled | | "mini-analyze A2 (run 2)"; before/after-owed2-list.png |
| Fixture run 3 (first.txt of another build, zip removed) | Ran in the selftest: first-2 is not written; a note says why | | "mini-analyze A3 (run 3)" |
| Fixture run 4 (order > 1000000) | Ran: before, the forged log stayed newest; now orders clamp to 1000001 and the stamp decides | plan.cpp:1212-1218, 1242 | "mini-analyze run 4"; before/after-owed4-*.png |
| mini-review N3 (minimum size off-screen) | Fixed generically: WM_GETMINMAXINFO clamps the minimum to the monitor's work area | core/host/host.cpp:273 | No core UI harness; Wine without a window manager can't show it. Needs a 1366x768 @125% check on Windows |
| mini-build P1 (selftest crash without a data folder) | Fixed: every log index goes through At() | selftest.cpp:101 | plain Release selftest: runs to the end, no crash |
| mini-build P2 (`small` macro) | Fixed: renamed to minRc | module.cpp (R24 test) | builds |
| mini-build unused SourceText() | Removed | module.cpp | no -Wunused-function |
| mini-build P3 (raw stamps) | Fixed: "YYYY-MM-DD HH:MM UTC" (with -N) in banners, undo/resume titles and source labels; no nested parentheses | plan.cpp:1373, 1405 | "ui R21 stamp" and banner selftests; r2-*-interrupted-boring.png, r2-*-multi-dark.png |
| mini-design 1 (stamp + doubled parenthesis) | Fixed with P3 | plan.cpp:1405 | as P3 |
| mini-design 2 ("Apply is off" three clauses) | Fixed: "Apply is off. This is fixture data." | module.cpp:1158 | "ui mini-design 2"; r2-baseline-plan-*-*.png |
| mini-design 3 (Name 15px vs 12px) | Rebutted in code, fixed in docs: core revert e5ee164 stands (proxy Q7 "no core font change"); baseline.md §2 amended | baseline.md §2 | n/a |
| mini-design 4 (Company gone in the suite) | Fixed: Display name drops before Company; spare width goes to Company; baseline.md §3 amended (at 1024x768 only Name/State/Start/Preset fit) | module.cpp:2132-2225 | "ui mini-design 4"; r2-baseline-suite-*.png, r2-baseline-suite-1024x768-dark.png |
| mini-design 5 ("Display na…", filter placeholder) | Fixed: "Display" when the title doesn't fit; placeholder "Filter" when narrow (both layouts) | module.cpp:2216, 1152 | "ui mini-design 5"; r2-baseline-suite-dark.png |
| mini-design 6 (plan selection on the last line) | Fixed: the selected plan row is centred | module.cpp:1101, 2694 | r2-baseline-plan-ti-light.png |
| mini-design 7 (Console Boot/System not on the strip) | Fixed: current Boot/System drawn as a disabled selected cell | module.cpp:3386 | r2-console-boot-dark.png |
| mini-design 8 (F-bar short tier) | Fixed: "Drivers"/"Services", "TI ON"/"TI off", "Stock: none" | module.cpp:2884-2930 | "ui mini-design 8"; r2-console-suite-keys-dark.png |
| mini-design 9 (empty stock plan) | Fixed: no Apply, no tick sentence, one reason; detail "Nothing to show: this plan has no rows."; F-bar "Nothing to apply" | module.cpp:2946 | "ui mini-design 9" (both layouts); r2-*-plan-stock-dark.png |
| mini-design 10 (nvlddmkm twice) | Fixed: warn line only "A single change is still possible where Windows allows it." when there is no KB entry | module.cpp:1979 | "ui mini-design 10"; r2-*-drivers-light.png |
| mini-design 11 (`first.txt` in plan basis; F1 legend) | Fixed: "Hadron's first-run snapshot of this PC (<date>), read from its service manager"; Console legend has "recorded" | plan.cpp:356 | goldens plan-services-default(.delayedlinger).txt; r2-*-plan-default-dark.png |
| mini-design 12 (pending marker; stock wording) | Marker: kept per layout on purpose ("(pending)" Baseline, "!" Console, each in its legend), noted in docs/design/services.md (console.md is outside this scope); stock line now uses the chip's words | module.cpp:2052; services.md "Pending marker" | "ui mini-design 12" |
| mini-design 13 ("impersonating") | Fixed: "needs TrustedInstaller: Hadron writes it as TrustedInstaller (toggle on)" | plan.cpp:643 | r2-*-plan-ti-light.png |
| mini-design 14 (cut Why and Start cells) | Fixed: Basis drops before Why is under 280 px; Start cells that don't fit say "+t" (measured per cell) | module.cpp:2239, 2800 | "ui mini-design 14"; r2-baseline-list-dark.png |
| mini-finalize 1-14 (design note vs code) | Fixed: docs/design/services.md rewritten for R1/A1 data dir, all undo-log records and resolution logs, Dismiss, control plans (R13, ti-resolved), TI claim (R4), R6, R2, Minimum rule, stock text, "Default: as first seen" and R11/R12, apply-order checks, recovery\first, Preset labels, R25, 10 goldens, order cap, stamps | docs/design/services.md | text |
| mini-finalize 15 (README F overclaim) | Fixed: "Presets and Default never change these…"; a single change is put back by Undo or its own recovery folder | recovery.cpp:301 | selftest "README F says…"; golden first-README.txt |
| mini-finalize 16 (first-N) | Fixed: "Which folder to use" names first-2..first-9 | recovery.cpp:233 | same selftest; golden first-README.txt |
| mini-finalize 17 (Company in the load pass) | Fixed: second Worker pass (companyW_) after the list shows; detail says "still being read"; a replaced pass is ignored | module.cpp:805 | "ui mini-finalize 17" (x2) |
| mini-finalize 19 (TI started before token work) | Logged as a deviation in docs/design/services.md | services.md "Deviations" | text |
| mini-finalize 22 (genkb count) | Fixed per the coordinator: 62 must-fail, 17 ok texts (true once 0a79277 from module/services-svcdrv is merged; this branch alone prints 50 and 15) | tools/services/README.md:10 | text |
| mini-finalize 23 (Confirm(), zip writer) | Fixed: tree comment says neither exists; also no relaunch, PNG writer, firmware.h in modules/bios; data-dir DACL wording (row 1) | ARCHITECTURE.md:10-14, :138 | text |
| store.h:37,40 | Fixed: I's identity is the planned one; E has done\|cancelled | store.h:37, 40 | text |
| ci.yml:98 | Fixed: "10 golden plans plus the delayedlinger plan" | ci.yml:99 | text |
| backend_live.cpp:356 | Fixed: MINOR_NONE is documented (Learn's value is a typo); fallback kept because no CI step runs the stop | backend_live.cpp:356 | text |
| mini-finalize 18, 20, 24; DECISIONS rows | Not done: DECISIONS.md and kb.py are outside this scope (KB is with kb-auditor) | | |

Golden diffs: `plan-services-default.txt`, `plan-services-default-delayedlinger.txt` (basis text, mini-design 11) and
`first-README.txt` (README F and "Which folder to use", mini-finalize 15/16). All intended; no other golden changed.

For the lead: A1 refuses a data folder under a user profile. The selftest now uses `%SystemRoot%\Temp`; the
Windows CI live-test steps (ci.yml:207, :223) that pass `$env:RUNNER_TEMP\hadron-live\services` as `--data-dir` will likely be refused
(RUNNER_TEMP is under the runner's profile or D:\a, whose DACL may let the runner account rename it). They need a
data dir under `$env:SystemRoot\Temp` or `$env:ProgramData`; ci.yml was comment-only in this scope.
ARCHITECTURE.md:80 ("Use `hx::ui::Confirm()`") still names the Confirm() that doesn't exist; that line states a
contract rule, so it is left to the lead.
