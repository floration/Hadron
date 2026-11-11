# svcdrv Review: Intention / aligner

Branch `module/services-svcdrv` at b7cc215, read-only. Chain traced: 1-idea.md -> 2-research.md -> 3-build.md ->
4-analyze.md (+ analyze/a-champion.md) -> 5-design.md (+ design/baseline.md, design/console.md) -> DECISIONS.md rows 20-24
(proxy) -> docs/design/services.md -> modules/services/, tools/services/kb.py, tests/golden/services/.

Owner's verbatim ask, in four parts:
(a) services/drivers page with a run-as-TrustedInstaller toggle;
(b) shows all services and drivers, their state, explains the default Windows ones, start settings;
(c) presets Default / Pure bloat removal / Stripped / Minimum;
(d) separate for services and drivers, with the same preset levels.

## Verdict
The skeleton of the idea holds through every stage: two tabs, live state and start type, four levels on each tab, Default
with two options, a TI toggle that is off by default, plan + confirm + undo, and no predicted numbers anywhere. No stage
renamed or dropped a level or a tab. Four things drift from what the owner will expect. Three are content gaps and one
is a behaviour change nobody logged:
1. Minimum ticks barely more than Stripped (F1, behaviour, not logged);
2. "Windows stock" is empty and looks like "already at stock" (F2);
3. the KB explains only 99 of about 650 items (F4);
4. OEM extras never reach Pure bloat (F5).

## Findings

### F1. Minimum offers its "use cannot be detected" rows unticked, which changes what Minimum means. Suggested category 2 (or scope)
- **Evidence:**
  - 1-idea.md:31-32: Minimum is "only what is needed to boot, network, play games and run Hadron".
  - 2-research.md §5.3 table: Minimum = "Stripped plus `minimum` ... only what's needed ... stays on. Includes contested services".
    The "only where the hardware or use is absent" condition is written on the Stripped row only.
  - DECISIONS.md:24 (proxy): "Stripped rows whose use cannot be detected are offered unticked".
  - The code applies that rule at every level: plan.cpp:257-262 tests `e->JoinLevel() == LV_STRIPPED`, not the level
    of the plan being built.
  - Result: tests/golden/services/plan-services-minimum.txt has 36 ticked and 23 unticked, against Stripped's 28 ticked.
    The Minimum plan leaves PcaSvc, CDPSvc/CDPUserSvc, OneSyncSvc, RmSvc, RasAuto, QWAVE, lltdsvc, SharedAccess and
    dmwappushservice unticked, all with "use cannot be detected: tick if you don't use it".
- **What diverged:** Minimum on the Services tab is now "Stripped's defaults plus 8 rows". That is not "only the
  essentials stay on". The proxy decision covered Stripped; nobody logged extending it to Minimum.
