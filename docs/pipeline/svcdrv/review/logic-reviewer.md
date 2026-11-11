# svcdrv Review: Logic group, code reviewer (`.claude/agents/reviewer.md`)

Branch `module/services-svcdrv` at b7cc215 against origin/main. Read: docs/design/services.md, 1-idea.md, 2-research.md
§5-§6. Not read: 3-build, 4-analyze, 5-design, analyze/, design/. No legacy tool, so no legacy notes. Saved by the
Review lead from the reviewer's returned report (the reviewer is read-only).

## Hard rules: no findings
- Rule 2: the firmware-guard grep is still in ci.yml and still scans core, modules and app. The new modules/services
  grep adds DeleteService, CreateService, RegDeleteKey, RegDeleteTree. All write and control APIs appear only in backend_live.cpp.
- Rule 3: SDK libraries only.
- Rule 4: TI toggle off by default, never persisted; `--ti on` only in fixture mode. AdjustTokenPrivileges checks
  ERROR_NOT_ALL_ASSIGNED. No relaunch.
- Rule 6: folder plus .zip, per the logged proxy decision.
- Rule 7: the UI shows the basis everywhere checked; KB content is kb-auditor's.
- Contract: WM_MODULE_BASE+n messages, class "Hadron.services.View", resource SERVICES_KB, WS_CHILD, no COM init, no PostQuitMessage.

## Findings, most severe first

**1. [Cat 1] An unreadable service is recorded as "Manual" in first.txt, and Default and recovery\first later restore that made-up value.**
backend_live.cpp:233-240; consumed at plan.cpp:299-310 (BuildDefault) and plan.cpp:1068-1077 (FirstRestoreRows).
When ReadOne fails, Enumerate pushes `SvcItem{}` with default `cfg = {3,-1}` and only sets `readError`.
EnsureFirstSnapshot writes it into first.txt; no plan or recovery path checks `readError`. Scenario: on first run
service X (really Auto) can't be read; first.txt records `3/-`; "Default: restore first snapshot" plans
"X: Auto -> Manual, as recorded"; the apply re-read matches live Auto, so Hadron writes Manual; recovery\first\restore.cmd
also writes Start=3. Items with `readError` must be left out of first.txt rows, BuildDefault and FirstRestoreRows, or marked unknown.

**2. [Cat 2] The "apply did not finish" banner can only be cleared by Resume, which re-applies the interrupted changes; sometimes nothing clears it.**
plan.cpp:789, module.cpp:1124, 1509-1511. An Undo plan with no rows can't be applied (`Included()==0` returns early;
RunApply writes no log for an empty order), so it never resolves the log, and the banner has no Dismiss. Undo is empty
when power is lost after the `I` flush and before the SCM write; when a log write or flush fails after B/P with no I
rows; when the log is refused for a machine or build mismatch. After a feature upgrade nothing resolves it.

**3. [Cat 6, with cat 2] The "TrustedInstaller left running" marker is never cleared.**
plan.cpp:1048-1051, module.cpp:703-707, 1513-1515, 1028-1032. `TiLeftRunning` is true for any log with `T started`
and no `T stopped`; logs are CREATE_NEW and never appended; the "Stop it" control plan writes no log; a failed Release
(plan.cpp:930-931) writes no `T stopped`. Later, when Windows Update starts TI, the banner falsely says Hadron started
it and "Stop it" stops TI mid-servicing.

**4. [Cat 2] Start, Stop and Restart skip every guard, including never-touch items.**
plan.cpp:405-442 (BuildControl never calls CheckGuard), module.cpp:2172-2175, 2415. Spec §6: never-touch means no
preset and no single-item change; §5.2 lists Start/Stop/Restart as single-item actions. Stop on BFE stops mpssvc,
IKEEXT, PolicyAgent: firewall off until restart. Same for EventLog, Winmgmt, Dhcp, AudioSrv, CryptSvc.

**5. [Cat 2] Rebuilding a plan throws away the user's ticks and unticks.**
module.cpp:1189 (ID_TI -> OpenPlan), 2401/3157 (F6 in Console plan mode), 1077 (no merge), 684 (OnLoaded re-opens).
Console: open Minimum, untick Spooler, press F6; the rebuilt plan ticks Spooler again silently; Apply disables it.
Carry overrides across a rebuild, keyed by name, or warn.

**6. [Cat 3] After an apply, the result banner's Undo opens the previous apply's log.**
module.cpp:1537, 1235-1240; OnApplied -> StartLoad at 1174, rebuild at 684. Until the reload finishes, `undoable_` and
`logs_` are stale; a click during the load builds "Undo the apply of A", and OnLoaded rebuilds it with the same stale
`logIndex`. Apply reverts A's rows instead of B's.

**7. [Cat 2] "Undo" can re-apply an apply that was already undone.**
plan.cpp:1039-1046 (FindUndoable), 1025-1031 (UndoBase). FindUndoable skips only `undo:` sources; a finished
`resume:U` where U is `undo:X` is offered for undo. Apply X, Undo (U) interrupted, Resume (R) finishes; "Undo last"
offers R and writes X's values back.

**8. [Cat 1 low likelihood, plus cat 2] Company lookup reads service image files with no cloud-placeholder or network check; two unchecked results.**
backend_live.cpp:383-419, called for every item on every load (module.cpp:605). GetFileVersionInfo*ExW read the image or
ServiceDll with no RECALL_ON_DATA_ACCESS / RECALL_ON_OPEN / OFFLINE check and no UNC check: a OneDrive-hosted service exe
is downloaded on each load; a UNC image stalls the load. Line 395 ignores ExpandEnvironmentStringsW's too-small return;
GetSystemWindowsDirectoryW at 385 is unchecked.

## Note, not a finding
core/ui/widgets.cpp changes selection text colour and focus-rect colour/inset for every module's ListView;
core/host/cmdline.h and host.cpp add `--accent`. No functional bug, but both are core/ edits on a module branch and
need the lead's sign-off; other modules' screenshots change.
