# svcdrv: Console layout (Design stage, Console designer)

Branch `design/svcdrv-console`, from `module/services-svcdrv` at 6e4df2b. Fixture data only; nothing touched a
real service. Screenshots: `/mnt/project-files/pipeline/svcdrv/design/console/` (list below).

Before this, Console was Baseline with smaller paddings and Consolas. The owner's Console is a second layout:
Consolas everywhere, ~18 px rows, ID/path/basis columns, a **bottom** detail pane, an **F-key bar**, keyboard
first. This is that layout. Theme, accent and semantic colours follow the owner's decisions (four themes,
Windows accent with the per-theme fallback, semantic colours fixed per theme, high contrast overrides).

## Where the code is
All Console code is one block near the end of `modules/services/module.cpp` ("Console layout"). Shared functions
get one dispatch line each, so the merge with the Baseline branch stays clean:

| Shared function | Line added |
|---|---|
| `Paint` | `if (Console()) { PaintConsole(dc, rc); return; }` |
| `OnInput` | `if (Console()) { ... OnConsoleInput(...); if (handled) return r; }`; everything not handled falls through |
| `OnView` | `WM_SYSKEYDOWN`: F10 is delivered as a system key; in Console it is an F-key, not the menu key |
| `OnClickId` | `ID_CONKEY + action` (the F-key bar's click targets) goes to `OnConsoleAction` |
| `Create` / `LoadFixture` | `--keys` (fixture only): keys replayed through `OnInput` after the fixture loads |
| `Refilter` sort key | `image` (the new Image path column) |

Selection still lives in `list_`/`plist_`, so `Refilter`, `SelectName`, the plan worker and every shared path work
unchanged in both layouts. The Console grid only paints and handles input.

## Choices: what, why, how

**1. Own dense grid instead of core's ListView.** What: 18 px rows, 20 px header, Consolas 14 px cells, a colour per
cell. Why: core's `ListView::RowH` is fixed at 22 px for Console and colours a whole column alike (first column text1,
others text2); the owner asked for ~18 px and the useful signals here are per cell (running vs stopped, protected,
folklore, drift). How: `PaintConsoleGrid` draws over core's `ScrollBar` (wheel, draggable thumb, paging), header
clicks sort (the list through the existing `onSortChanged`, the plan through `ConsoleSortPlan`), Up/Down/PgUp/PgDn/
Home/End move, the selection scrolls into view, and the first show (`--select`, a new plan, a re-sort) centres the
selected row. Core change wanted (below) so this can go back to `ListView`.

