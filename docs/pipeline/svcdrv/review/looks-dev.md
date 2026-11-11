# svcdrv Review, Looks group: developer reviewer

Branch `module/services-svcdrv` @ b7cc215, built in a detached scratch worktree, so the repo was not touched.
Screenshots (94 files) are in `/mnt/project-files/pipeline/svcdrv/review/looks-dev/`.

## Environment
- MinGW-w64 (Ubuntu 24.04 apt), CMake + Ninja, `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DHADRON_SERVICES_TEST_HOOKS=ON`.
- Wine 9.0 (Ubuntu 9.0~repack-4build3), Xvfb at 96 DPI.
- My renders used their own WINEPREFIX with `z:` removed. `d:` held a copy of the fixtures and `e:` held the built exes.
- **Review harness, not on the branch.** `Host::Screenshot` hard-codes a 1280x800 client at 96 DPI, and there is no
  `--size` or `--dpi` flag. So the scratch copy reads the env vars `HX_SHOT_W`, `HX_SHOT_H` and `HX_SHOT_DPI`. The patch
  is saved as `review-harness-screenshot-size.patch` in the shots folder. Under this patch, DPI 120/144 means the
  host's `dpi_`: fonts and every `Sc()` scale, as they would after `WM_DPICHANGED`.

## Build and test
- Build: **OK**. 24 warnings, all in `core/host/host.cpp` and none in services:
  - `-Wcast-function-type` on the `GetProcAddress` casts at lines 44 and 416;
  - `-Wmissing-field-initializers` on `WNDCLASSEXW wc{sizeof(wc)}` at lines 425 and 477.
- `tests/tools/services_check.sh`: **PASS** (exit 0). It covers:
  - the genkb selftest, and `services.db` matching byte for byte (99 entries);
  - the fixture regenerating identically;
  - the selftest standalone and in the suite, each **189 checks, 0 failed**;
  - the 8 golden plans plus delayedlinger matching `tests/golden/services`;
  - all 17 CI screenshots written.
- That check run used a default prefix (with `z:`), the same way CI runs it. Fixture mode never applies changes.

## Matrix rendered
- **Sizes:** 1280x800, 1024x640, 1920x1080 and 760x1200 (tall-narrow), in both layouts.
- **Services list:** all four themes, standalone.
- **In dark:** the Drivers tab, the plan view (Stripped), and the suite (`Hadron.exe --module services`).
- **DPI:** 120 at 1600x1000, 144 at 1920x1200, and 144 at 1280x800. Each covers list dark, plan light and suite dark, in both layouts.
- **Other modules:** DiskMap, BIOS and RunAs at 1280x800 dark, both layouts. They were pixel-diffed against a build of
  6e4df2b, the commit before the core/ui ListView commits 667ca24..e5ee164.

At the CI size (1280x800, 96 DPI) every theme and layout is clean. Every finding below shows up only when the window is
smaller than about 1100 px of content width. You get that from the suite on a 1024-wide window, any window at 150% on a
1280-wide or 1920x1080 laptop panel, or narrowing the window. **The host sets no minimum window size**: there is no
`WM_GETMINMAXINFO` anywhere in `core/` or `modules/`. Legacy DiskMap used 900x600 and legacy BSX used 1000x640.

## Findings

