# svcdrv: Finalize (stage 7)

Stage 7 (Finalize), 2026-11-11, on `module/services-svcdrv`, from Review's hand-off at 907d435. Every write-up named
below is in `finalize/`. Fixture only: no preset was ever applied on a real PC, and none may be on James's.

## Verdict
**Ready for Push.** All 25 Review findings (R1-R25) are fixed in code with selftests. A fresh re-review of the merged
result confirmed each one closed (`finalize/mini-review.md`). Every category 1-4 finding that the mini-stage passes raised
afterwards is also fixed, so no category 1-4 finding is open. Category 5 and 6 are done; the KB passed kb-auditor
twice more (`kb-audit-2.md`, `kb-audit-3.md`), and every must-fix from both is applied.

## What happened, in order
1. **Main merged in** (bee75a9). Main's narrowed firmware guard was kept and the services API guard added after it;
   DECISIONS rows came from both sides. A merge was used instead of a 37-commit rebase, so the stage history stays intact.
2. **Proxy answers Q1-Q7** (`finalize/proxy.md`, logged in DECISIONS as `proxy`):
   - Q1: Minimum ticks rows whose use can't be detected.
   - Q2: "Windows stock" says it has no data yet; there's no published client source.
   - Q3: the first snapshot is labelled "as first seen (date)".
   - Q4: OEM narrowing logged.
   - Q5: KB expanded with explanation-only entries.
   - Q6: core widgets/`--accent` changes kept and logged.
   - Q7: no core font change.
3. **Three builders in parallel**, each on its own branch:
   - logic and data safety, `module-builder-careful` (`finalize/logic.md`): R1-R9, R11-R18, R20 plan side, R22, R25,
     plus the recovery README text;
   - UI (`finalize/ui.md`): R10, R19, R20/Q3 labels, R21, R24, Q7, category 5, and the module.cpp category 6 text;
   - KB (`finalize/kb.md`): category 6 in kb.py, the number gate, and the Q5 expansion (88 + 11 entries became 265:
     111 services and 55 drivers added, no preset level; preset sizes unchanged: bloat 17, stripped 48, minimum 11).
     kb-auditor's must-fix and should-fix items from `kb-audit-2.md` were applied.
4. **Merged and rebuilt** (e3cfd92). The plan goldens were regenerated for the KB rewording only. Empty plans now show
   their reason, and an unreadable item explains itself.
5. **Six mini-stage passes** on the rebuilt program:

   | Pass | Write-up | Result |
   |---|---|---|
   | Research | `mini-research.md` | Win32 use checked against Microsoft Learn; I1/I2 in the Company lookup |
   | Build | `mini-build.md` | Both builds, every selftest, goldens and bios check pass; P1-P3 |
   | Analyze | `mini-analyze.md` | A1 (cat 1), A2-A6 (cat 2), A7 (cat 3), A8 (cat 6) |
   | Design | `mini-design.md` | 44 renders, 14 cat 5/6 items |
   | Review | `mini-review.md` | R1-R25 closed; N1 (cat 1), N2 (cat 4), N3, N4 |
   | Finalize | `mini-finalize.md` | 24 doc/code mismatches, the follow-up list and the Needs-Windows list |

6. **Round 2.**
   - Finalize fixed N1/I1/I2 itself: the Company lookup treats every reparse point as a leaf, reads attributes through
     FindFirstFileExW, and checks MUI satellites.
   - kb-auditor audited the late KB commits (N4, `kb-audit-3.md`), and its fixes were applied.
   - One builder closed everything else (`finalize/round2.md`), with a selftest for each, including:
     - A1: every folder above the data folder must be admin-owned and not renamable by others;
     - A2: a dismissed log stays undoable;
     - A3: recovery\first-N is written only for this PC and build;
     - A4-A6, A7, A8;
     - N3: the minimum size is clamped to the monitor;
     - P1-P3, every mini-design item, the README F overclaim, Company moved to a second Worker pass, and the doc refresh.
7. **CI fix.** The Windows live tests use `%SystemRoot%\Temp\...` as their data folder, because A1 rightly refuses
   RUNNER_TEMP, which the runner account owns.