**2. Bottom detail pane in two columns.** What: about 40% of the height under the grid (34% in a plan); a header band
with the ID in bold and display name, type and state; a one-line action strip; then two scrolling columns: left
holds warnings and Configuration (and the service's own description), right holds Hadron's KB text (what it does,
if it's off, how to test, basis, presets). Why: the owner asked for a bottom pane; a full-width pane of one column
gives 150-character lines, which read badly, and the two halves answer different questions (what is it configured
as vs what does Hadron know). How: `SplitDoc` partitions the same `ItemDoc`/`PlanRowDoc` paragraphs Baseline uses
(heads "Configuration" and "From the service" go left; the rest right), so no text is duplicated or rewritten. The
right column is `detSb_` (keyboard-scrollable with the shared Detail focus), the left has its own scrollbar.

**3. Columns.** What and why below ("Column set"). Fits 1280 px standalone at full width; in the suite (about
1070 px) the flexible columns shrink, and below that columns drop in a fixed order (account/type/group, then
basis, vs 1st and the image path), never the ID, state, start type or Hadron's verdict.

**4. F-key bar.** What: a 22 px bar at the bottom of the module's own window (above the host status line), key in
the accent, action in body text, disabled keys dimmed, a mode tag (LIST, ITEM, PLAN) on the right and a short hint
when it fits. Every cell is also a click target. Why: keyboard first, and discoverable without the help page. How:
`ConsoleKeys()` builds the bar from the same enable rules as the Baseline chips and buttons; labels shorten
("Snapshot" to "Snap") when the bar doesn't fit, as in the suite.

**5. Item keys as a sub-bar (F4).** What: F4 turns the bar into F1 Auto, F2 Delayed, F3 Manual, F4 Disabled, F5
Start, F6 Stop, F7 Restart, Esc Back, enabled exactly as the detail pane's buttons are. Why: one key per action for
the selected row without stealing letters (letters type into the filter) or using Ctrl chords nobody remembers.
How: each key calls the same `OnClickId(ID_SET/ID_CTL ...)` the buttons do, which opens a single-item plan.

**6. Plan safety unchanged.** No key applies a plan. Enter does nothing in a plan (shared code, kept); there is no
Apply key; the F-bar says "No key applies: click Apply"; Apply is the same click target with the same enable rule
(`CanApply && n > 0`) as Baseline. Fixture mode never applies (`CanApply` refuses, unchanged). F6 in a plan
rebuilds it with the toggle flipped, which is what Baseline's `ID_TI` already does in plan mode. Esc during an apply
only requests the banner's Cancel (stop after the current item), never anything else.

**7. Colours.** Owner's palette unchanged; Console derives text-safe "inks" from it:
- the accent and the semantic colours, as text, are moved toward the theme's body text only as far as 4.5:1 needs,
  checked against both row shades (they keep their hue). The raw tokens miss 4.5:1 as text in several themes:
  Windows blue on dark is 3.8:1, light/boring warn 3.4-3.7:1, light good 3.75:1;
- boundaries (grid frame, header rule, pane frames, button edges) use a "rule" colour of at least 3:1: the theme's
  `border` token is 1.1-1.6:1 in every theme;
- the selection is inverse video: the accent with white text, as Windows draws on its accent. Windows blue gives
  white 4.50:1, so the fill is darkened about 3% (0078D7 to 0074D0, 4.76:1). Black text only where white is under 4:1
  (the Dark and Fun fallback accents). When the grid isn't focused the selection is an accent tint with an accent
  bar at its left edge;
- the focus rect on the selected row is drawn in the selection's own text colour (core draws it in the accent on an
  accent fill, where it vanishes: U2);
- high contrast: every derived colour falls back to the system colours, as `Pal` already does.

**8. Banners.** One full-width line each (up to three wrapped), a solid INFO/WARN/FAIL tag in the tone colour, text
in body colour on a 10% tint, action buttons on the right. Same `Banners()` data as Baseline.

**9. Header and status line.** One 20 px line: title left, source facts right (fixture/this PC, build, counts, KB,
data folder). Refresh and Undo last apply moved from header buttons to F5 and Ctrl+Z. In Console the host status line
shows the selected item's name, image and account as you move (as diskmap's Console does); action messages still
replace it.

**10. Help (F1).** Replaces the detail pane with the key map and a column legend (State, Start's `+t` and `*`,
vs 1st, Hadron, Basis), plus "English only; no screen-reader support in v1" (CLAUDE.md asks the UI to say so).

## F-key map

