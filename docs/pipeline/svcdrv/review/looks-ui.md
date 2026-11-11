# svcdrv Review: Looks / UI developer

Branch `module/services-svcdrv` at b7cc215. I only read files; nothing was changed. Scope: `modules/services/module.cpp` UI, `git diff origin/main...HEAD -- core/`
(widgets.cpp, cmdline/host `--accent`), core/ui headers, ARCHITECTURE "core/ui", 5-design.md, design/baseline.md, design/console.md,
and the final screenshots (baseline-suite-dark, baseline-plan-stripped-dark, baseline-boring, console-suite-dark, console-light).

Widths are in **logical px**, meaning 96-DPI units. At any DPI, the host's default window is `S(1200) x S(800)`, so the suite's
default content width is about 996 logical and the standalone's is about 1184. Thresholds come from measurements on the
screenshots, which use Wine's substitute fonts. Real Segoe UI and Consolas are slightly narrower, so treat each threshold as
±5%. The host has no `WM_GETMINMAXINFO`, so the module can be any size, including a half-screen snap. At 1920 px and 125%, a
half-screen snap gives a window of about 768 logical px.

Severity: none of these is category 1 or 2. The highest is category 5, with one category-4 item.

---

## F1. Baseline: the plan list toggles the wrong row on a click below the last row (cat 5; could be argued as 2)
`modules/services/module.cpp:1396-1402`. In plan mode, a left click anywhere in the first 52 units and below the header calls
`TogglePlanRow(lv.Selected())`. It does this even when `lv.OnMouse` hit no row (a click in the empty area under a short plan), so
it ticks or unticks the row that was **already selected**.
- Scenario: any theme, Baseline, a plan with 5 rows (for example "Default: first snapshot" with few drifts). Select row 2 and
  click in the [x] column well below row 5. Row 2 flips from [x] to [ ], and Apply's count changes.
- Also: `WM_LBUTTONDBLCLK` is mapped to `WM_LBUTTONDOWN` (line 1288), so a double-click in Baseline toggles twice, a net no-op.
  Console toggles only on DOWN (`:3337`), so a double-click there toggles once. The two layouts disagree.
- Fix: toggle only when the click hit a row (`idx = (y - bodyTop + offset)/rowH < RowCount()`), and only on the first click.

## F2. Baseline: the preset chips run under the filter box at moderate widths (cat 5)
`module.cpp:2096-2110`. The chips are laid out with no width check. `rcSearch_` is then forced to at least `Sc(140)` from the
right edge, whatever `x` is.
- Scenario: Baseline, suite, all themes, module content below about 830 logical px (the suite window below about 1035 logical:
  a half-screen snap at 100% on a 1920 px screen gives 772 content). "Minimum" and then "Stripped" are drawn under the search
  box. The click on the search rect wins (`OnInput:1281`), so **the Minimum preset can't be clicked**, and the hover cursor
  shows a hand over the search box. Standalone, this happens below about 850 logical window width.
- Fix: wrap the chip row (move the filter to its own line), or shorten the labels ("First snapshot", "Windows stock") below a
  threshold.

## F3. Baseline: the detail action strip overflows the pane at or just under the default suite size (cat 5)
`module.cpp:2148-2169`. The `Start type: Auto | Auto (delayed) | Manual | Disabled` row needs about 372 logical px and has no
wrap or clip. The pane is `0.4*(content-40)-12` wide.
- Scenario: Baseline, suite, default window (996 content), giving a 370 px pane: "Disabled" touches or crosses the rounded
  border. At a 960 logical window (half-snap, suite) the pane is 286 px, so "Disabled" and half of "Manual" are drawn past the
  pane and the right page padding. At 150% with a 1280 px window (853 logical), the click targets sit off the window. The
  baseline-suite-dark.png render (1092 content) has only about 38 px of room.
- Fix: wrap to a second line when `ax` would pass `det.right - Sc(14)`, or let the label column shrink ("Start type" moves above
  the buttons when narrow).

## F4. Baseline: columns overflow the list, and the always-shown Hadron column hides under the detail pane (cat 5)
`module.cpp:1914-1928`, with `core/ui/widgets.cpp` ListView header paint. After every column with a drop rank is gone, fixed
(412) + flex minimum (100) + 12 = 524 logical. Below that the columns run past `rcList_.right`. Body rows are clipped, but the
header text is not clipped horizontally. The detail pane is painted afterwards and covers it.
- Scenario: Baseline, content below about 913 logical (listW = 0.6*(content-40) < 524). That is the suite at any window below
  about 1117 logical, which includes the half-snap. "Hadron" (drop 0, "never dropped") is partly or fully hidden, along with
  its sort arrow, and a header click in that area can't reach it.
