# svcdrv Finalize: Mini Build (tester, 2026-11-11)

Branch `module/services-svcdrv` @ e3cfd92. Toolchain: MinGW-w64 GCC 13-posix, CMake 3.28.3, Ninja. Wine 9.0 (Ubuntu 9.0~repack-4build3),
Xvfb 1600x1000x24 at **96 DPI**. Dedicated `WINEPREFIX=/tmp/claude-0/mini-build/wine` with `dosdevices/z:` removed and `d:` -> `/tmp/claude-0/mini-build`
(builds, a `git archive HEAD` copy of the repo in `src/`, fixtures, shots). Nothing ran against any real folder or service.

| # | Step | Result |
|---|------|--------|
| 1 | Clean configure+build, `-DHADRON_SERVICES_TEST_HOOKS=ON` (`/tmp/claude-0/mini-build/hooks`) and plain Release (`.../release`) | PASS, both exit 0 |
| 2 | CI linux-mingw selftests on both builds (hello, uikit, diskmap x3, runas x2, services x2) | PASS on hooks (all); release: all PASS except services (expected under Wine, see P1) |
| 2b | `HadronServices.exe --live-test HadronTestSvc` on plain build | PASS: exit 2, "--live-test is not in this build"; no data dir created |
| 2c | BIOS KB: `genkb.py --selftest` (182 checks) + regenerate + `cmp` settings.db | PASS |
| 2d | 10 golden plans + delayedlinger via `--dump-plan` on the **plain** build (what the windows job uses) | PASS 11/11 byte-identical |
| 3 | `services_check.sh hooks shots`: genkb selftest + services.db cmp, fixture cmp, selftest 256/256 standalone and suite, 11 golden plans, 17 shots | PASS |
| 4 | `bios_check.sh release shots` (as CI) + bios selftests on hooks build | PASS (159/159 each), 11 shots |
| 5 | windows-msvc job: every exe/flag/file it names exists (`--dump-plan --tab --out`, `--live-test --ti --data-dir` in hooks build, `--launch-test`, `--write-demo`, `--snapshot`, `--keys`, `make_bios_snapshot.py`, `restore-dryrun.cmd`, delayedlinger fixture) | PASS (read only) |
| 6 | MSVC pitfall grep (modules/services + core/host/host.cpp, core/ui/theme.{h,cpp}, core/ui/widgets.cpp) | 1 latent hit (P2) |
| - | CI firmware-guard and services banned-API greps | PASS (no hits) |

Commands (from `/tmp/claude-0/mini-build`, `WINEDEBUG=-all`):
`cmake -S /home/user/Hadron -B {hooks,release} -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release [-DHADRON_SERVICES_TEST_HOOKS=ON] && cmake --build ...`;
`wine <b>/HadronServices.exe --selftest --fixture D:\src\tests\fixtures\services` (and the CI list);
`bash src/tests/tools/services_check.sh hooks shots`; `bash src/tests/tools/bios_check.sh release shots`.

## Warnings (identical in both builds; none in core/ui)
- `modules/services/module.cpp:232` `-Wunused-function`: `SourceText()` is dead since 4b9f68a moved the banner to
  `plan.cpp InterruptedText` (which uses `SourceLabel`). **New on this branch.** Delete it. (cat 5, hygiene)
- `core/host/host.cpp:44,430` `-Wcast-function-type` (GetProcAddress casts) and `:439,491` `-Wmissing-field-initializers`
  (`WNDCLASSEXW wc = {sizeof(wc)}`): on lines this branch did not touch; pre-existing.

## Problems
- **P1 (cat 2, selftest only)** `modules/services/selftest.cpp:1148` indexes `logs[0]` (also `:1163` `logs[FindUndoable(logs)]`, `:1266` `logs[0]`)
  right after a `check()` that can fail, so the plain-Release selftest under Wine (no data folder: A5 owner check) logs
  14 FAILs and then **crashes** (page fault in `BuildUndo`, exit 5) instead of reporting. Guard with `if (logs.size()==1)`.
  On real Windows the release selftest has a data folder, so CI is not affected; it only hides the failure count.
- **P2 (MSVC latent, not cat 1-4)** `modules/services/module.cpp:3996` `const RECT small{...}`: `rpcndr.h` has
  `#define small char`. Today the TU does not pull `rpcndr.h` (WIN32_LEAN_AND_MEAN via win_compat.h; only `winsvc.h`
  in its deps), so it compiles; any later `<shlobj.h>`/`<objbase.h>` include breaks MSVC. Rename (e.g. `minRc`).
- Other MSVC greps clean: no `interface` identifiers; Co* users (`store.cpp`, `host.cpp`) include `<shlobj.h>`/objbase;
  every SDK name in `svc_compat.h` is a `#define` in the SDK and is `#ifndef`-guarded; own names for structs.
- **P3 (cat 5, UI text)** raw stamps in banners/titles: "An undo did not finish (20261110-220000 UTC, the undo of
  20261110-210000 UTC)", "Plan: Undo the apply of 20261110-210000", TI banner "20261110-213012 UTC", while the header uses
  "2026-11-01 18:30". Interrupted banner also nests parens: "(..., Pure bloat removal (Services)):".
- Env note: Wine 9.0 `wineserver` aborts (`free(): invalid pointer`) when `TMPDIR` is set; used a `mktemp -p` shim instead.

## Screenshots (`/tmp/claude-0/mini-build/shots/`, 1280x800; no committed services goldens exist, judged on layout)
All 17 services shots render with no empty or overlapping areas; lists sorted by Name asc, selection scrolled into view;
detail panes clip at their scrollbar as designed. Suite shots drop the Company column and shorten the legend to fit.
`services-console-f9.png` (Drivers + F9): "Nothing to change" with an empty detail box (correct; could show a hint, cat 5).
BIOS: 11 shots written by bios_check.sh, not reviewed here (no BIOS change on this branch).

**Verdict: PASS** (build, selftests, goldens, shots). Fix before merge if cheap: P1, unused `SourceText`; P2/P3 optional.
