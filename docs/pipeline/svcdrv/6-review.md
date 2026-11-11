# svcdrv: Review (stage 6)

Stage 6 (Review), 2026-11-11, on `module/services-svcdrv`, from Design's hand-off at b7cc215. Ten reviewers ran in
parallel: the nine of the brief plus `kb-auditor`, because user-facing text changed. The Logic group got the spec
(docs/design/services.md, 1-idea.md) and the diff only. Every full report is in `review/`:

| Group | Reviewer | Report |
|---|---|---|
| Intention | developer | `review/intent-dev.md` |
| Intention | documentation writer | `review/intent-docs.md` |
| Intention | aligner | `review/intent-aligner.md` |
| Looks | developer (built and rendered) | `review/looks-dev.md` |
| Looks | UI developer | `review/looks-ui.md` |
| Looks | user | `review/looks-user.md` |
| Logic | lead developer | `review/logic-lead.md` |
| Logic | logic machine (ran a harness on the real plan code) | `review/logic-machine.md` |
| Logic | code reviewer (`.claude/agents/reviewer.md`) | `review/logic-reviewer.md` |
| KB | kb-auditor | `review/kb-audit.md` |

## Verdict
**Not ready to merge: 3 category 1 and 22 category 2-4 findings are open.** Finalize must close every one of them
(fix it, or show it is wrong in `7-finalize.md`) before Push can merge. Categories 5-6 and docs are for Finalize too,
but don't block the merge on their own.

What held, confirmed independently by several reviewers:
- Build: MinGW Release with test hooks builds; `services_check.sh` passes (KB byte-identical, selftest 189/189 standalone
  and `--solo`, 8 plan goldens and delayedlinger match, 17 shots render). Warnings only in `core/host/host.cpp`.
- Hard rules 2, 3, 4, 6: firmware-guard grep intact and extended; SDK only; TI toggle off by default, never saved,
  `--ti on` fixture-only, ERROR_NOT_ALL_ASSIGNED checked, no relaunch; recovery ships as folder plus .zip.
- Only `backend_live.cpp` writes to the SCM, only while armed; no fixture or render flag reaches live data; test hooks
  are absent from a default build; fixture mode never applies.
- Snapshot, recovery files and the flushed `I` record are written before each SCM write. Boot-start drivers are never
  touched by any path; presets never touch a System-start driver without `sysok`; dependency checks settle both ways.
- No predicted or unbacked number anywhere; every `published` basis has a URL; genkb selftest 32/32.
- No GDI leaks. At 96 DPI, 1280x800 and 1920x1080, both layouts and all four themes render cleanly. DiskMap, BIOS and
  RunAs Baseline are pixel-identical to before the widgets.cpp change; DiskMap Console gains only the intended focus rect.

Lead spot-checks: R1 (store.cpp:440-466 sets a protected Administrators+SYSTEM DACL on any existing folder whose owner
is Administrators or SYSTEM) and R2 (backend_live.cpp:233-240 pushes a default-config item with only `readError` set;
no plan path reads `readError`) are confirmed in the code.

## Category 1: data safety, or could leave Windows broken (block merge)
| # | Finding | Where | Found by |
|---|---|---|---|
| R1 | `--data-dir` pointed at an existing folder owned by Administrators or SYSTEM (C:\ProgramData, D:\Games, C:\Users) gets a protected Administrators+SYSTEM-only DACL, inherited by everything below, with no record of the old one. Also via `--live-test --data-dir`. Fix: only ever set the DACL on a folder Hadron creates, or require it empty and owned by Hadron's marker. | store.cpp:357-470 (SetSecurityInfo :459) | logic-lead |
| R2 | A service whose config can't be read is recorded in first.txt as Manual (`cfg={3,-1}`); Default (first snapshot) and `recovery\first\restore.cmd` later write that made-up value. Exclude `readError` items from first.txt rows, BuildDefault and FirstRestoreRows, or mark them unknown. | backend_live.cpp:233-240; plan.cpp:299-310, 1068-1077 | logic-reviewer |
| R3 | The Company lookup reads every service image/ServiceDll with GetFileVersionInfo*ExW and no RECALL_ON_DATA_ACCESS / RECALL_ON_OPEN / OFFLINE or UNC check: a OneDrive-hosted service exe is downloaded on every load, a UNC one stalls it. Also ExpandEnvironmentStringsW too-small return ignored (:395) and GetSystemWindowsDirectoryW unchecked (:385). | backend_live.cpp:383-419 | logic-reviewer, logic-lead |

