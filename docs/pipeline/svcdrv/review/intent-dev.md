# svcdrv Review: Intention / developer

Branch `module/services-svcdrv` at b7cc215. I only read the code; nothing was built. I checked it against 1-idea.md
(features 1-9 and the proxy answers), 2-research.md (the spec), 3-build.md, 4-analyze.md, 5-design.md, the two design
write-ups and docs/design/services.md.

Overall: the safety core does what the docs say. The backend is armed only from `StartApply` (module.cpp:1144) and
`--live-test`, and is disarmed on every path. Apply runs in this order: lock, snapshot, recovery, log `B`/`P`, then per row
`I`, flush, write, re-read, `D`, flush, and finally `E` (plan.cpp:784-936). The TI toggle is off at launch, is never
written to Prefs, and `--ti on` works only in fixture mode (module.cpp:557). Hadron's own file writes refuse while it is
impersonating (store.cpp:181/191/359/483). No key applies a plan (module.cpp:1298, 1334). I found no write path that skips
plan+confirm, and no live backend function that is a stub. The findings below are the places where behaviour differs
from the docs or quietly misleads.

## Findings, most severe first

### 1. [cat 2] The "Hadron started TrustedInstaller" banner never clears, and later tells the user to stop a TI that Windows started
- plan.cpp:931 writes `T stopped` only on the normal path, and only when `Release` succeeded.
- `TiLeftRunning` (plan.cpp:1048-1050) returns true for **any** log that has `T started` and no `T stopped`.
- The banner's "Stop it" button (module.cpp:1521-1523, `ID_TI_STOP`) runs `RunControl`, which writes no log record. So
  nothing can ever clear the marker. The same holds for the OnExit release path (plan.cpp:851) and for a crash.
- The banner is gated only on "TI is running now" (module.cpp:703-707).

Scenario:
1. An apply that uses TI hits an undo-log flush failure (or a crash, or a failed TI stop).
2. TI is stopped later, either by the banner or by Windows' idle timeout.
3. Weeks later Windows Update starts TrustedInstaller to service the PC.
4. The user opens Hadron. The banner says "Hadron started TrustedInstaller during an apply that did not finish, and it
   is still running … you can also stop it now", with a Stop button.

That claim is false, and pressing Stop interrupts servicing. This repeats every time TI runs, for as long as that log
exists. The text "did not finish" is also wrong when the apply did finish but the stop failed (`E` present, no
`T stopped`).

Fix: have the banner's stop (and a later successful apply) record that the marker is resolved, for example a small
`ti-stopped-<stamp>` file or a `T stopped` record in a new log. Alternatively, consider the marker only when no later log
exists.

### 2. [cat 2] The result banner's "Undo" can open an Undo of the previous apply, not the one just made
- `OnApplied` (module.cpp:1160-1175) sets `haveResult_` and starts a reload.
- The banner adds "Undo" whenever `undoable_ >= 0` (module.cpp:1537). Until the reload finishes, `undoable_` still
  indexes the **old** `logs_`.
- `ID_RESULT_UNDO` (module.cpp:1235-1241) does not check `CanApply` or `loadW_.Running()`.
- `OnLoaded` (module.cpp:688) then reopens `planReq_` with the same stale `logIndex` against the new `logs_`. New logs
  sort last, so the stale index still points at the older log.

Scenario:
1. Yesterday the user applied Pure bloat.
2. Today the user sets X to Disabled and clicks the result banner's Undo during the load, which takes a few seconds on
   about 650 items because of `FileCompany` and `CanWrite`.
3. The plan opened is "Undo the apply of <yesterday's stamp>", which re-enables yesterday's services. Only the stamp in
   the title shows that it is the wrong apply.

Fix: hide or disable the button while `loadW_.Running()`, or carry the result's stamp and resolve the index by stamp.

### 3. [cat 4] Never-touch items can still be stopped and restarted live, while the pane says "Hadron never changes this"
- The spec says never-touch means "no preset, no single-item change" (§6), and §5.2 lists Start/Stop/Restart as
  single-item actions.