8. **N2 (evidence for the core changes).**
   - Before/after renders of DiskMap, BIOS and RunAs are pixel-identical (`compare -metric AE` = 0). The one intended
     exception is BIOS on a green accent, where the text changed from white at about 2.3:1 to dark at about 9:1.
   - Renders are in the project's shared files: `pipeline/svcdrv/finalize/ui/other-modules/`, `finalize/round2/`.
   - Final services renders are in `pipeline/svcdrv/finalize/final/`.

## Final pass (at the hand-off commit)
- **Builds:** MinGW Release with test hooks and plain Release, clean configure, both build. The only warnings are the
  24 that were already in core/host/host.cpp.
- **Selftests:** hello, uikit, runas (standalone and `--solo`) and diskmap with fixture pass.
  - `services_check.sh`: genkb selftest passes and `services.db` is byte-identical; fixture cmp OK; selftest
    **282/282** standalone and in the suite; 10 plan goldens plus delayedlinger match; 17 shots.
  - `bios_check.sh` and `diskmap_check.sh` pass.
- **Release exe:** `--live-test` refuses (exit 2, "not in this build").

## Rebuttals and choices (not fixed, on purpose)
- **mini-design item 3:** the Baseline Name cell stays at 15px. Per proxy Q7 there is no core font change, and
  baseline.md §2 is amended.
- **Pending-restart marker:** it differs by layout, "(pending)" in Baseline and "!" in Console. Each is in its own
  legend; recorded in services.md.
- **Suite at 1024 px:** only Name, State, Start type and Preset fit, so Company drops there. At 1280 px the suite keeps
  Company and drops Display name. Recorded in baseline.md §3.
- **A data folder under a user profile is refused, by design (A1).**

## For Push
- **Docs Push owns:**
  - `docs/modules/services.md`, README, `docs/MODULES.md`, `docs/PROGRESS.md`.
  - `docs/ARCHITECTURE.md:80` names `hx::ui::Confirm()`, which doesn't exist (pre-existing contract text; fix or remove).
  - DECISIONS rows still to add:
    - the Preset-column redefinition (Design);
    - R25 (`--live-test` only in the test-hooks build);
    - Dismiss writes a `dismiss:` log;
    - applies are refused while first.txt is missing (R12) or is from another PC (A6);
    - `--readonly` opens the default data folder read-only;
    - A1's ancestor rule.
  - The rows for core/host minimum size and the core/ui contrast helper are already added. The N3 clamp is part of the
    same minimum-size change.
- **Windows-only checks.** The first `windows-latest` CI run is the first:
  - MSVC compile of this module;
  - `--live-test` (and the TI variant, informational);
  - restore-script run on real cmd.exe;
  - EnsureRoot run against real folder owners (A1/R1; skipped under Wine).
  If one fails beyond a small fix, send it back here.

## Follow-up ideas (Push files these)
1. Windows-stock start-type table for 19045, from a source rule 7 allows.
2. Sourced OEM entries (MSI services on MS-7E06 and others).
3. The rest of the KB: Fax, plus the drivers listed in `finalize/kb.md` "left out" (usbvideo, usbprint, wcifs, SMB
   redirector, Hyper-V, and others).
4. Custom presets with export/import.
5. Measured before/after RAM and process count (owner-measured basis).
6. Signer column (Authenticode), next to the Company "file's own claim".
7. Event 7040 account in drift.
8. Interface-trigger and last-arrival hardware evidence (DECISIONS 2026-11-11 deviation).
9. Registry-write fallback for items the SCM refuses.
10. Lead/core: the TI helper into core; a `--size`/`--dpi` fixture-render flag; ListView and theme APIs; a shared
    F-key bar; an opt-in per-list font.

## Needs James's PC or real Windows (read-only only; never apply a preset on his PC)
- Class filters, boot path, network names, VBS, load time, high contrast, DPI and a keyboard pass.
- The N3 minimum-size clamp at 1366x768, 125%.
- Run from the agreed project folder (C:\Users\user\Desktop\Hadron) with a prebuilt exe; Remote Control there can't
  compile or run the Python fixture scripts.
- In CI: TI against WaaSMedicSvc, per-user service templates, the stop reason code, delayed-start and grouped-service
  behaviour, WinRE `reg load`, and restore-online.cmd.

## Hand-off
To **Push** at the commit that adds this file.