- **Finalize:**
  - Proxy question Q1 (below). The likely answer, which keeps the safety of decision Q9: at Minimum, tick the
    use-undetectable rows by default and keep the evidence text; hot-plug classes stay unticked (Q9).
  - Code: base the unticked rule on `level == LV_STRIPPED` (the plan's level), or keep the current rule and log it in
    DECISIONS.md with a plan note.
  - Regenerate plan-services-minimum.txt.
  - Drivers are not affected, because every driver in a level has an `hw` hint.

### F2. "Default: Windows stock" is an enabled chip over an empty table, and the empty plan says "Nothing to change." Suggested category 6 (claims) / 5
- **Evidence:**
  - kb.py has 0 entries with `stock`. design/services.md:195 and 3-build.md:83 both say "empty".
  - The chip is enabled whenever data has loaded (module.cpp:2103; Console F8 at module.cpp:2446).
  - On 19045, which is the owner's build, BuildDefault(stock) returns no rows and one note: "0 of N items have a Windows
    stock value..." (plan.cpp:356-358).
  - The plan screen then draws "Nothing to change." centred in the list (module.cpp:2236-2240). The summary line reads
    "0 ticked ... 0 already set".
  - The detail pane says "Windows stock: no value with a measured or published basis in Hadron's KB yet"
    (module.cpp:1798).
  - No golden covers the stock plan. The 8 goldens cover Default as first-snapshot only, while 2-research.md §9.4 asks
    for "all 8 combinations".
- **What diverged:** the idea promised two Default options (proxy answer 3). One of them exists in name only. The big
  "Nothing to change." can be read as "this PC already matches Windows stock". That is an unbacked claim (rule 7 spirit).
- **Finalize:**
  - Proxy question Q2. Either fill the table with a sourced capture, or say plainly that Hadron has no stock data. For
    example: relabel the empty state "Hadron has no Windows-stock data for this build yet: nothing will be changed"
    and/or disable the chip with that reason.
  - Add a `plan-<tab>-stock.txt` golden either way.

### F3. Default can't take a tuned PC back to Windows defaults. Scope (owner-gap, open since Analyze E1)
- **Evidence:**
  - a-champion.md E1 and 4-analyze.md:71.
  - first.txt is written the first time live data loads (2-research.md §5.4). On the owner's already-tuned PC it records
    his tuned state.
  - 5-design.md "Not done" passes this to Finalize as content and scope.
- **What diverged:** nothing in the code. The owner's word "default" most likely means Windows' defaults. Together with
  F2, neither Default option gives him that today.
- **Finalize:**
  - Q2/Q3 below.
  - At minimum, the first-snapshot plan and chip should say it is "as Hadron first saw this PC (date)", not Windows'
    default. The row reason already gives the date; the chip label "Default: first snapshot" does not say it.

### F4. "Explains what they do (default windows ones)": KB coverage is thin. Scope (owner's verbatim words)
- **Evidence:**
  - kb.py has 99 entries: 88 services and 11 drivers. Breakdown: 17 bloat, 42 stripped, 10 minimum and 19 never among
    the services; 6 stripped, 1 minimum and 4 never among the drivers.
  - a-champion.md E2: RpcSs, DcomLaunch, Audiosrv and nearly every driver show "Not in Hadron's KB". Drivers get no
    explanation at all.
  - 2-research.md §7: "Seed size for Build ... 60–100 entries. More is Finalize's job."
  - 5-design.md leaves it open for Finalize.
- **What diverged:** this is one of the owner's four explicit asks. Today most stock items fall back to Windows' own
  registered description, and drivers get nothing. The spec gave this work to Finalize; no stage has started it, and
  nothing records it as deferred.
- **Finalize:**
  - Expand the KB, at least with `never`/explanation-only entries for the core stock services and the common inbox
    drivers. Every entry needs a basis, and the diff goes through kb-auditor (rule 7).
  - Or, through the proxy, record a deferral with a target, file it as a follow-up idea, and say so in 7-finalize.md.
  - Note: more KB entries also widen Minimum. Presets only touch KB items, and a non-KB stock service is never in a preset.

### F5. Pure bloat never includes "consumer and OEM extras" from third parties. Scope (unlogged narrowing)
- **Evidence:**
  - 1-idea.md:29 says Pure bloat covers "telemetry, Xbox, consumer and OEM extras".
  - 2-research.md §5.3: "OEM extras only where the KB has a matching entry". §6 then puts "any third-party or unknown item"
    in the never-in-preset set.
  - CheckGuard enforces that (plan.cpp:196-197, "third-party or unknown: no Hadron KB entry").
  - kb.py has no OEM entries (grep "oem": none).
  - Analyze E3 notes it.
  - DECISIONS.md:21 lists the research revisions but does not mention dropping OEM/third-party from presets.
- **What diverged:** a feature in the idea's text is unreachable, and no decision row records it. The narrowing is a
  sound safety call within rules 1 and 7, but it changes part of the idea's Pure-bloat meaning without a log.
- **Finalize:**
  - Proxy question Q4: log the narrowing in DECISIONS.md, or add sourced OEM entries (for example MSI's own services on
    the owner's board) with a basis.
  - If OEM entries are added, they need a KB build key and kb-auditor.

### F6. Preset levels across the two tabs: same mechanism and meaning, thin on Drivers. Doc/info, no defect
- **Evidence:**
  - One BuildPreset(level, tab) and one BuildDefault(stock, tab) serve both tabs (plan.cpp:205, 283). Both use the same
    KB level tags, CheckGuard, dependency check and "start type only, effective after restart" note.
  - Drivers add only the `sysok` gate (plan.cpp:241-244), as the idea's "System-start only from a curated list" asks.
    The gate actually applies to every driver, which is stricter than "System-start only", and genkb enforces it.
  - Console keeps all five chips (F7-F11, module.cpp:2446, 2948).
  - Content: Drivers Pure bloat is always empty, by KB design (0 driver bloat entries). The plan says so
    (plan.cpp:268-270, golden plan-drivers-bloat.txt).
  - Drivers Minimum is Stripped plus `luafv` (goldens: 6 vs 5 ticked).
- **What diverged:** nothing in meaning. On Drivers the four levels show almost no difference. That follows from F4
  (driver coverage), not from the levels themselves.
- **Finalize:** nothing to do beyond F4. You could add one plan note on Drivers Minimum saying it only goes beyond
  Stripped for KB `sysok` drivers.

### F7. The TI toggle's Defender/PPL caveat disappears at narrow widths. Suggested category 6 / 5
- **Evidence:**
  - 2-research.md §5.6: the toggle's "label says what it unlocks ... and what it doesn't (Defender, PPL, Tamper Protection)".
  - module.cpp:2082-2092 (and Console 2779-2782) fall back to "Only for writes Windows refuses to administrators." and
    then "For admin-refused writes.", or no hint at all, when the full caption doesn't fit. The status line on toggle
    (module.cpp:1187) doesn't mention it either.
  - Context: 1-idea.md feature 6 named WinDefend as the example; research §0.1 corrected that with a source (PPL plus
    Tamper Protection). That is a factual correction, not a change to the idea, and the UI is honest where it shows the
    caveat. The detail pane says it per item (module.cpp:1817-1828).
- **Otherwise aligned:**
  - The toggle is off at every launch and never persisted (module.cpp:363).
  - `--ti on` works only in fixture mode (module.cpp:557).
  - It is per feature: TI is used for marked config writes only, and is not applied to control plans (module.cpp:1027).
  - TI is taken only when marked rows exist (module.cpp:1134).
  - TrustedInstaller's own state is restored (ti.cpp:114-138).
- **Finalize:** add "Not Defender, PPL or Tamper Protection" to the toggle-on status message, or to the plan note that
  lists TI rows, so the caveat shows at every width.

### F8. Measured before/after RAM and process count: deferred, with no predictions anywhere. Scope/doc, aligned
- **Evidence:**
  - Proxy feature 9 (1-idea.md:43) was later deferred by the proxy in research (2-research.md §0.6 and §2; DECISIONS.md:21).
  - 3-build.md:85 "Not done".
  - No RAM, process or savings text appears in module.cpp or plan.cpp (grep). Basis labels only.
- **What diverged:** nothing the owner said. He never asked for measurements; the proxy added them and the proxy
  deferred them, which is allowed because it does not change the idea. Custom presets with export/import, the signer
  column and the event 7040 account follow the same route.
- **Finalize:** carry the deferred list into 7-finalize.md, so Push files them as follow-up ideas (2-research.md §2).
  Keep the "no predicted savings" property: any new text must go through kb-auditor.

### F9. The Hadron column's meaning moved from the spec, and Design's decisions are not in DECISIONS.md. Doc
- **Evidence:**
  - 2-research.md §5.1 asks for a column "Preset target for the selected level".
  - Built: a single "Hadron" column showing the lowest level an item joins after guards and hardware evidence
    (`KbVerdict`; baseline.md §4, decision 5).
  - baseline.md:207-218 lists ten decisions "resolved without asking". Decision 3 was reverted in 5-design.md.
  - Console decisions are in console.md.
  - None of these is in DECISIONS.md; only rows 20-24 exist for svcdrv.
- **What diverged:** a spec column was redefined. The new meaning is sensible, and the list has no "selected level"
  outside a plan. PIPELINE.md asks for proxy answers to be logged.
- **Finalize:** log the Design decisions, and the Hadron-column meaning, in DECISIONS.md tagged `proxy`.

### F10. Design changed `core/`, which the spec put out of scope. Suggested category 4 (contract) / doc
- **Evidence:**
  - 2-research.md §3: "Out of scope (must not touch): everything else under `core/`".
  - Design commits:
    - 6e4df2b changed core/host/cmdline.* and host.cpp (`--accent`);
    - 667ca24, 19e4fd9 and b0ed957 changed core/ui/widgets.cpp (focus rect, sort-arrow slot, selection text);
    - 9209b03 (font) was reverted.
  - `git diff origin/main...HEAD -- core`: 6 files, +49/-9.
  - 5-design.md says the lead made the `--accent` change and the widgets change has no API change. CLAUDE.md lets only
    the lead own core.
  - No DECISIONS.md row covers either change.
  - 5-design.md asks Review to re-check the other modules after the font revert.
- **What diverged:** a scope boundary was crossed. It is probably fine, because there is no API change and the lead
  approved, but it is unrecorded.
- **Finalize:**
  - Record both core changes and their approver (lead) in DECISIONS.md.
  - Make sure the Looks group's DiskMap, BIOS and RunAs before/after renders exist on the final tree.

### F11. Branch is behind origin/main. Doc, for Push
The local `main` ref is stale. origin/main is at 122d2ad (#5 "BIOS Settings: guarded writes"); this branch forked at
6835fd4. CMakeLists.txt, core/host/registry.* and ci.yml are touched on both sides.
**Finalize/Push:** rebase or merge origin/main before the final pass, so goldens and CI run on the tree that will merge.

## Decisions checked and found carried through (no finding)
| Decision | Where | Code |
|---|---|---|
| Services get live start/stop/restart; drivers start type only, no live unload (proxy 1) | 1-idea F3, 2-research §5.2 | module.cpp:2164-2176 (Start/Stop/Restart only `!drv`), Control plans with confirm and log, no undo |
| Presets change start type only, after restart, Hadron never reboots (Q2) | 2-research §5.3 | plan.cpp:278, 361 note |
| Defender stack, TI, VSS and swprv never in presets (Q4) | §6 | CheckGuard NeverPreset, plan.cpp:194 |
| Contested items never in Pure bloat (Q7) | §5.3, DECISIONS:24 | genkb gate, a-champion confirms |
| Stripped only on never-seen hardware; hot-plug unticked (Q9) | §5.5, DECISIONS:23 | plan.cpp:248-262, goldens |
| Registered description shown, labelled not Hadron's (Q6) | §5.1 | module.cpp:1836-1838 |
| Data dir in ProgramData with an admin/SYSTEM DACL (Q10) | §5.4 | store.cpp (A5 fixes) |
| Recovery as a folder plus .zip (rule 6 reading) | DECISIONS:22 | recovery.cpp, goldens |
| Boot-start drivers never touched; System-start only with sysok | idea risks, §6 | CheckGuard plan.cpp:164, BuildPreset 241-244 |
| Drift shown, never re-applied | §5.7 | module.cpp:698-701, 1541-1545 |
| No-screen-reader note (CLAUDE.md UI) | 5-design | status line |

## Open owner-gap items Finalize must resolve
1. **Empty Windows stock table (F2/F3).** Fill it, or relabel/disable it. Add a stock golden.
2. **KB coverage (F4).** Expand through kb-auditor, or log a deferral and file it.
3. **OEM extras in Pure bloat (F5).** Log the narrowing, or add sourced entries.
4. **The TI vs WinDefend expectation (F7).** Make the caveat always visible.
5. **Analyze leftovers (4-analyze.md:153-156).** M3, M8, M12, F3-F5, re-reading live dependents at apply time, en-us
   KB URLs, UNC data dir, `FileCompany`'s unchecked `ExpandEnvironmentStringsW`, CI coverage of `:dry` and
   `restore-online.cmd`, and the first windows-latest run (MSVC, `--live-test`, A1 on real cmd).
   - ti.cpp:126 now sets `started_ = own` before the post-start Query. Check that the "wait for RUNNING fails" path still
     releases TI.
   - Check selftest.cpp:1035's `logs[0]` index in the release build.
6. **Needs-Windows list (4-analyze.md:76-86).** This includes the read-only check on the owner's PC inside the project
   folder. Analyze did not do it.

## Open questions for Finalize to put to the proxy
- **Q1 (F1).** At Minimum, should use-undetectable rows be ticked by default? Recommended: yes; hot-plug classes stay
  unticked. If not, log it and add a plan note saying Minimum leaves use-based items to the user.
- **Q2 (F2/F3).** Windows stock: (a) capture a fresh 19045 VM's SCM start types as the table, or (b) relabel or disable
  the chip and say "no data yet".
  - Caution for (a): rule 7's bases are owner-measured, published, reasoning and folklore. A pipeline VM capture is not
    an owner measurement. The proxy can't make a rule-7 exception, so (a) needs a basis the generator accepts and
    kb-auditor approves, for example `published` if Microsoft documents the defaults. Otherwise take (b).
- **Q3 (F3).** Should the first-snapshot chip or plan state "as Hadron first saw this PC (date), not Windows' default"?
- **Q4 (F5).** Log "third-party/OEM never in a preset" as a deviation from 1-idea.md's Pure-bloat text, or add OEM KB
  entries?
- **Q5 (F4).** KB expansion in this run: what target, for example explanation-only `never` entries for every core
  stock service and common inbox drivers? Or defer to a follow-up idea?
- **Q6 (F9/F10).** Log the Design decisions (Hadron column meaning, column priority, keyboard) and the two core changes
  in DECISIONS.md.

No stage changed what the idea is. The research corrections (TI can't unlock Defender; storage drivers never get a stock
value; contested items stay out of Pure bloat) narrow features for factual or safety reasons. The deferrals affect
proxy-added features only, and DECISIONS.md:21 logs them. The two unlogged narrowings are F1 and F5.