- Fix: shrink the fixed widths proportionally once all drop ranks are used, or let Display name go below `kFlexMin`. Clipping
  the header to `rc` in core would also stop the header from painting outside the list (that change affects DiskMap and RunAs
  too, but only fixes a bug there).

## F5. Primary buttons: hover drops white text below 4.5:1 (cat 5)
`module.cpp:1445`, `:1585` (Baseline `Button` and banner actions): the hover fill is `Mix(acc, white, 0.18)`, but the text is
still `OnColor(acc)`, which was chosen for the un-hovered fill. `module.cpp:2383` (Console `ConButton` primary): the fill is
`Mix(selFill, selInk, 0.12)`, which moves toward the ink colour.
- Scenario: Light, Dark, Fun or Boring with Windows accent 0078D7 (the design's reference). Hover "Apply 28 changes", or a
  banner's "Undo"/"Resume": the fill becomes #2E90DE and white on it is **3.4:1**. In Console, white on 0074D0 mixed 12% toward
  white is about 3.9:1. The red accent E81123 behaves the same way. The design's contrast tables check only the resting state.
- Fix: hover toward the side away from the ink (darken when the ink is white), or recompute `OnColor(hotFill)`.

## F6. Baseline: text that matters is cut silently (cat 5)
- `module.cpp:2217`: the plan summary is one `TextLeft` line with an end ellipsis. The reason Apply is off (`"  (" + why + ")"`)
  sits at the **end**, so it is the first part to go. Scenario: Baseline, suite at 1092 content or the default window, live,
  not elevated or read-only. The line ends "...Click [x] or press Space to tick or unt…" and the reason Apply is greyed out is
  never shown. At 1280 it fits only just (baseline-plan-stripped-dark.png). Fix: put the reason first, or on its own line, or
  wrap it.
- `module.cpp:1569-1578`: banners are capped at `Sc(16)*4`. `DT_END_ELLIPSIS` with `DT_WORDBREAK` does not mark a dropped line,
  so line 5 and later disappear with no "…". The apply-result banner adds the `fail…`/`skip…` lines at the end, which is the
  part lost. Scenario: suite width, an apply with 3 or more failures plus a zip path. Console's version (`WrapLines` plus an
  explicit "…", `:2517`) is correct; use it in Baseline too.
- `module.cpp:2219-2227`: plan notes are capped at `Sc(32)` (2 lines) in the same way: HwNote or the build-mismatch note loses
  its third line silently.
- `Sc(16)` and `Sc(32)` stand in for the line height instead of `tmHeight`. Where the real height is larger (another fallback
  font, or rounding at 175%) the last line is cut through the middle. Use `GetTextMetrics`, as `WrapLines` does.

## F7. Console: the F-key bar has no fallback below short labels (cat 5)
`module.cpp:2454-2501`. There are two tiers, full and short. When even the short labels don't fit (about 700 logical for the
12 list-mode cells, plus the tag and padding, so module width below about 760 logical), the cells go on past the "LIST" tag and
the right edge. `AddClick` registers targets outside the window.
- Scenario: Console, suite, 125% on 1920 px with a half-screen snap (content about 580 logical): F10, F11 and ^Z are drawn
  under or past "LIST". The ^Z cell is off-screen, so it can't be clicked and its state can't be seen. The keys themselves
  still work.
- Fix: a third tier with key captions only ("F7 F8 …", with the labels in help), or stop at `tr.left` and drop the cells that
  don't fit (lowest-priority first: ^Z, F11 … only if help lists them), or wrap to two rows.
- Related, at the same place: the cells reach `r.bottom == rc.bottom`, so the 2 px focus ring from `BuildTabStops`
  (`:2053-2054`, drawn `g+1` px outside) loses its bottom edge to the window clip. The ring is visible on 3 sides only, in
  every theme. Inset the cell by `Sc(3)` at the bottom, or draw the ring inside for the bar.