- The Stop/Restart enable test is `cen = can && kind != UserTemplate` (module.cpp:2172, Console 2878, keys 2429-2431),
  with no guard check. `BuildControl` (plan.cpp:405-442) and `RunControl` don't check either.

Scenario: select BFE, Dnscache, Dhcp, Audiosrv or mpssvc. The detail pane says "Protected: Hadron never changes this
(networking, WFP and the firewall)" (module.cpp:1731). Stop is enabled anyway. Run stops BFE and its running dependents,
mpssvc and IKEEXT among them, so the firewall is off until a restart.

Either gate Stop/Restart on `!g.neverTouch`, or have the docs say that never-touch covers configuration only and change
the pane text.

### 4. [cat 2] A failed Restart leaves the dependents it stopped first stopped
- `BuildControl` orders a Restart as: stop dependents, stop X, start X, start dependents.
- `RunControl` stops at the first failed stop (plan.cpp:955).

Scenario: Restart X. Its running dependents D1 and D2 stop. X's stop then times out after 30 s, or comes back RUNNING.
The run ends with D1 and D2 stopped and never restarted, and the summary is only "fail stop X". Spec §5.2 lists "the
dependents that will stop", not "that may stay stopped".

Fix: on a failed stop of the main service, start the dependents again (or offer to), and report it.

### 5. [cat 2] Undo after a Resume misses the rows the interrupted apply had already written (open since Analyze, logic M3)
- `FindUndoable` (plan.cpp:1039-1046) skips logs without `E`.
- `BuildUndo` reads only the chosen log (plan.cpp:444-488). After Resume, "Undo last apply" offers the `resume:X` log,
  whose rows are only the rest.

Scenario: Minimum with about 270 rows loses power at row 150. Resume does the other 120. Undo last apply puts back 120;
the first 150 have no Undo route apart from "Default: first snapshot". That doesn't meet idea feature 7 ("Undo restores
the exact prior config"). Analyze left this open for Finalize, and it should not ship unmentioned.

### 6. [cat 2] TI started by Hadron but not RUNNING within the wait is never claimed, so never stopped (open from Analyze round 3)
`TiService::Ensure` returns at ti.cpp:126 when `Control(Start)` fails in `WaitState`, before `started_ = own` at line 129.
The start request did succeed, so TI comes up later, with no `T started` record and no stop. That breaks idea feature 6,
"TI service left in the state it was found". The one-line fix Analyze gave (set `started_` before checking the wait) is
not applied.

### 7. [cat 4 / doc] System-start drivers can't get a single-item change, though the design note says they can
- docs/design/services.md (Guards) says: "System-start drivers are not on that list (spec §5.2): a single change may set
  one where Windows allows."
- `BuildSingle` allows it, but the UI requires `s->cfg.start >= 2` (module.cpp:2153; Console 2412), so every
  start-type button is disabled for a System-start driver.

Either the docs or the code should change. If the code changes, keep the UI gate and correct the note, because it is
the safer behaviour.

### 8. [cat 5] The Baseline plan screen has no TrustedInstaller toggle
- In Baseline the TI chip is drawn only in `PaintListMode` (module.cpp:2075-2079). `PaintPlanMode` has none. Console
  has F6 in plan mode (module.cpp:3155).
- A plan with TI-only rows says "left out while the TrustedInstaller toggle is off". In Baseline the user must Cancel,
  toggle, then reopen the preset, and that loses any row toggles.

5-design.md says "A TI toggle change rebuilds the plan"; in Baseline that is unreachable.

### 9. [cat 6] `--readonly` without `--data-dir` claims there is no first snapshot
- With `--readonly` the store root is never set (module.cpp:572-578), so an existing
  `%ProgramData%\Hadron\services\first.txt` is not read.
- "Default: first snapshot" then says "No first-run snapshot yet: it is written the first time live data loads"
  (plan.cpp:292), the "vs first" column is empty, and an interrupted-apply banner is hidden.

Read-only mode could open the default root for reading (no `EnsureRoot`, no writes), or the note should say why.

## Feature check against 1-idea.md

