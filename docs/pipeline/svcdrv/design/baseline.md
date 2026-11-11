# svcdrv: Design, Baseline layout

Stage 5 (Design), Baseline designer, 2026-11-11. Branch `design/svcdrv-baseline` from `module/services-svcdrv` at
6e4df2b. Scope: the default Segoe UI card look of `modules/services` in the four themes, the shared UI defects from
`4-analyze.md`, and the two core/ui fixes allowed (U2 focus rect, sort arrow), plus two more small ListView changes
argued below. No change to plan.cpp, store.cpp, recovery.cpp, ti.cpp, the backends, the KB or presets; the plan
goldens are byte-identical.

Screenshots: `/mnt/project-files/pipeline/svcdrv/design/baseline/` (the "before" shots from 6e4df2b are in `before/`).
Reproduce them with `tests/tools/services_baseline_shots.sh <build> <out> [main|extra|focus|all]` (fixture only).

| Shot | Shows |
|---|---|
| `baseline-light.png`, `-dark`, `-fun`, `-boring` | The four themes, `--accent 0078D7`, DiagTrack selected |
| `baseline-plan-stripped-dark/-light.png` | Plan mode |
| `baseline-plan-ti-stop-light.png` | The Stop TrustedInstaller plan with its new Why (`--plan ti-stop`) |
| `baseline-suite-dark.png`, `baseline-suite-sort-company-light.png` | Inside Hadron.exe (1092 px content); Company still visible; sorted by Company, descending |
| `baseline-sort-hadron-dark.png` | Sorted by the Hadron column (level order), selection centred |
| `baseline-bluetooth-dark.png` | BluetoothUserService: column "kept" agrees with the detail's "kept by every preset" |
| `baseline-accent-grey-fun/-light.png` | Achromatic Windows accent (808080): each theme's fallback accent |
| `baseline-accent-red-dark/-boring.png`, `-yellow-light`, `-green-light` | Accent following, including a light accent that needs black text |
| `baseline-focus-list-<theme>.png` | U2 fixed: the list focus rect on the selected row in all four themes |
| `baseline-focus-chip-dark.png`, `-preset-light`, `-plan-boring` | The keyboard focus ring on a chip, a preset and a plan button |
| `baseline-drivers-light.png`, `baseline-interrupted-boring.png` | Drivers tab (row 0 selected on load); banners with actions |

## 1. Colour and contrast

**Problem.** Measured WCAG 2 contrast of the theme tokens (the worst of panel, alternate row/chip `panel2`, and page `bg`):
text3, warn, good and the accent fell to 2.4–3.9:1 when used *as text* (light and boring worst; dark/fun only for the
accent and text3). They are used as text for KB headings (accent), protection lines (bad), warnings and the plan
summary when Apply is off (warn), and "Fixture data: applying is off." (text3).

**Choice.** Fills keep the theme's colours and the Windows accent exactly as they are (chip, selected row, primary
button, banner bar). For text, the module derives a *text form* of each: the same hue moved towards black (light
backgrounds) or white (dark ones) in 5% steps until it reaches 4.5:1 on panel, panel2 and bg (`Readable()` in
module.cpp; members `accT`, `warnT`, `badT`, `goodT`, and `faint`). This is the owner's rule applied literally:
accent lightness is adjusted per theme for contrast; the semantic colours stay fixed per theme (each theme always
gives the same text form). High contrast still overrides everything (system colours, unchanged).