## F8. Console: the detail split depends on heading text, and keyboard reach of the left column (cat 5)
`module.cpp:2353-2370` (`SplitDoc`).
- **Fragile key:** a heading that *starts with* "Configuration" or "From the service" goes left, and everything else goes right.
  KB headings are `e->title + " (Hadron's KB)"` (`:1751`, `:1867`). A future KB entry titled "Configuration Manager …" or
  "From the service host …" would put the whole KB block in the **left** column, with no error. Renaming "Configuration" (for
  example a copy edit to "Settings") puts the configuration on the right, after the KB text. No selftest covers this. No
  current KB title matches (checked in `modules/services/data/services.db`). Fix: add a `side` field to `Para`, set where
  `ItemDoc`/`PlanRowDoc` push the heading, and have `SplitDoc` read it. Add a selftest asserting that the left column of
  DiagTrack starts with Configuration.
- **When `right` is empty** (`left->swap(*right)` at :2369, or a document with no KB part): `rcDetail_ = {}` (`:2913`). The detail
  tab stop then disappears (`BuildTabStops:2036`) and no key can scroll the detail at all. Even in the normal two-column case,
  only the right column (`detSb_`) has keyboard scrolling. The left column, which holds Configuration (the longer one;
  DiagTrack's needs a scrollbar in console-light.png), scrolls only with the wheel or a drag, and `conLeftRc_` is never a tab
  stop. CLAUDE.md requires keyboard navigation. Fix: make the left column a tab stop (for example `ID_FOCUS_DETAIL_L`) and route
  Up/Down/PgUp/PgDn to `conLeftSb_` while it has focus.
- **Dead colour remap:** `SplitDoc` maps `acc`/`warn`/`bad`/`good` to the Console inks. Since the Baseline merge, `ItemDoc` and
  `PlanRowDoc` emit the `Readable()` forms (`badT`, `warnT`), so the remap fires only where `Readable` returned the colour
  unchanged (Dark warn and good). Console documents therefore use Baseline's text forms. Contrast is still at least 4.5:1 on
  the panel, so nothing is unreadable, but the comment at :2350-2352 is wrong. Map `*T` to the inks as well, or drop the remap
  and its comment.

## F9. Console: overlaps at narrow or short sizes (cat 5)
- `module.cpp:2690-2700`: when the sorted column is pushed fully past `body.right`, `right = body.right` and
  `ax = right - gapCh - tri`, so the sort triangle is drawn over **another** column's header. Scenario: Console, sorted by
  Display name or Image path, grid width below about 560 logical. Fix: skip the triangle when `x >= body.right - tri`.
- `module.cpp:2775-2776`: the search box is at least `Sc(160)` and anchored right with no check against `x`, so it covers the
  TrustedInstaller chip below about 580 logical (the TI chip's click target sits underneath; the search box wins the click).
- `module.cpp:2867-2888`: the action strip (start buttons, now buttons, hint) is one row with no wrap. Below about 520 logical
  it runs past the pane frame.
- `module.cpp:2797-2801` (and plan, `:3108-3112`): with `avail < ~200` logical, `detH = max(Sc(80), avail-Sc(120))` makes
  `grid.bottom < grid.top`. The grid header is still painted at `grid.top` (height 21), and the detail pane starts *above* it,
  overlapping the header and the filter row. Scenario: Console, 3 banners (fixture or interrupted, TI-left, drift), and a
  module height below about 360 logical. That is a quarter-screen snap at 200% on a 1080p screen, or a short window. Fix: clamp
  the heights (`grid.bottom >= grid.top + headH + rowH`), and drop the detail pane before the grid when there isn't room.

## F10. Baseline: inverted pane rects when banners are tall (cat 5)
`module.cpp:2114-2118`, `:2231-2241`. `bottom = rc.bottom - pad` is never clamped to `y`. With 3 or more tall banners (each can
be 4 lines, up to about 76 px) in a short window, `det` and `rcList_` are inverted. `RoundRect` normalises inverted rectangles,
so `RoundFill(det)` paints the detail panel **upwards over the presets row and the banners**, and the detail buttons are drawn
over the banners. Scenario: Baseline, interrupted-apply fixture, plus drift and an apply result, module height below about 420
logical. Fix: `bottom = std::max(bottom, y + Sc(120))`, and skip the panes when there's no room.

## F11. DPI contract gaps (cat 4)
- No render at more than 96 DPI exists: `Host::Screenshot` forces `dpi_ = 96` and 1280x800 (`core/host/host.cpp:317-318`). Every
  finding above that depends on width is untested in CI. Suggest `--dpi` and `--size WxH` for `--screenshot` (fixture only) and
  4 extra renders: 150% at 1280x800, and 960x800 at 96 in both layouts.
- After a monitor change, the scroll offsets in pixels are not rescaled. `ConGrid::sb` (module) and `ListView::sb_` (core) keep
  their old pixel offset. `Ensure` runs only when `sel != lastSel` (`:2671-2676`), so going from 96 to 144 DPI leaves the
  selected row off-screen until the selection moves. Fix: in `OnDpiChanged`, set `conList_.lastSel = conPlan_.lastSel = -2`
  (re-centre) and call `RevealSelection(false)` for Baseline.
- `module.cpp:2967`: Console help uses `lh = Sc(17)`, but `f.semi` is Consolas 15 px, whose `tmHeight` is about 18 (27 at 150%).
  The key captions lose about 1 px of descenders. Use `tmHeight`.
- `module.cpp:2923`: `InflateRect(&fr, -2, -2)` is not scaled. `core/ui/draw.h` `FocusRectC` is a 1 px dotted pen at every DPI,
  so at 200% the list and detail focus rects are hairlines. That is a core follow-up, but the widgets.cpp U2 change is where it
  shows.
- `module.cpp:2731`, `:2792`, `:2924`: Console tests `GetFocus() == hwnd_` where Baseline uses `HasFocus()`. As a result,
  `--focus list|search|detail` renders do **not** show Console's list, search or detail focus (only the 2 px Control ring), so
  the design's "checked with `--focus`" didn't actually exercise those indicators in Console. Use `HasFocus()`.
- Good: the module builds no fonts. Everything goes through `Sc()` with `host_->Dpi()`, read at paint time. Console's 18 px row
  **is** scaled (`g.rowH = Sc(18)` on every paint; the unscaled default of 18 in `ConGrid` is used only before the first
  paint). The list widths go through 96-DPI units (`SetListColumns`, `MulDiv`), and Console's widths come from the measured
  Consolas `"0"`.

## F12. High contrast (cat 5)
- Several colours are mixed in HC instead of taken from the system: disabled controls (`Mix(fill, bg, .5)`, `Mix(tc, fill, .5)`
  at `:1430-1433`, `:1448-1450`, `:1465-1467`), banner fills (`Mix(panel, tone, .16)` at `:1574`), primary hover (F5), and
  Console's unfocused selection (`Mix(panel, acc, .22)` at `:2723`). Use `COLOR_GRAYTEXT` for disabled items and
  `COLOR_HIGHLIGHT`/`COLOR_BTNFACE` for state in HC.
- `Pal` in HC puts `WINDOWTEXT` text on `BTNFACE` chips (`:120-122`). The system pair is `BTNTEXT`. They match in the stock HC
  schemes but not in custom ones. In HC, hover feedback also disappears (`chip == chipHot`).
- In HC the semantic text maps to `COLOR_HOTLIGHT` in Baseline but to `WINDOWTEXT` in Console (`CPal`, `:2296`). Pick one; a
  "protected" row reads differently depending on the layout.
- Core (pre-existing, outside this diff): the host never re-resolves the theme on `WM_SETTINGCHANGE` or `WM_SYSCOLORCHANGE`, so
  turning HC on while Hadron runs has no effect until a restart.

## F13. Three implementations of "text on a fill" (cat 5)
- WCAG helpers are duplicated: `Lin` (sRGB threshold 0.04045, `:82`) and `ChanLin` (0.03928, `:2266`), with
  `RelLum`/`Contrast` declared twice (the outer and the nested anonymous namespace; it compiles only because no outer caller
  follows the inner declaration). Core `widgets.cpp` `selText()` is a third copy.
- There are three different rules: core ListView (white if at least 4.45, otherwise the better of the two), module `OnColor`
  (the same), and Console `InverseVideo` (darkens the fill until 4.5). For the same accent 0078D7, a Baseline selected row
  measures 4.50:1 and Console's 4.76:1. That's fine, but it is the same decision coded three ways.
- `4.45` admits accents whose white contrast is 4.45 to 4.49, which fails AA by the letter. The design says "to one decimal";
  state it as a decision or use 4.5.
- The suite outside this diff: BIOS still uses Rec. 601 (`modules/bios/module.cpp:81`). With a bright green Windows accent
  (10C010), BIOS chips draw near-white text at about 2.3:1, while Services draws black at 8.6:1. The suite disagrees with
  itself. Suggest `ui::OnFill(COLORREF)` in core, used by widgets, Services and BIOS (a lead/core follow-up).

## F14. core/ui/widgets.cpp diff and its effect on DiskMap, BIOS and RunAs (cat 5, mostly OK)
- **Selection text by WCAG:** correct. The high-contrast path is unchanged (`COLOR_HIGHLIGHTTEXT`). On the shipped fallback
  accents the result is the same as before. Only DiskMap and RunAs (and the UIKit gallery) use `ListView`. BIOS has its own
  list, which matches the claim of 0 changed pixels.
- **Sort arrow slot:** correct for left-aligned and right-aligned columns. `S(8)+S(6)` equals `S(14)` at 125% and 150%. DiskMap's
  narrow right-aligned "%" and "Size" columns still fit their titles. If a column were narrower than `2*pad + 8` px, the arrow
  would start left of the column (`x + w - pad - S(8) < x`). No current caller has such a column.
- **U2 focus rect:** visible now (selText on selBg), inset `S(3)`. It still uses a 1 px dotted pen (see F11). It still draws
  nothing when the list has focus but no selection (empty filter result): Tab lands on the list with no visible focus. This
  affects the services filter "Nothing matches", DiskMap and RunAs. Suggest a focus rect around the body when `sel_ < 0`.
- **Header text is not clipped horizontally** (pre-existing). This is the cause of F4.
- Outside this diff, FYI: `modules/runas/module.cpp:279` passes `Sc(360)` etc. to `ListView::SetColumns`, which scales again, so
  the widths are doubly scaled at more than 96 DPI.

## F15. GDI, threading and double buffering (cat 3): no leaks found
- Every pen and brush in the module is freed (`:2051-2057`, `:2697-2700`), and `draw.h` helpers free their own. The
  double-buffered `WM_PAINT` cleans up on both branches (`:427-447`). `WM_ERASEBKGND` returns 1, and `WM_SIZE` invalidates
  without erasing. No paint path opens a dialog or touches a worker.
- `ui::DrawText` and `PaintBanners:1567` / `PaintPlanMode:2221` leave a host font selected in the DC. On a memory DC this is
  harmless. On a `WM_PRINTCLIENT` DC it leaves state behind; restore the old font.
- Core (pre-existing): the frame window is created without `WS_CLIPCHILDREN` (`core/host/host.cpp:444`), and `PaintChrome` fills
  the whole client area. During a resize the frame paints `bg` over the module before the module repaints, so the content
  flickers. Add `WS_CLIPCHILDREN` to the frame.
- Cost (low): `Pal`/`CPal` are rebuilt for each chip, button, cell row and banner on every paint, and each runs several
  `Readable`/`Ink` loops with `pow`. Inside the paint, `SetListColumns`/`ConsoleListColumns` call `Refilter()`, which writes
  Prefs. Cache the palette for each theme, and move the Prefs write out of the paint path.

## F16. Small items (cat 5, low)
- Focus ring shape: Baseline uses a pill (`rad = h + 2g`, `:2053`) around buttons whose corner radius is 8 to 10, so the ring
  doesn't follow the button. In Console it uses `Sc(6)` around square `ConButton`s. Use the control's radius plus `g`.
- `:2731`: Console's grid focus rect is drawn at `row.left + 1`, with no inset on 18 px rows. It sits on the row edges, and on
  the first and last visible rows it's half-clipped by the body clip.
- `:380`: merge artefact. The comment on `centrePending_` carries another member's comment ("--focus <n>: …"). `pendingStop_`
  is the member that comment describes.
- Baseline-only functions keep `con ? …` branches (`PaintPage` after the Console early return, `PaintListMode`,
  `PaintPlanMode`) that are now dead code.
- After centring (`RevealSelection(true)`) the top row can be half-clipped (baseline-boring.png, "CaptureService"). The offset
  isn't aligned to a row.

## Checked and fine
- Theme fit in all four themes in the final renders: tokens only, no literal colours except white and black for ink and the
  mix target. The accent, semantic and faint text forms are at least 4.5:1, and the focus ring is text1, at least 10:1, in
  every theme. The ring is outside the control, so its contrast doesn't depend on the fill.
- `--accent` (cmdline and host) applies only to the screenshot path (`deterministic && hasAccentOverride_`), and an achromatic
  value takes the fallback, as on live.
- Console's selection is inverse video at 4.76:1 with the focus rect in its own ink (U2 fixed locally). The grid scrolls with
  the wheel and a draggable scrollbar, sorts by header click, and Up/Down/PgUp/PgDn/Home/End all work. On Enter, a focused
  F-key cell runs `OnConsoleAction`; Apply is never a tab stop (`BuildTabStops` filters out `ID_APPLY`, and the activation path
  re-checks).
- `WrapLines` never overflows. Doc panes are clipped with `IntersectClipRect` (in logical coordinates, which is correct under
  the suite's viewport origin).
