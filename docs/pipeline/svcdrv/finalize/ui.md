# svcdrv Finalize: UI findings

Branch `finalize/svcdrv-ui` (from fdd5c31). `M` = modules/services/module.cpp. Selftest lines are the `check(...)` text
in `ServicesModule::UiSelftest` (M:3843), run by `--selftest` (212 checks, 0 failed). Renders are
`tests/tools/services_baseline_shots.sh <build> <out> finalize` (`fin-*`), the existing sets, and services_check.sh's
`services-*`; copies are in `/mnt/project-files/pipeline/svcdrv/finalize/ui/`.

## Review findings

| Finding | Fixed how | Where | Test / render |
|---|---|---|---|
| R10 ticks | A rebuild (TI toggle, F6, reload) restores the user's ticks by lower-case name on toggleable, unblocked rows, re-runs DependencyCheck and says "kept N tick(s)"; a new plan clears them | M:1140 OpenPlan(rebuild), M:1240 ApplyUserTicks, M:1263 | "ui R10: after the TI toggle rebuilds...", "...back off", "...after a reload", "a new plan from the chip starts from the preset's own ticks" |
| R10 TI during apply | ID_TI refused while the apply worker runs; switch drawn disabled with no click target in both layouts; F6 greyed | M:1335, M:2269 TiToggle, M:3159, M:2801ff | "ui R10: during an apply the TI toggle doesn't flip (click or F6)..." |
| R19 | The [x] column toggles only on a single click on a real row (`r && !dbl`, below the header) | M:1414, M:1557 | "ui R19: a click ... below the last row toggles nothing", "a double-click ... toggles it once" |
| R20 / Q3 (UI side) | Chip and F8: "Default: Windows stock (no data for this build yet)" (F8 "Stock (no data)", short "Stock-"); first-snapshot chip "Default: as first seen (YYYY-MM-DD)"; F7 "1st seen" | M:2241, M:2248, M:2806 | "ui: first-snapshot chip reads...", "ui: Windows-stock chip says it has no data...", "ui: Console F7 reads \"1st seen\"..."; baseline-dark, console-dark |
| R21 | Result-banner Undo is shown and accepted only when `resultFresh_` (set by the post-apply reload) and no load runs | M:1389, M:1700 | "ui R21: Undo is off in the result banner ... until the reload's logs are in" |
| R24 min size | core/host WM_GETMINMAXINFO: sidebar + 800x560 logical content + status bar, DPI-scaled; not applied to --screenshot | core/host/host.cpp:126, :272 | "ui R24: Baseline/Console list/plan at 800x560" (no overlaps, targets inside the page, Preset shown, columns fit); min-* renders |
| R24 narrow | Start-type strip wraps; preset chips wrap under, filter clamped 180..320; Console F-key bar has full/short/caption-only tiers and never draws under the LIST tag; Baseline columns drop in order (Display, Image, Type...) and Name/Start shrink last | M:2055 SetListColumns, M:2252 PresetChips, M:2815 PaintFKeyBar | min-baseline-*, min-console-*, suite1024-* |
| R24 DPI | WM_DPICHANGED rescales detail, Console left column, list and plan scroll offsets by new/old DPI and re-reveals the selection | M:666 OnDpiChanged | "ui R24: WM_DPICHANGED rescales the detail's scroll offset (200 -> 300 at 144 DPI)"; dpi120-*, dpi144-* |
| Q7 | Baseline Name column sized from the tab's widest name (120..180 logical px, cached), Name has priority over Display/Image; full name in the detail | M:2090 | baseline-suite-dark, suite1024-baseline-dark |

## Category 5 (6-review.md)