| Theme | text1 | text2 | faint (was text3) | accent text (0078D7) | warn text | bad text | good text | focus ring (text1) |
|---|---|---|---|---|---|---|---|---|
| light | 15.1 | 5.4 | 2.7 → 5.0 #62666E | 3.9 → 4.7 #006CC1 | 3.3 → 4.8 #906010 | 4.4 → 4.8 #BE3934 | 3.3 → 4.8 #207848 | 16.1 |
| dark | 13.0 | 6.1 | 3.3 → 4.8 #898F99 | 3.5 → 4.7 #3393DF | 8.4 (unchanged) | 5.3 (unchanged) | 7.6 (unchanged) | 14.4 |
| fun | 12.0 | 6.7 | 3.9 → 4.5 #9C8CB2 | 3.1 → 4.6 #3F99E1 | 8.5 (unchanged) | 4.8 (unchanged) | 8.2 (unchanged) | 14.0 |
| boring | 9.2 | 4.5 | 2.4 → 4.9 #60594C | 3.2 → 4.5 #0060AC | 2.7 → 4.9 #765415 | 4.1 → 4.8 #993F2D | 3.1 → 4.9 #436134 | 10.3 |

Text on fills (selected row in core's ListView, "on" chips, primary buttons): previously black/white by a Rec. 601
brightness threshold with near-black/near-white. It picked white on a bright green accent (2.3:1) and near-white gave
4.2:1 on the default blue. Now: pure white while it reaches 4.5:1 (to one decimal; white on the accent is the
Windows convention), else pure black, which is then above 4.6:1.

| Accent fill | Text now | Ratio | Before | Ratio before |
|---|---|---|---|---|
| 0078D7 (default, light fallback) | white | 4.5 | near-white | 4.2 |
| E81123 | white | 4.6 | near-white | 4.3 |
| FFB900 | black | 12.2 | near-black | 10.9 |
| 10C010 | black | 8.6 | near-white | 2.3 |
| 60A0FF (dark fallback) | black | 8.0 | near-black | 7.1 |
| FF50AA (fun fallback) | black | 7.0 | near-black | 6.2 |
| 966E46 (boring fallback) | white | 4.5 | near-white | 4.2 |

Other pairs checked (text1 on): warn/bad/accent banners 9.1–14.3; the current start-type button (panel + 35% accent)
7.2–10.5; chips 9.2–15.1, hovered chips 7.9–13.7. UI boundaries: the list/detail borders are 1.2–1.6:1 against
the panel in every theme (as built). They are decorative here (the panels are also set apart by fill and the content
is all text), so they were left; the focus indicators, which must be seen, are ≥ 4.5:1 (list) and ≥ 10:1 (ring).
Disabled controls stay faded on purpose (WCAG exempts them; every one has a reason line nearby).

## 2. Fonts and hierarchy

The list drew its first column at 15px and the rest at 12px: two sizes on one baseline, and the Name column lost
about a fifth of its width ("BluetoothUserServ…", "BrokerInfrastruct…"), which is what pushed Company out of the
suite. **Decision:** one cell size per list, Segoe UI 12px (9pt, the Windows body size); the first column keeps text1
against text2 for the others, so it stays the anchor. This is a one-line core/ui change (commit 9209b03, separate so
it can be dropped alone). Headers stay 15px semibold (one header row, scanned once). Page hierarchy is unchanged:
20px title, 12px source line, 15px semibold detail title and section heads (now in the readable accent).

## 3. Column plan

List width is 60% of the content: 732 units standalone at 1280x800, 619 in the suite (188 px sidebar), about 674 in a
1200x800 window. Old plan: Type, Account and Company were the first to go, so none of them ever showed below ~1700 px.

What matters in the list is "what is it, what state, what start type, what would Hadron do" (always shown), then
**Company**, because third-party and OEM items are how bloat stands out and they are never in the KB, then **vs
first**. Basis, Account and Type go first: the detail pane shows each in full (the basis is always in the detail, so
the UI still shows it, CLAUDE.md rule 7).

| Column | Services | Drivers | Dropped (1 first) |
|---|---|---|---|
| Name | 140 | 124 | never |
| Display name | flexible, ≥ 100 | flexible, ≥ 100 | never |
| State | 72 | 72 | never |
| Start type | 116 | 90 | never |
| Hadron | 84 (fits its title beside the sort arrow) | 84 | never |
| Basis | 76 | 76 | 3 (services), 2 (drivers) |
| vs first | 70 | 70 | 4 / 3 |
| Company | 100 | 100 | 5 (last) |
| Account / Load group | 110 | 116 | 2 / 4 |
| Type | 96 | 96 | 1 |

Result: 1280x800 standalone shows Name, Display (150), State, Start type, Hadron, vs first, Company. The suite and a
1200x800 window show the same without vs first. Drivers show Company everywhere and Load group at 1280. Every column
fits from about 1670 px of window. The Company column drops a trailing legal form (" Corporation", " Inc.", " Ltd.",
" GmbH"…): "Microsoft Corporation" becomes "Microsoft"; the detail pane and the filter use the full text the file
claims. The filter placeholder is now "Filter by name, company or group" (it no longer ellipsises; it still matches
display name and account too).

Console keeps the build's table unchanged (Consolas cells are wider and the Console designer owns them).

## 4. The Hadron column

Before: the KB entry's lowest level, ignoring guards that only BuildPreset applied and the hardware evidence
(BluetoothUserService: "stripped" in the list, "kept by every preset" in the detail), sorted as text.
Now `KbVerdict()` runs the same per-item tests as BuildPreset (CheckGuard, a driver needs `sysok`, HwEvidence), with
the existing plan/KB code, no preset change:

| Label | When | Sort rank |
|---|---|---|
| bloat / stripped / minimum | the level it joins, when nothing keeps it | 0 / 1 / 2 |
| kept | KB hardware hint present or seen before (detail: "kept by every preset") | 3 |
| no preset | in the KB but never in a preset (KB `never`, launch-protected, never-preset list, other build, driver without sysok, no level) | 4 |
| protected | never touched (CheckGuard neverTouch) | 5 |
| — | not in the KB | 6 |

Order is "goes first" to "never touched", so an ascending sort reads as a removal order. A hot-plug class that a
preset offers unticked still shows its level (the plan explains it). The column does not look at the current start
type, so "bloat" on an item that is already Disabled is still bloat.

## 5. Selection and sorting

- **U1 (stale row after a tab switch).** The list kept its row index while `selName_` was cleared. Now whenever
  nothing is chosen (first load, tab switch) the first row is selected and scrolled to, so highlight and detail
  always show the same item.
- **Selection off-screen after a sort.** After a header click the selected row is scrolled to the middle of the list
  (rows below and above are revealed first, then the row). `--select` centres the same way. Plain keyboard moves still
  scroll minimally.
- **Saved sort by key.** The sort is stored as a column key (`sortKey` pref; the old index-based `sort` value is
  ignored). If that column does not fit at the current width the list shows name order until it fits again, so the
  order on screen always matches the header arrow; the choice is kept. The arrow now also shows on Name by default and
  after a restart (it was missing after a restart).
- **Sort arrow overlap (U5).** Core: a sorted column's title now ellipsises before a reserved 14 px arrow slot.

Core's ListView has no sort setter and no scroll setter, so the module drives them through public calls (a header
click; Select + Home/Down). Proposed core API (lead's call, not done): `ListView::SetSort(int col, bool asc)` and
`ListView::Reveal(int row, bool centre, const RECT&, const PaintCtx&)`.

## 6. Keyboard

Tab and Shift+Tab move through every enabled control in reading order (top, then left), built from what the last
paint drew, so the order follows the layout in both layouts and both modes:

- List mode: Refresh, Undo last apply (when enabled) → banner actions (Undo, Resume, Stop it, Dismiss) → Services,
  Drivers, TrustedInstaller switch → the five presets → filter → list → detail Start-type buttons → Start/Stop/Restart
  → detail text → back to the top.
- Plan mode: header buttons → banner actions → Cancel → plan list → row detail.

Disabled controls register no target, so they are skipped. Enter or Space activates the focused chip or button;
Left/Right move along a row like a toolbar; Escape returns to the list (or closes the plan, as before); typing a
character still goes to the filter. **Apply and Run are never tab stops, and the activation path refuses ID_APPLY
again, so no key can apply a plan** (Enter in the plan list still does nothing; Space still toggles the row).
The focus indicator is a 2px ring in text1 just outside the control (10.3–16.1:1 on the page in every theme, and
independent of the control's own fill). Focus moves back to the list when a control is clicked with the mouse or
becomes disabled.

**U2 (core).** The list focus rect was a dotted outline in the accent on the accent-filled selected row: invisible.
It is now drawn in the selection text colour (white or black by the rule in §1, or the system highlight text in high
contrast), inset 3 px inside the fill. The focus row is always the selected row, so this is the only case.

Screen readers: the status line after every load reads "Keyboard: Tab and Shift+Tab move, Enter or Space activates.
No screen-reader support in v1." (CLAUDE.md requires saying so; the status line is the one place every view shows.)

## 7. Plan screen

The "Stop TrustedInstaller" plan (the banner's "Stop it") said only "stop" in its Why column. It now says: "Hadron
started it for an apply that did not finish, and nothing stopped it. It is a Manual (on-demand) service: Windows
starts it again when it next installs updates or features." (Set in the module for that request only; BuildControl
is unchanged.) The summary line when Apply is off now uses the readable warn colour.

## 8. Core changes and their effect on other modules

All in `core/ui/widgets.cpp`, no public declaration changed:

| Commit | Change | Effect elsewhere |
|---|---|---|
| 667ca24 | Focus rect in the selection text colour, inset; sort arrow gets its own slot; WCAG text pick on the selection | DiskMap's lists now show keyboard focus (was invisible); titles of sorted narrow columns ellipsise instead of overlapping |
| 9209b03 | One 12px cell font in Baseline lists | DiskMap's and RunAs's first columns draw at 12px (were 15px). BIOS has its own list: pixel-identical. Console unchanged (Consolas) |
| 19e4fd9, b0ed957 | Pure white/black selection text, white while it reaches 4.5:1 | Same colours as before on the shipped fallback accents (white on blue, black on the dark/fun fallbacks) |

Re-rendered DiskMap (Baseline and Console, dark and light), BIOS (dark, light) and RunAs (dark, light) before and after:
BIOS 0 pixels changed; DiskMap and RunAs differ only in the first-column font and, in DiskMap Console, the now-visible
focus rect. Nothing truncates that did not before.

Proposed for core later (not done here): move the text forms (`accT`, `warnT`, `badT`, `goodT`, a readable text3)
into `Theme`/`Resolve` so every module gets them; the ListView sort/scroll setters above; per-cell colour so the
Hadron column could tint "protected" and "bloat".

## 9. Not done / limits

- High contrast can't be checked under Wine (it ignores the setting); the code path is unchanged.
- Only 96 DPI rendered. Widths are in 96-dpi units and scale with the DPI as before.
- Keyboard traversal has no automated test (selftest has no UI harness); it was checked by rendering focus states
  (`--focus list|search|detail|<n>`) and by reading the code paths. Needs a hands-on pass on Windows.
- The Display name column is about 107 px in the suite: names cut at ~16 characters there. Giving it more would drop
  Company; the detail title shows the full name.
- Rendering is Wine's Segoe UI substitute; real Segoe UI is slightly narrower, so Windows truncates less.
- The list's border contrast (1.2–1.6:1) was left as built (decorative, see §1).

## Decisions recorded (resolved without asking, within the owner's rules)

1. Text forms of accent and semantic colours are derived per theme for 4.5:1; fills keep the theme/Windows colours.
2. Selection and filled-control text: white while ≥ 4.5:1 (Windows convention), else black.
3. One 12px cell font in Baseline lists (core, separate commit).
4. Column priority: Company before vs first before Basis/Account/Type.
5. Hadron column labels and order (bloat < stripped < minimum < kept < no preset < protected < not in KB).
6. A sort key that doesn't fit falls back to name order (choice kept); arrow always shown.
7. After a sort, centre the selection; after a tab switch, select and show the first row.
8. Apply/Run excluded from Tab; Enter/Space activate other controls only.
9. The no-screen-reader note goes in the status line at every load.
10. Console keeps its column table; shared fixes apply to both layouts.