## Category 2: correctness (block merge)
| # | Finding | Where | Found by |
|---|---|---|---|
| R4 | "TrustedInstaller left running" marker never clears: any log with `T started` and no `T stopped` counts forever; the Stop-it plan writes no log; a failed Release writes nothing. Whenever Windows later runs TI, the banner says Hadron started it and offers to stop it mid-servicing. The Why text ("did not finish", "It is a Manual service") is also wrong in cases. Fix: store pid + process start time in `T started`, claim only while they match, record resolution, word the text from data. | plan.cpp:1048-1051, 852-854, 930-931; module.cpp:703-707, 1513-1515 | all three Logic, intent-dev, kb-audit |
| R5 | Start/Stop/Restart skip every guard, including never-touch items labelled "Protected: Hadron never changes this" (BFE, Dhcp, EventLog, Winmgmt, mpssvc, Dnscache, AudioSrv, CryptSvc). Stop BFE takes the firewall down. | plan.cpp:405-442; module.cpp:2171-2175, 2415, 2878-2884 | logic-reviewer, logic-lead, intent-dev, looks-user |
| R6 | A single change can lower a System-start driver with no `sysok` entry (e.g. rdbss), and then nothing in the app can set it back (Undo's TargetGuard, Single, Default-first all refuse; recovery\first omits it). README F and spec :184 say "Hadron never changes these". | plan.cpp:383-391, 117; recovery README F | logic-machine, intent-docs |
| R7 | Undo after a Resume reverts only the resumed rows; rows the interrupted apply already wrote are never offered again (Analyze's M3, still open). | plan.cpp:999-1046 | intent-dev, logic-lead, logic-machine |
| R8 | "Undo the undo" is reachable: a finished `resume:<undo-log>` is offered for Undo and writes the reverted preset back. | plan.cpp:1025-1031, 1039-1046 | logic-reviewer, logic-machine |
| R9 | The "apply did not finish" banner can only be cleared by Resume. Undo is empty (power lost after the `I` flush, log write failed after B/P, machine/build mismatch), empty plans can't be applied, and there is no Dismiss; after a feature upgrade it is permanent. | plan.cpp:789; module.cpp:1124, 1509-1511 | logic-reviewer |
| R10 | Rebuilding a plan (TI toggle, F6, reload) silently discards the user's ticks and unticks, so an unticked Spooler is disabled anyway. Baseline also lets the TI toggle be clicked mid-apply. | module.cpp:1077, 1185-1189, 684, 2401, 3157 | logic-reviewer, logic-lead |
| R11 | Default (first snapshot) never compares builds; after an in-place upgrade it writes the old build's start types. Undo and Resume already refuse. | plan.cpp:283-310 | logic-lead |
| R12 | If first.txt is missing while undo logs exist, the post-change state is captured as "first" with no note, so Default "restores" Hadron's own changes. Applies are allowed when first.txt failed to write; recovery\first is never retried. | plan.cpp:1087-1133 | logic-machine |
| R13 | A failed stop of the main service during Restart leaves the dependents it already stopped down, silently. | plan.cpp:955 | logic-lead, intent-dev |
| R14 | If Hadron's start of TI succeeds but the wait for RUNNING fails, TI is never claimed, so never stopped (Analyze round-3 one-line fix not applied). | ti.cpp:126-129 | intent-dev |
| R15 | `IsDriverType` ignores type 0x4/0x8 (recognizer/adapter drivers such as Fs_Rec), so they show as services and skip fail-closed, `sysok` and the no-unload rule. | model.cpp:66 | logic-machine |
| R16 | `Rem` keeps CR/LF, so a service name containing a newline injects a line into restore.cmd; the .reg has the same hole. | recovery.cpp:62 | logic-machine |
| R17 | At apply time a dependent whose write failed protects services it names directly but not `+group` dependencies. | plan.cpp:878-887 | logic-machine |
| R18 | Minimum leaves rows whose use can't be detected unticked, as Stripped does (36 ticked vs Stripped's 28), which isn't "only what's needed to boot, network, play games". The proxy decision covered Stripped only. Proxy question Q1. | plan.cpp:257-262 | intent-aligner |
| R19 | Baseline plan: a click in the [x] column below the last row toggles the selected row; double-click toggles twice (Console once). | module.cpp:1396-1402 | looks-ui |
| R20 | "Default: Windows stock" (chip, F8) is enabled with 0 stock entries and opens "Nothing to change.", which reads as "already at stock". Disable or relabel with the count until the table has sourced values; add a golden. Proxy question Q2. | module.cpp:2103, 2236-2240 | intent-aligner, intent-dev, kb-audit, looks-user |

## Category 3: resources and threading (block merge)
| # | Finding | Where | Found by |
|---|---|---|---|
| R21 | After an apply, the result banner's Undo shows during the reload with stale `undoable_`/`logs_`; a quick click reverts the previous apply's rows instead of this one's. | module.cpp:1174, 1235-1240, 1537, 684 | logic-reviewer, intent-dev |
| R22 | TI `Ensure`/`Control` ignore cancel, so Cancel and module Shutdown can block for about a minute. The `T started` record is written only after TI has started (crash window). | ti.cpp:159-185; plan.cpp:852-854 | logic-lead |

## Category 4: contract (block merge)
| # | Finding | Where | Found by |
|---|---|---|---|
| R23 | `core/ui/widgets.cpp` (selection text, focus rect, sort-arrow slot for every module) and `--accent` in core/host were changed on a module branch; the spec's out-of-scope list forbids core edits and DECISIONS.md has no entry. Reviewers found no functional bug in them. Finalize: log both in DECISIONS.md (proxy, as core changes kept) with the before/after module renders, or split them out. | core/ui/widgets.cpp, core/host/cmdline.*, host.cpp | logic-lead, logic-reviewer, intent-aligner |
| R24 | No minimum window size (`WM_GETMINMAXINFO`) and no narrow-width fallbacks: below about 1000 px of content (suite at 1024, or 150% DPI on a 1280 window) the Start-type strip runs past the pane, the filter covers the Minimum chip (click rects still live underneath), Console's F10/F11/^Z go under the LIST tag and can't be clicked, and the Hadron column hides. DPI scaling itself is right. Also scroll offsets aren't rescaled on WM_DPICHANGED. | module.cpp:1914, 2096-2169, 2454-2501; core/host | looks-dev, looks-ui |
| R25 | `--live-test` ships in the release exe: a command-line write path with no plan or confirm (limited to services named `HadronTest*`). Gate it behind the test-hooks build or document why it stays. | selftest.cpp, cli.h | logic-lead |

## Category 5: UI consistency (Finalize; doesn't block alone)
- TI toggle reads as an ordinary tab chip; "On" gets the accent fill; `F6 TI off` reads like an action. Give it on/off
  styling and a warning banner while on, and keep the "Not Defender, PPL or Tamper Protection" caveat when narrow
  (it drops at module.cpp:2082 and in the suite's Console hint). (looks-user, intent-aligner)
- "protected" is drawn in error red although it means "left alone"; with a red accent the screen looks like an error page. (looks-user)
- The Hadron column has five ways to say "won't change" (protected, no preset, kept, —, Kept:); F1 help omits "kept"
  and "—"; consider renaming the header to "Preset". "vs 1st", the LIST/PLAN tag and `+t`/`*` need the legend. (looks-user, kb-audit, intent-docs)
- Baseline's plan has no Basis column and no current → target arrow, and Why is cut; Console's does both. The reason
  Apply is off is ellipsised away at suite width; banners lose lines past 4 and notes past 2 with no "…". (looks-user, looks-ui, looks-dev)
- Hover drops white-on-accent text to 3.4:1 (Baseline) and 3.9:1 (Console). The text-on-accent rule exists in three
  copies with different thresholds and BIOS still uses the old one (2.3:1 on green); make it one core helper. (looks-ui)
- Disabled Apply in Baseline still looks like an active blue button. Internal terms leak: "sysok" in plan reasons, the
  raw `preset:bloat:services` id and stamp in the interrupted banner. Third-party items in Console say "the protection
  above is why Hadron leaves it alone" when there is no protection (module.cpp:1785). Console cuts the last list row. (looks-dev, looks-user)
- Console detail split keys off heading text ("Configuration", "From the service"); a KB title starting with either
  word moves blocks; the left column can't be scrolled by keyboard; with no right column the detail is no tab stop.
  Tag paragraphs with their side and add a selftest. (looks-ui)
- High contrast: several colours are mixed from system colours instead of taken from them; Baseline and Console map
  warn/error to different system colours. No focus indicator on a focused list with no selection. (looks-ui)
- Suite: names cut at about 13 characters after the font revert (design merge decision 1) and the "Display name"
  header is cut. Baseline has no image-path column; "pending restart" shows only in the detail pane. (looks-user, intent-dev)
- No TI toggle on Baseline's plan screen (Console has F6); Baseline has to cancel and reopen. (intent-dev)
- A key label breaks mid-word ("remova l"); Account and Type stay truncated at 1920. (looks-dev)

## Category 6: claims (Finalize, through kb-auditor again)
kb-audit counted 34 claims: 11 supported, 11 overstated, 7 unsourced, 5 wrong. Full rewordings are in `review/kb-audit.md`.
- Stripped notes (`P_STRIP_*`) say "only where the hardware or use is absent"; use is never detected (rows are offered
  unticked). They misquote the IoT page ("enterprises", not "devices").
- Contested Minimum items (SysMain, DPS, WdiSystemHost, WerSvc, DiagSvc, DusmSvc) are labelled `published` though
  sources split and inclusion is Hadron's reasoning; SysMain's "can be negative" and memory-compression claims are unsourced.
- intelppm: the whole entry is folklore; only the latency claim is. What the driver does is reasoning.
- The genkb number gate misses counts ("40 processes", "30 services"), "200 megabytes", "half", "two seconds", and
  never reports the bios soft warnings. Counts are what the deferred measurement will show.
- Recovery README F says "Windows sets those per PC" about all 55 items, including TrustedInstaller, VSS, Defender.
  Per-apply README says "What this restores: Stripped (Services)" but it restores the values from before that plan
  (recovery.cpp:197). restore.cmd says "No Windows installation" on a BitLocker-locked drive with no hint.
  "WinRE can't open .zip files" has no source.
- Hadron column legend (module.cpp:2959) "the lowest preset that changes it" is wrong for rows already set or offered unticked.
- Drivers note says Pure bloat is "near-empty": it is always empty. Company shows the file's own claim with no label.
  Console help's "every key changes the view or opens a plan" isn't true of F6 or Esc during an apply. "The TI toggle
  does not help" for protected services was never tested.
- Two soft claims need a `reasoning` label: "Windows stops it on its own when idle", "a desktop rarely needs it". Raw
  URLs and "gives No guidance" / "rates it Do not disable" need quoting. XboxGipSvc's class hint is unverified;
  CaptureService, SSDPSRV, RasAuto say "test it" without how; RetailDemo, MessagingService "nothing uses it" is unsourced.
- `--readonly` without `--data-dir` says "No first-run snapshot yet" even when one exists and hides the interrupted banner. (intent-dev, intent-docs)

## Docs (Finalize; Push owns the final docs)
From `review/intent-docs.md` (R-D1 to R-D13) and `review/intent-aligner.md`:
- Recovery README: say how to choose among `recovery\<stamp>` folders (UTC stamps, newest first, or `first`), give a
  fallback when WinRE doesn't open (install media > Repair), fix "beside" (the zip is `<root>\recovery-<stamp>.zip`),
  and fix paths for `--data-dir` on another drive or UNC. The result banner shows only the .zip, which WinRE can't open.
- Two plan notes name a "Restore first snapshot" control that doesn't exist (it's "Default: first snapshot" / F7).
  Console help says Tab only cycles list, filter and detail; Ctrl+F is undocumented.
- DECISIONS row 2 says Stripped never removes hot-plug classes; the code offers them unticked (spec matches code).
- Spec/store.h format errors: deps joined with `;`, missing or extra meta keys, `created` local in first.txt but UTC in
  snapshots, P/I/D/E fields described wrongly, ControlService fallback unmentioned. Spec :232 describes the Hadron column pre-Design.
- Undocumented flags: `--update-golden`, `--golden`, `--plan undo|undo-last|resume|ti-stop`, `--focus`, `--sort`,
  `--keys`, `--tab`; how to regenerate goldens. ARCHITECTURE.md lacks `%ProgramData%\Hadron\services\`. UTC stamps unlabelled.
- Design note says a single change may set a System-start driver; the UI disables those buttons (consistent with fixing R6).
- Log in DECISIONS.md: Design's ten unasked decisions (incl. redefining "preset target" as a lowest-level verdict) and
  the OEM narrowing (third-party items never in a preset; the idea put OEM extras in Pure bloat).

## Questions for Finalize to put to the proxy
- Q1: Should Minimum tick by default the rows whose use can't be detected? (R18)
- Q2: Fill the stock table from a published source, or disable/relabel "Windows stock" with its count? A capture from a
  pipeline VM has no basis rule 7 allows. (R20)
- Q3: Should the first-snapshot option say it is "as Hadron first saw this PC (date)", not Windows' default?
- Q4: Log the OEM narrowing, or add sourced OEM entries?
- Q5: KB expansion target for this run (88 services, 11 drivers now; most drivers have no text), or defer it as a follow-up idea?
- Q6: Keep the core widgets.cpp and `--accent` changes in this PR (logged), or split them out? (R23)
- Q7: Re-add the 12px first-column font as its own core change for all modules, given the suite truncation?

## Still open from earlier stages
- Analyze's leftover minors (M8, M12, F3-F5, KB URLs, UNC data dir) and the Needs-Windows list: the first windows-latest
  CI run is the first MSVC compile, the first `--live-test` and the first restore-script run on real cmd.exe. The
  read-only check on James's PC is still owed. Never apply a preset on his PC.
- Deferred features for 7-finalize.md so Push files them as follow-up ideas: before/after RAM and process count,
  custom presets with export/import, the signer column, the event 7040 account.
- The branch is behind main (#5, 122d2ad, BIOS guarded writes); CMakeLists.txt, core/host/registry.* and ci.yml change
  on both sides. Rebase before the final pass.
- Review renders above 96 DPI used a scratch-only size/DPI env patch (`pipeline/svcdrv/review/looks-dev/review-harness-screenshot-size.patch`
  in shared files, 94 shots there). Finalize may want `--size`/`--dpi` for fixture renders as a real core flag (lead/core change).

## Hand-off
To **Finalize** at the commit that adds this file.