| Key | List mode | Plan mode | Item bar (after F4) |
|---|---|---|---|
| F1 | Keys / help (toggle) | Keys / help | Auto |
| F2 | Services / Drivers tab | | Auto (delayed) (services only) |
| F3 | Filter (selects its text); typing anywhere also filters | | Manual |
| F4 | Item bar for the selected row | | Disabled |
| F5 | Refresh (live data only) | | Start (services) |
| F6 | TrustedInstaller toggle (session only) | TI toggle; rebuilds the plan | Stop |
| F7 | Plan: Default, first snapshot | | Restart |
| F8 | Plan: Default, Windows stock | | |
| F9 | Plan: Pure bloat removal | | |
| F10 | Plan: Stripped | | |
| F11 | Plan: Minimum | | |
| Ctrl+Z | Plan: undo the last apply (same enable rule as Baseline's button) | | |
| Space | | Tick / untick the row | |
| Esc | Clear filter, then focus back to the list (shared) | Cancel the plan; during an apply: stop after the current item | Back |
| Enter | (nothing) | nothing: no key applies | |
| Up/Down/PgUp/PgDn/Home/End | move | move | move |
| Tab | list, filter, detail (shared) | list, detail (shared) | |

F12 is not used: Windows reserves it for a debugger break. F10 needed the `WM_SYSKEYDOWN` line in `OnView`.

## Column set

Services: **Name** (22 ch) | **State** (9) | **Start** (14) | **vs 1st** (8) | **Hadron** (10) | **Basis** (10) |
**Account** (15) | **Display name** (flex, 45%) | **Image path** (flex). Drivers: the same with **Type** (7: kernel/fs)
and **Load group** (15) in place of Account.

- Name first: it's the ID you type in `sc`, search for, and see in logs.
- State in words, coloured (running = good, pending = warn, stopped = muted).
- Start compact (`auto`, `auto-delay`, `manual`, `disabled`, `boot`, `system`), `+t` for trigger start, ` *` (warn) for
  drift since Hadron set it.
- vs 1st and Hadron are what this page is for: what changed since first run and what Hadron would do.
  "protected" is in the bad colour, "no preset" and "—" muted.
- Basis on every row (rule 7: the UI shows the basis); "folklore" in the warn colour since it means no evidence.
- Account (services) or Load group (drivers): what the row runs as or loads with; "NT AUTHORITY\" is dropped.
- Image path, which the Analyze stage noted was missing from the list: a shared svchost shows as `svchost -k group`
  (the group is the information; the path is always svchost.exe), anything else as its full path with the middle
  elided so the file name stays. Display name and image share the spare width.
- Dropped when narrow, in order: Account/Type/Load group, Basis, vs 1st, Image path. Not Company: it's a claim from
  the file, in the detail pane where it says so.

Plan grid: `[x]` (5) | Name (22) | Now (12) | Planned `→ x` (13) | Basis (10) | Why / protection (flex; blocked in
bad, warnings in warn, reason muted). Control plans: `[x]` | Name | Op | Why.

## Contrast checked (WCAG 2 ratios, computed from the theme tokens and the module's derivation)

Body text against the worse of the two row shades; "rule" against panel and page; selection with accent 0078D7.

| Theme | text1 | text2 | accent ink | warn ink | bad ink | good ink | rule (3:1) | selection | F-bar key |
|---|---|---|---|---|---|---|---|---|---|
| Light | 15.95 | 5.70 | 4.82 | 4.70 | 4.67 | 4.72 | 3.17 | 4.76 | 4.56 |
| Dark | 13.80 | 6.50 | 4.77 | 8.91 | 5.59 | 8.09 | 3.04 | 4.76 | 4.51 |
| Fun | 12.94 | 7.25 | 4.68 | 9.15 | 5.18 | 8.82 | 3.13 | 4.76 | 4.65 |
| Boring | 10.13 | 4.94 | 4.66 | 4.58 | 4.72 | 4.51 | 3.16 | 4.76 | 4.51 |

Other accents: E81123 (Windows red) gives accent inks 4.53-4.66 and a white-on-red selection at 4.63 in every theme;
the fallback accents (808080 is achromatic) give 4.58-6.26 and selections at 4.55-7.97 (black text on the Dark and
Fun fallbacks). Raw tokens for comparison: `border` 1.1-1.6:1 everywhere; accent 0078D7 as text 3.8 (dark), 3.6
(fun), 3.9 (boring); light warn 3.73, boring warn 3.38, light good 3.75; text3 (faint) 2.4-4.6, so Console uses it
only for disabled controls (WCAG exempts inactive components). Body text in the doc panes is text1/text2. The
F-bar's body text is text1 on panel2 (9.2-15:1).

Not measured: core's scrollbar thumb (panel3 on panel2, about 1.2:1) and the search box, which core draws.

## Screenshots

All in `/mnt/project-files/pipeline/svcdrv/design/console/`, rendered with
`xvfb-run -a -s "-screen 0 1600x1000x24 -dpi 96" wine ... --screenshot`, `--fixture tests/fixtures/services`:

| File | Arguments |
|---|---|
| console-light.png, console-dark.png, console-fun.png, console-boring.png | `--layout console --select DiagTrack --accent 0078D7 --theme <t>` |
| console-plan-stripped.png / console-plan-stripped-light.png | `... --theme dark/light --plan stripped` |
| console-drivers.png | `... --tab drivers --select nvlddmkm` |
| console-suite.png | `Hadron.exe --module services --layout console ...` |
| console-accent-808080.png | achromatic: Dark's fallback accent |
| console-accent-E81123.png | Windows red, light theme |
| console-help.png | `--keys F1` |
| console-itemkeys.png | `--keys F4` (boring) |
| console-f9-bloat.png | `--keys F9` (fun): the plan an F-key opens |
| console-interrupted.png | `--fixture .../interrupted`: WARN banners with actions |
| console-control.png | `--ti on --plan stripped --select dmwappushservice` |

Wine has no Consolas. Under the default prefix it falls back to a proportional face, which misrepresents Console,
so these renders use a copy of the Wine prefix with `HKCU\Software\Wine\Fonts\Replacements\Consolas = "DejaVu Sans
Mono"`. DejaVu Sans Mono is about 9% wider than Consolas, so on Windows every column has a little more room than
shown. `tests/tools/services_check.sh` now renders five Console shots too (with Wine's default font).

## Core changes I'd want (not made; core is out of scope)
1. `ListView`: a row-height option (Console 18 px), a colour per cell (or a cell-colour callback), a per-column
   path-ellipsis flag, and centring on first select. Then the module's grid can go.
2. `Theme`: text-safe tokens (`accentText`, `warnText`, `badText`, `goodText`) and a `rule` token of at least 3:1, or
   make `border` 3:1. Today each module would have to derive them as this one does.
3. Selection: core's `selText` picks by Rec. 601 luminance > 140, not by contrast; with some accents that is under
   4.5:1. Use the contrast check, and draw the focus rect in the selection's text colour (U2).
4. `ScrollBar::Paint` uses theme colours directly, not the high-contrast fallback, and its thumb is about 1.2:1.
5. `Fonts`: a Consolas semibold at the mono size, for grid headers (Console uses `semi`, 15 px, against 14 px cells).
6. A shared F-key bar widget, if bios and diskmap get F-keys in Console too (they have none today), so the bar
   looks and behaves the same across modules.
7. A host-level `--keys` for every module's screenshots (here it is module-local and fixture-only), and a GUI-level
   selftest hook: the services selftest has no window, so the F-keys are checked by the replayed screenshots, not by
   assertions.

## Shared defects seen in Console (not fixed here; Baseline designer's scope)
- U1: after a tab switch (F2) `selName_` is cleared but `list_` keeps its index, so the grid shows a highlighted row
  while the detail pane says "Select an item". Console shows it the same way Baseline does.
- The saved sort is a visible-column index (`sortCol_` in prefs), and Console's column set differs from Baseline's,
  so a saved sort lands on another column after switching layout.
- The Hadron column shows the lowest preset level, not the hardware-adjusted outcome.
- The Stop-TI plan's "Why" text is just "stop" (seen in the control plan).

## Not done
- High contrast wasn't rendered (Wine can't switch to a high-contrast scheme here); the code falls back to the
  system colours through `Pal`.
- 125% and 150% DPI weren't rendered; every metric goes through `Sc()`.
- Banner actions (Undo, Resume, Dismiss, Stop it) are mouse-only in Console, as in Baseline; Esc covers Cancel
  during an apply. A key for them belongs with the shared Tab-reach work.
- Real Consolas metrics weren't checked; see the font note above.

## User-facing text added (for kb-auditor; none contains a number or a new factual claim)
- Help legend: "running, stopped, starting, stopping, paused"; "+t trigger-start; * changed since Hadron set it";
  "changed or new against the first-run snapshot"; "protected, no preset, or the lowest preset that changes it";
  "published, measured, reasoning or folklore (folklore: no evidence)"; "English only; no screen-reader support in v1."
- Help keys: "TrustedInstaller toggle, this session only"; "Every key changes the view or opens a plan; no key
  applies one: Apply is a click on the plan."
- F-bar: "No key applies: click Apply"; "Stop after this item"; detail strip "F4: keys for these; each opens a plan".
- Plan summary wording "click Apply" / "click Run" for "press" (Baseline keeps its own), and "Space or a click on [x]
  ticks."
- Reused unchanged: the TI scope note ("Only for writes Windows refuses to administrators. Not Defender, PPL or
  Tamper Protection.").