| Bullet | Fixed how | Where | Test / render |
|---|---|---|---|
| TI toggle looks like a tab chip; "On" in accent; "F6 TI off" reads as an action; caveat drops narrow | Outlined switch when off, warn fill "TrustedInstaller: ON (writes as TI)" when on; warn banner while on with "Turn off"; F6 "TI toggle: off/ON"; caveat has three lengths and never drops | M:2269, M:1640, M:2801ff | fin-*-plan-ti-light, baseline-dark, min-console-dark |
| "protected" drawn in error red | Protected/blocked text uses text/mute colours; "Protected:" line in text colour | M:3020, ItemDoc/PlanRowDoc | fin-*-protected-red |
| Five ways to say "won't change"; header; legend | Header renamed "Preset"; "not in KB" replaces the em dash; "kept (hw)"; legend under the Baseline list and in Console F1 covers vs 1st, LIST/PLAN tag, +t, *, (pending) | M:895, M:903, M:2066, M:2303, M:3353 | "ui: an item Hadron's KB doesn't know shows \"not in KB\""; fin-console-help-dark |
| Baseline plan: no Basis, no arrow, Why cut; Apply-off reason ellipsised; banners/notes cut with no "..." | Plan columns box/name/now/"-> after"/basis/why with narrow drop order; "Apply is off: <why>" on its own wrapped line; banners wrap to 4 lines and notes to 3, both with "..." | M:2143, M:2558, PaintBanners | baseline-plan-stripped-dark, suite1024-baseline-plan, min-baseline-interrupted |
| Hover contrast; three copies of text-on-accent; BIOS 2.3:1 on green | One `ui::Contrast`/`ui::OnFill` (core/ui/theme.h:44, :48); widgets selText, Services, BIOS use it; hover moves the fill away from its ink (HoverAway) | core/ui/theme.cpp:127, M:88, modules/bios/module.cpp | "ui: OnFill picks white on 0078D7 and the dark ink on 10C010"; bios-green diff below |
| Disabled Apply looks active; "sysok"; raw preset id/stamp; third-party "protection above" text; Console cuts last row | Disabled controls use their own fill and label (no accent); interrupted banner shows source title and "YYYY-MM-DD HH:MM UTC"; third-party text reworded; Console grid snaps to whole rows. "sysok" is in plan.cpp (other builder) | M:117-153, M:225, M:232, M:1965, M:3039 | baseline-plan-ti-light, *-interrupted-boring, fin-*-thirdparty-dark |
| Console split by heading text; left column not keyboard-scrollable; no tab stop | Paragraphs carry a side (SIDE_LEFT/RIGHT); left column is its own tab stop (ID_FOCUS_DETAIL_L) and scrolls with keys | M:161, M:1970, M:2016, M:2724, M:62, M:2359 | "ui: DiagTrack's Console detail has Configuration on the left...", "ui: a KB title starting ..." |
| High contrast mixes; warn/error differ by layout; no focus on an empty-selection list | HC uses system pairs only (HIGHLIGHT/HIGHLIGHTTEXT, BTNTEXT, GRAYTEXT, HOTLIGHT, WINDOWTEXT) in both layouts; focus rect when focused with no selection | M:117-153, CPal, PaintListMode, Console grid | not rendered (Wine has no HC theme); code read |
| Suite name cut; "Display name" header cut; no image path; pending restart only in detail | Name sizing (Q7); Display header shortens before cut; Image path column; Start shows " (pending)" | M:2068, M:983 | baseline-suite-dark, wide1920-* |
| No TI toggle on Baseline's plan | Switch next to Cancel; rebuilds and keeps ticks | PaintPlanMode, M:2269 | fin-baseline-plan-ti-light |
| "remova l"; Account and Type truncated at 1920 | Key column at least the longest single word (max W/2); slack goes to Account/Type/group/image | PaintDoc, M:2055 | wide1920-baseline-dark |

## Category 6, text in module.cpp

| Bullet | Fixed how | Where | Test / render |
|---|---|---|---|
| Legend "the lowest preset that changes it" | "the first preset that lists it (it may already be set, or be offered unticked)" | M:2294-2300, M:3352 | baseline-dark, fin-console-help-dark |
| Console help "every key changes the view" | "No key applies a plan or changes a service: Apply is a click. Keys change the view, open a plan, flip the TrustedInstaller toggle, or (Esc during an apply) stop after the current item." | M:3333 | fin-console-help-dark |
| Third-party text (~1785) | "No KB text for it, so it is never in a preset (third-party or unknown to Hadron). The company ... is what the file says about itself (its version resource), not a verified signature." | M:1965 | fin-*-thirdparty-dark |
| Company is the file's own claim | Detail label "Company (file claim)"; list legend line "Company: what the file says about itself, not a verified signature" (header kept "Company") | M:1983, M:2300 | baseline-dark |
| "TI toggle does not help" for protected services (untested) | "...is not expected to help (reasoning, not tested)" | M:1996 | fin-*-protected-red |
| `--readonly` without `--data-dir` | Read-only also opens the default data folder, read-only (first.txt, logs, interrupted banner); StartLoad never writes it | M:650 | code read; no selftest (it needs %LOCALAPPDATA% state) |
| Ctrl+F and Tab in Console help | Ctrl+F row; Tab and Shift+Tab rows in the right column (left one is full at 800 px high) | M:3338, M:3364 | fin-console-help-dark |
| Text-on-accent helper | See the hover row in category 5 | core/ui/theme.h:44 | bios-green |

## Renders in other modules (`compare -metric AE`, before = fdd5c31)

29 renders (DiskMap, BIOS, RunAs; both layouts, four themes; green accent; suite). 28 identical. `bios-green`: 354 px,
intended (Export changes and the selected chips switch from white text, ~2.3:1, to dark, ~9:1).

## For DECISIONS.md

- core/host: frame minimum = 800x560 logical content (DPI-scaled) + sidebar + status bar; modules degrade down to it; --screenshot is not clamped.
- core/ui: `ui::Contrast` and `ui::OnFill(fill, light, dark)` are the one text-on-fill rule (WCAG; light wins at >= 4.45 so 0078D7 keeps white at 4.499).

## Ambiguities resolved

- "Hadron" column renamed "Preset" (review suggested it); "Company" header kept, with the file-claim note in the legend and the detail label.
- Minimum size 800x560 logical content: the smallest at which both layouts lay out without overlap.
- F8 short label "Stock-" in the short F-key tier.
- A warn banner while TI is on (review asked for one); its "Turn off" hides during an apply.
- 4.45 threshold kept (matches widgets.cpp so no existing render moves).
- The red accent's hue is not changed for "bad"; protected simply stops using it.
- "Windows stops it on its own when idle" is in the R4 banner (out of scope); "a desktop rarely needs it" is KB data (data builder).
- Not done here: the plan's Q3 header note and the empty-stock note (plan.cpp, other builder), "sysok" in plan reasons and the Drivers "near-empty" note (plan.cpp).
- cli.h gained one optional parameter on RunSelftest so the page's checks run inside `--selftest`.