| # | Cat | Screenshot(s) | What's wrong | Likely cause |
|---|---|---|---|---|
| 1 | 5 (arguably 2) | `suite-baseline-dark-1024x640.png`, `dpi144-svc-baseline-dark-1280x800.png`, `svc-baseline-light-760x1200.png`, `drv-baseline-dark-760x1200.png`, `suite-baseline-dark-760x1200.png` | The list overflows its pane. The **Hadron** column (whether a preset changes the item) is cut mid-word ("protecte", "no pres", "minimur"), or pushed out of view completely. At 760 the "H"/"S" of the next header bleeds into the gutter between the panes. Start type also disappears at 760 in the suite. | `SetListColumns` (module.cpp:1876). Once drop ranks 1–5 are gone, the drop-0 columns are always kept and the flex column still gets `kFlexMin`. Their total (140+72+116+84+100=512 at 96 DPI) exceeds the list. Fix: shrink the flex/Name columns below the minimum before overflowing, or give Hadron priority over Display name. The header leak at 760 suggests `ListView::Paint` in core/ui/widgets.cpp doesn't clip the header to `rc`. |
| 2 | 5 (arguably 2: the current start type isn't visible) | `suite-baseline-dark-1024x640.png` ("D" past the window edge), `dpi144-svc-baseline-dark-1280x800.png` ("Dis"), `svc-baseline-light-760x1200.png`, `suite-baseline-dark-760x1200.png` | In the detail pane's Start type strip (Auto / Auto (delayed) / Manual / **Disabled**), the buttons run past the pane and the window. The highlighted current value is the one that gets cut. Their click rects lie outside the pane. | `PaintListMode` (module.cpp ~2150–2160): the `ButtonL` x keeps growing with no wrap, clip or label shortening. Wrap to a second row, or use short labels (Auto / Delayed / Manual / Off) when `ax` would pass `det.right`. |
| 3 | 5 | `svc-baseline-light-760x1200.png`, `suite-baseline-dark-760x1200.png` | Preset chips are hidden under the filter box. At 760 standalone, **Minimum** is gone. In the suite at 760, "Pure bloat removal" is covered and "Stripped" bleeds past the window edge. The chips' click rects are still registered under the search box. | `PaintListMode` row 2 (module.cpp ~2098–2108): chips are laid out left to right, then the search box is forced to at least `Sc(140)` at the right edge with no collision check. Wrap the search box to its own row, or shrink it first. |
| 4 | 5 | `svc-console-light-760x1200.png` ("MiLIST"), `suite-console-dark-1024x640.png` ("^Z UndLIST"), `dpi144-suite-console-dark-1280x800.png` (F10/F11/^Z gone, "F10" drawn under "LIST") | The Console F-key bar overflows. Even with short labels, the keys run under the mode tag, and the last keys are clipped or unreachable by mouse. The keyboard still works. | `PaintFKeyBar` (module.cpp:2454) has only two levels, full and short. Add a third level (key caps only), or stop drawing keys at `tr.left`. |
| 5 | 4 | `dpi144-svc-baseline-dark-1280x800.png`, `dpi144-suite-console-dark-1280x800.png`, `dpi120-*` | DPI itself scales correctly: fonts, `Sc()`, columns via `MulDiv(...,96,dpi)`. But at 150% on a 1280-wide window (≈853 logical px), findings 1, 2 and 4 all appear, and there is no minimum size to stop it. The window opens at `S(1200)xS(800)` and can then be shrunk freely. | `core/host/host.cpp`: add `WM_GETMINMAXINFO` with a DPI-scaled minimum. Content width ≥ ~1000 logical px clears 1–4 in Baseline; the suite needs sidebar + that. This is a core change for the lead. Alternatively, make the module degrade (fixes 1–4). |
| 6 | 5 (minor) | `plan-baseline-dark-760x1200.png`, `plan-console-dark-760x1200.png` | The plan summary is a single ellipsized line. At 760 the "(Fixture data: applying is off.)" reason and the end of the tick instructions are cut. The "Why / protection" column is dropped entirely, so an unticked row's reason ("needs TrustedInstaller") shows only when the row is selected. | `PaintPlanMode` (module.cpp ~2210): `TextLeft` single line. Use `DT_WORDBREAK` as the notes below it do. `SetPlanColumns` gives fixed widths totalling 442 plus a 120 minimum for Why, so Why overflows first. Consider shrinking Name, or putting Why ahead of Planned when narrow. |
| 7 | 5 (minor) | `plan-baseline-dark-1024x640.png` vs `plan-console-dark-*` | When applying is off (fixture mode), Baseline's disabled "Apply 28 changes" still reads as a filled accent button: `Mix(acc,bg,0.5)` on dark is still clearly blue. Console's disabled Apply is visibly grey. The two layouts are inconsistent, and Baseline looks clickable. | `Button` (module.cpp:1438): for disabled primary buttons, use the `chip` fill plus faint text (as Console does), not a half-mixed accent. |
| 8 | 5 (cosmetic) | `svc-baseline-dark-1920x1080.png`, `svc-baseline-fun-1920x1080.png` | At 1920 the list still truncates Account ("NT AUTHORIT...") and Type ("Shared proc..."), because every non-flex column has a fixed width and only Display name grows. | `SetListColumns`: share the slack between Display name, Account and Type once all the columns fit. |
| 9 | 5 (cosmetic) | `suite-baseline-dark-760x1200.png` ("remova l") | When the detail key column is narrow, a key label ("Pure bloat removal") breaks inside a word. | `PaintDoc` key column (module.cpp:1645): `DT_WORDBREAK` falls back to character breaks when one word is wider than the column. Widen the key column to the longest word, or ellipsize. |
| 10 | info | `dm-crops.png` (ref above, new below), `other-diskmap-console-dark-1280x800.png` | Re-check of the other modules after the widgets.cpp changes. BIOS (both layouts), RunAs (both) and DiskMap Baseline are **pixel-identical** to 6e4df2b. The only difference is in DiskMap Console: the selected row now has a dashed focus rectangle, from 667ca24 "visible focus rect", and it is drawn even in the unfocused screenshot. The intent matches CLAUDE.md "focus rects". No regression. | — |
| 11 | info (pre-existing, not this branch) | `other-runas-console-dark-1280x800.png` | In RunAs Console's Recent launches list, "When" and Identity are truncated at 1280 ("2025-11-11 11:...", "TrustedInstaller (F..."). This is identical on 6e4df2b. | modules/runas column widths |

**Not defects (checked):**
- The four themes' colours are consistent in both layouts (`themes-baseline.png`, `themes-console.png`): accent, protected red,
  running green, and Boring's boxed chips. No wrong-theme colours.
- Partly visible rows and lines at the bottom of the lists and detail panes are ordinary scroll clipping, and each has a scrollbar.
- The Name column uses a larger font than the other cells. That is the e5ee164 revert, so it is intended.

## Suggested priority for Finalize
- 1 to 4 share one root cause: the services page doesn't degrade below about 1000 px of content.
- Fix them in the module with column, strip and wrap fallbacks, and/or have the lead add a DPI-scaled minimum track size in core/host (5).
- At minimum, 2 (the current start type is hidden) and 4 (F10/F11/Undo can't be clicked) should be fixed before merge.
