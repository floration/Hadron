# svcdrv: Design (stage 5)

Stage 5 (Design), 2026-11-11, on `module/services-svcdrv`, starting from Analyze's hand-off at 785074c. Two designers ran
in parallel, each on its own branch, and the lead merged both:

| Designer | Branch (final) | Write-up |
|---|---|---|
| Baseline | `design/svcdrv-baseline` (573d251) | `design/baseline.md` |
| Console | `design/svcdrv-console` (ca1b2da) | `design/console.md` |

The two write-ups hold every choice with its reason, the contrast numbers per theme, the column plans, the keyboard
maps and the core changes each designer would want. This file is the summary and the lead's merge notes.

## Checks on the merged branch
- MinGW Release with `-DHADRON_SERVICES_TEST_HOOKS=ON`: no new warnings (the existing `core/host/host.cpp` ones remain).
- `tests/tools/services_check.sh`: genkb and fixture byte-identical; selftest **189/189** standalone and `--solo services`;
  the 8 plan goldens and the delayedlinger golden **unchanged**; every screenshot renders.
- Presets, guards, plan building and apply (`plan.cpp`, `store.cpp`, `recovery.cpp`, `ti.cpp`, backends, KB) are
  untouched by Design.
- No key applies a plan in either layout: Apply/Run is never a tab stop, the activation path refuses it, and Console
  has no Apply key. Fixture mode still never applies.

## The eight screenshots
All at 1280x800, 96 DPI, fixture data, DiagTrack selected, `--accent 0078D7` (Windows' default blue) standing in for
the Windows accent. Shared files `pipeline/svcdrv/design/final/`:

| Theme | Baseline | Console |
|---|---|---|
| Light | `baseline-light.png` | `console-light.png` |
| Dark | `baseline-dark.png` | `console-dark.png` |
| Fun | `baseline-fun.png` | `console-fun.png` |
| Boring | `baseline-boring.png` | `console-boring.png` |

Extras in the same folder: `*-light-accent-808080.png` (achromatic, so the theme's fallback accent),
`*-light-accent-E81123.png` (another accent), `*-suite-dark.png` (inside `Hadron.exe`), `*-plan-stripped-dark.png`.
Each designer's full set (focus states, sort, drivers, interrupted, help page, before shots) is in
`pipeline/svcdrv/design/baseline/` and `pipeline/svcdrv/design/console/`. Wine has no Segoe UI or Consolas, so the
fonts are substitutes; Console's own set used DejaVu Sans Mono, about 9% wider than Consolas.

## What changed
**Core (lead, before the designers):** `--accent RRGGBB` for `--screenshot` (6e4df2b) so accent-following can be shown
deterministically; an achromatic value takes the theme's fallback, as the live path does.

**Shared (Baseline designer, both layouts):**
- Contrast: text forms of the accent, warn, bad, good and text3, derived per theme and moved toward black or white
  only until 4.5:1 on bg, panel and panel2. Fills and the Windows accent are unchanged; semantic colours stay fixed per
  theme. Text on an accent fill is white while it reaches 4.5:1, otherwise black.
- `core/ui/widgets.cpp`, no API change: U2 focus rect now visible on selected rows; the sort arrow has its own slot
  (no overlap); selection text chosen by WCAG contrast instead of Rec. 601 luminance.
- U1 fixed (tab switch selects and shows the first row). The selection stays on screen after a sort.
- Sort saved by column key (`sortKey` pref), not visible index.
- The Hadron column runs the same per-item tests as `BuildPreset` (guards, driver `sysok`, hardware evidence), so it
  matches the detail pane (BluetoothUserService now says "kept"), and sorts by level: bloat, stripped, minimum, kept,
  no preset, protected, not in the KB.
- Column priority reworked: Company now shows at 1280x800 and in the suite; Basis, Account and Type drop first (the
  detail pane has them).
- Tab/Shift+Tab reach every chip, toggle and button with a 2px text1 ring; Enter/Space activate.
- Stop-TrustedInstaller plan has a real Why.
- "No screen-reader support in v1" is in the status line after every load.

**Console (Console designer):** a real second layout. Dense 18px grid (module-drawn on core's ScrollBar) with Name,
State, Start (`+t` trigger, `*` drift), vs 1st, Hadron, Basis, Account (Type and Load group on Drivers), Display name
and Image path; a two-column detail pane at the bottom (configuration left, KB right); an F-key bar (F1 keys, F2 tab,
F3 filter, F4 item keys, F5 refresh, F6 TI, F7-F11 the five presets as plans, Ctrl+Z undo-last as a plan); a help page;
inverse-video selection; 3:1 rule colour for frames. No key applies.

## Lead's merge decisions
1. **Dropped the Baseline designer's core font change (9209b03, reverted in e5ee164).** It made every Baseline
   ListView's first column 12px instead of 15px. It helps services fit, but it changes DiskMap and RunAs too, which
   breaks DiskMap's legacy parity checklist (names in `g_fUI`) and was outside the core fixes Design allowed. Cost:
   in the suite, Name and Display name are narrower (names cut at about 13 characters). If the owner or Review wants
   it, it goes in as its own core change for all modules.
2. **Console uses the shared fixes, not its own copies.** The Hadron column comes from `KbVerdict`, sorting goes
   through `SortKeyShown` (by key), and Console now builds tab stops so Tab and the focus ring work there too
   (checked with `--focus`). The load status line names F1 in Console instead of Tab.

## For Review
- The core `widgets.cpp` change touches every module: the Baseline designer rendered DiskMap, BIOS and RunAs before and
  after (BIOS 0 pixels changed; DiskMap Console's focus rect now shows). Re-check after the font revert.
- Console's tab order follows the click-target order (top-to-bottom, then left); verify it reads sensibly with the
  F-key bar, and that Enter on a focused F-key cell never reaches Apply.
- Console's detail split keys off the section heads "Configuration" and "From the service"; renaming them silently
  puts everything in the left column.
- `--focus`, `--sort`, `--keys` and `--plan ti-stop` are fixture-only render hooks; confirm none is reachable on live data.
- User-facing text changed, for kb-auditor (no numbers anywhere): the Stop-TI Why ("Hadron started it for an apply that
  did not finish, and nothing stopped it. It is a Manual (on-demand) service: Windows starts it again when it next
  installs updates or features."), the status-line keyboard and screen-reader note, the filter placeholder, the Hadron
  labels "kept" and "no preset", Company drops a trailing legal form in the list, and Console's help legend and
  "no key applies" strings (listed in `design/console.md`).

## Not done
- High contrast and DPI above 96 can't be rendered under Wine; both paths use system colours and `Sc()`.
- Keyboard traversal has no automated GUI test; it needs a hands-on pass on Windows.
- Core API proposals (both write-ups): `ListView::SetSort`, `Reveal`, per-cell colour, row-height option; text-safe
  tokens and a 3:1 rule token in `Theme`; ScrollBar high-contrast and thumb contrast; a shared F-key bar widget.
  These are lead/core follow-ups, not part of this idea.
- The owner-gap items from Analyze (empty Windows stock table, KB coverage, TI toggle vs WinDefend) are content and
  scope, not design; they stay open for Finalize and the Ideas follow-ups.

## Hand-off
To **Review** at the commit that adds this file.
