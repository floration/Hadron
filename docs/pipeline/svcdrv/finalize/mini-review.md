# svcdrv Finalize: mini review (re-check of 6-review.md)

Reviewer subagent (read-only; saved by Finalize). Branch `module/services-svcdrv` at 94cafef; diff `907d435..HEAD`
over modules/services, core, tools/services, tests, .github, CMakeLists.txt, docs/DECISIONS.md. Verdict formed without
the builder reports. No build or run (the tester builds).

## R1-R25
| # | Status | Evidence |
|---|---|---|
| R1 | closed | store.cpp:418-525: permissions only via CreateDirectoryW(kRootSddl) on a folder Hadron creates; an existing folder is checked by handle (owner, DaclProblem) and refused, never re-secured. Selftest R1 x4 |
| R2 | closed | backend_live.cpp:239-247 `cfgUnknown`; plan.cpp:170 never-touch; skipped by preset :237, Default :345, stock :383, first.txt :1543-1550, FirstRestoreRows :1461; old rows parsed as unknown (store.cpp:83). Selftest R2 x4 |
| R3 | closed, residual N1 | backend_live.cpp:401-424 local-drive only, DRIVE_REMOTE refused, RECALL/OFFLINE refused, return values checked |
| R4 | closed | plan.cpp:961-985 `T starting` / `T started <pid> <ctime>`; claim needs pid+ctime match (:1364-1376), re-checked at run time (:1097-1108); ti-resolved logs; banner from data. Selftest R4 x11 |
| R5 | closed | plan.cpp:497-512; UI module.cpp:2519, 2780, 3274. Selftest R5 x2 |
| R6 | closed | plan.cpp:442-445. Selftest R6 |
| R7 | closed | resume chain (:1205-1214), BuildUndo from the oldest log (:523-549). Selftest R7 |
| R8 | closed | IsUndoLog/UndoBase/FindUndoable (:1166-1272). Selftest R8 |
| R9 | closed | `dismiss:` log under the apply lock (module.cpp:745-751, plan.cpp:1430). Selftest R9 |
| R10 | closed | userTicks_/ApplyUserTicks (module.cpp:1244-1262); TI toggle off during apply (:2809, 3574). Selftest ui R10 x5 |
| R11 | closed | plan.cpp:319-331. Selftest R11 |
| R12 | closed | baseline kind (:1526-1531), recovery\first retried as first-N (:1471-1500), requireFirst (:901). Selftest R12 x4 |
| R13 | closed | RunControl restarts what it stopped (:1123-1131). Selftest R13 |
| R14 | closed | ti.cpp:127-133. Selftest R14 |
| R15 | closed | model.cpp:60-71. Selftest R15 |
| R16 | closed | recovery.cpp A()/NoCtl/RegSafeName, README C. Selftest R16 |
| R17 | closed | ApplyOrder (:806-822), run-time blocker (:1013-1017). Selftest R17 x2 |
| R18 | closed | plan.cpp:268-281; minimum goldens updated |
| R19 | closed | module.cpp:1427-1453, 1568. Selftest ui R19 x3 |
| R20 | closed | plan.cpp:371-405 emptyText; labels module.cpp:2252, 2608, 2811, 3345; stock goldens in CI and services_check.sh |
| R21 | closed | resultFresh_ (module.cpp:776, 1323, 1396, 1700). Selftest ui R21 |
| R22 | closed | ti.cpp waits honour cancel; `T starting` flushed before Acquire (plan.cpp:961). Selftest R22 x2 |
| R23 | partly | logged in DECISIONS (widgets.cpp/--accent; OnFill and min size in 94cafef); Q6 before/after renders not linked (N2) |
| R24 | closed | host.cpp:272-280 WM_GETMINMAXINFO; DPI rescale. Selftest ui R24. Caveat N3 |
| R25 | closed | selftest.cpp:2290-2296 `--live-test` only with HX_SERVICES_TEST_HOOKS |

## Proxy answers
Q1, Q2, Q3, Q7 implemented as written. Q6 kept and logged; renders missing (N2).

## CI after R25
Windows builds `build-svctest` (hooks ON) for the two live-test steps only (:206, :222); everything else uses the
release build. Wine job runs services_check.sh on its own hooks build. Correct; stale comment at ci.yml:98 ("8 golden plans").

## New or still-open findings
- **N1 (cat 1, low):** backend_live.cpp:410-418. GetFileAttributesW reports a final-component symlink, not its target,
  and GetFileVersionInfo*ExW follows it: an image that is a symlink to a OneDrive placeholder or a UNC path still
  recalls or stalls. Fix: refuse FILE_ATTRIBUTE_REPARSE_POINT (rule 1 treats every reparse point as a leaf); selftest.
- **N2 (cat 4, process):** the DECISIONS rows claim before/after renders and pixel identity with no link. Fix: link the
  render set (and the BIOS green before/after) in 7-finalize.md or the rows.
- **N3 (cat 5, low):** host.cpp:272-280 minimum not clamped to the monitor work area (1366x768 at 125% runs off-screen).
  Fix: clamp with MonitorFromWindow/GetMonitorInfo.
- **N4 (cat 6, rule 7):** 1b6e074 and 9817ebb came after kb-audit-2; run kb-auditor on fe827bc..HEAD.
- Category 5 spot-check: Preset header, no "sysok" in UI text, SourceLabel banners, Baseline Basis column, narrow
  caveats, HC system colour pairs, SIDE_LEFT/RIGHT tags, OnFill: done.
- Category 6: kb-audit-2 F1, F8, F9, F12-F15 and README A closed in code.
- Docs (Push): services.md:134 still lets a single change set a System-start driver (R6 reversed it); services.md:245
  describes the old `T started`/`T stopped` marker.

## Verdict
All R1-R25 closed in code except R23's evidence. Open category 1-4: N1 (cat 1) and N2 (cat 4). N4 is required by rule 7 before merge.