| # | Feature | Status | Evidence |
|---|---|---|---|
| 1 | Two lists, columns, sort, filter, keyboard | partly | Tabs, sort by key, filter and wheel work: module.cpp:835-910, 1886-1937. Image path is a column only in Console (2588); Baseline shows it in the detail pane. Signer is deferred (proxy). "Preset target for the selected level" became the lowest level, a deviation recorded in services.md. "Pending restart" appears only in the detail pane (1743), not in the Start column as 3-build.md says. |
| 2 | Detail pane | implemented | module.cpp:1720-1842: KB text with basis, presets with pbasis, hardware, first/stock, triggers, deps, "Needed by" (direct deps only, not `+group`), failure actions, protection and whether TI helps, registered description labelled "not Hadron". |
| 3 | Live control: services only, drivers never | implemented | backend_live.cpp:425 and plan.cpp:422 refuse drivers. Never-touch is not gated (finding 3). Restart has the partial-failure gap (finding 4). |
| 4 | Four presets, both tabs | implemented, with gaps | plan.cpp:215-365. "Windows stock" has 0 rows because the 19045 table is empty, a known owner gap. "First snapshot" on an already-tuned PC records the tuned state. So "Default" cannot return to Windows defaults today; Finalize/Ideas must carry this. Minimum never touches Boot-start (CheckGuard plan.cpp:165), and System-start needs `sysok` (246-249). |
| 5 | Custom presets, overrides, export/import | missing (approved deferral) | DECISIONS.md proxy row; per-row toggles cover overrides (module.cpp:1102-1114). |
| 6 | TI toggle | implemented | ti.cpp, plan.cpp:840-933, MarkTi 529-560. Off by default and not persisted. Covers only SCM-DACL refusals, not WinDefend/PPL, and the UI says so (module.cpp:2083). Findings 1 and 6. |
| 7 | Plan, confirm, flushed undo, exact undo, recovery .zip | implemented, with gaps | RunApply plan.cpp:784-936; recovery folder plus zip before any change (812-817); first-run recovery (1082-1135). Finding 5 (Undo after Resume) and finding 2 (wrong Undo target). |
| 8 | Explanations for stock items only, with basis | implemented | ItemDoc 1748-1790. Coverage is 88 services and 11 drivers (Analyze gap). |
| 9 | Measured before/after RAM and process count | missing (approved deferral) | 2-research §2, DECISIONS.md. No predicted numbers anywhere. |

## Acceptance (docs/design/services.md and 2-research §9)
- 1-5 (services_check.sh): wired as described (tests/tools/services_check.sh: genkb plus cmp, fixture cmp, both
  selftests, 8 goldens plus delayedlinger, screenshots including forged and multi). Not re-run here.
- Source guard: ci.yml:25 covers all six APIs.
- `--live-test`: selftest.cpp:1750-1820 refuses non-`HadronTest*` names, drivers and non-elevated runs; it applies,
  verifies, undoes and verifies on a temp data dir; ci.yml:190-227 has the TI variant as continue-on-error. Untested on
  Windows until the first CI run.
- MSVC compile, real cmd.exe and the owner check on the runner run only in CI (as Analyze recorded).
- Test-code nit, still open from Analyze r3: selftest.cpp:1035 indexes `logs[0]` without a size guard. That crashes the
  release-build selftest under Wine; the hooks build is unaffected.

## Hard rules
- Rule 1 (runtime data): no user files are touched. Service config writes go only through plan, confirm, a flushed `I`,
  then the write. Every Hadron file is `CREATE_NEW` and flushed.
- Rule 4: TI is off by default, the toggle is per session, and `ERROR_NOT_ALL_ASSIGNED` is checked (ti.cpp:68-71).
  Non-elevated runs show a banner and are read-only (module.cpp:579, 1478). Recovery `restore-online.cmd` uses `fltmc`
  (recovery.cpp:147). Findings 1 and 6 concern rule 4's "TI left as found".
- Rule 5: fixture mode never applies (module.cpp:981), and `--live-test` refuses any other name.
- Rule 7: no numbers are shown. The TI banner claim (finding 1) and the read-only first-snapshot note (finding 9) are
  claims the code can make falsely.
