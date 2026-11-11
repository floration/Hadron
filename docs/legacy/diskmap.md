# Legacy DiskMap: reference for the port

Source (read-only): `legacy/diskmap/diskmap.cpp` (1571 lines), `legacy/diskmap/res.rc` (2 lines),
`legacy/diskmap/app.manifest` (12 lines). All line numbers below are `diskmap.cpp` lines unless a file is named.
Written from the source alone. Nothing here was run; where behaviour depends on Windows and could not be checked by
reading, the text says "unverified".

Build line (comment, lines 2-3): `x86_64-w64-mingw32-g++ -O2 -municode -mwindows diskmap.cpp res.o -o DiskMap.exe -static -lgdi32 -lmsimg32 -lshell32 -ldwmapi`.

Resources:
- `res.rc` line 1: `1 ICON "app.ico"`; line 2: `1 24 "app.manifest"` (RT_MANIFEST, id 1). No VERSIONINFO.
- `app.manifest`: assemblyIdentity `DiskMap` 1.0.0.0 (line 3); `asInvoker` (line 5); `<dpiAware>true</dpiAware>`,
  i.e. system-DPI aware, no `dpiAwareness`/PerMonitorV2 (line 7); `longPathAware true` (line 8); one supportedOS,
  `{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}` = Windows 10 (line 11). No Common Controls v6 dependency (none needed: no common controls are used).

---

## 1. Function map

### Data model and globals
| Lines | Item | What it is |
|---|---|---|
| 25-27 | `u64`/`u32`/`u8` | typedefs |
| 29-34 | `FILE_ATTRIBUTE_RECALL_ON_*` | SDK-gap defines (0x00400000, 0x00040000) |
| 36-37 | `WM_APP_SCANDONE` = `WM_APP+1`, `TIMER_PROGRESS` = 1 | worker-done message, progress timer id |
| 40-42 | `enum Cat`, `NOCTX` = 255 | 12 categories; "no directory context" |
| 44-61 | `kCatName`, `kCatColor` | category names and colours (section 3) |
| 64-78 | `SF_*` enum, `SAFE_COL`, `SafeInfo`, `kSafe` | safe-to-delete reasons, names, descriptions |
| 79-102 | `SafeRule`, `kSafeDirRules`, `kSafeFileRules` | pattern tables |
| 103-105 | `CompiledRule`, `g_dirRules`, `g_fileRules`, `g_fileDepth[64]` | compiled patterns; which directory depths can hold a file rule |
| 149-157 | theme colours | section 3 |
| 160 | `struct FileE` | size, name offset, dir index, cat, safe reason |
| 161-170 | `struct DirE` | name, parent, file range, kids, recursive total, per-category and per-reason sums, own safe reason, recursive file/dir counts, context, dominant category |
| 172-174 | `g_files`, `g_dirs`, `g_names` | flat arrays; names in one `wchar_t` pool addressed by `u32` offset |
| 176-180 | `AddName` | append a NUL-terminated name to the pool, return its offset |
| 181 | `NameOf` | offset to pointer |
| 184-194 | scan state | `ST_IDLE/ST_SCANNING/ST_DONE`, cancel flag, file/byte counters, `g_curLock` + `g_curPath[1024]`, thread handle, scanned letter, drive used/total, start tick, duration |
| 196-199 | view state | `g_view` (dir index), `g_filter` (-1 none, 0..11 category, 100 = all safe, 100+r one reason), `g_sideTab` |
| 200-203 | `SafeMatch` | does a reason pass the current safe filter |
| 204 | `SafeSum` | sum of reasons 1..9 |
| 205 | `g_isAdmin` | |
| 207-208 | `Drive`, `g_drives` | letter, label[64], total, free, type |
| 210-213 | `Click`, `g_clicks`, hover ids | clickable regions rebuilt every paint |
| 215-219 | `Hit`, `g_hits`, hover hit, top-level hover rect | treemap hit list (kind 0 file, 1 dir, 2 lump) |
| 221-222 | `Lump`, `g_lumps` | "small items" blocks |
| 224-225 | `ListE`, `g_largest` | sidebar list (kind 0 file, 1 folder) |
| 227-229 | `g_dpi`, `S()`, fonts | `S(v) = MulDiv(v, g_dpi, 96)` (line 228) |
| 231-236 | layout rects, map back buffer, dirty flag, mouse tracking flag | |
| 238-241 | control ids | `ID_RESCAN`=1, `ID_CANCEL`=2, `ID_ADMIN`=3, `ID_UP`=4, `ID_DRIVE`=100, `ID_CRUMB`=200, `ID_CAT`=300, `ID_FILE`=400, `ID_CLEARFILTER`=500, `ID_TAB`=600, `ID_SAFE`=700 (the per-item index goes in `Click.data`, not added to the id) |

### Helpers (formatting, GDI, paths)
| Lines | Function | What it does |
|---|---|---|
| 244-254 | `U64Str` | integer with `,` every three digits (hard-coded, not locale) |
| 256-263 | `FmtSize` | bytes to "N B" / "x.xx KB".."PB" (section 2, Formats) |
| 265-270 | `Mix` | linear blend of two colours, t clamped to 0..1 |
| 271 | `Lighten` | blend toward white |
| 272 | `Darken` | blend toward black |
| 274-277 | `FillC` | fill rect with DC brush |
| 278-284 | `RoundC` | filled round rect, brush and pen same colour |
| 285-292 | `RoundOutline` | round-rect outline with a created pen |
| 293-299 | `Outline` | `width` nested 1-px `FrameRect`s |
| 300-308 | `GradV` | vertical `GradientFill` |
| 309-314 | `Text` | `DrawTextW` with font/colour, always `DT_NOPREFIX` |
| 315-320 | `TextW` | text width via `GetTextExtentPoint32W` |
| 321-323 | `TL`/`TR`/`TC` | single-line vcentred left (with end ellipsis) / right / centre formats |
| 325 | `AddClick` | register a clickable rect |
| 326-328 | `IsHover` | is (id, data) the hovered clickable |
| 330-339 | `DirPath` | rebuild `C:\a\b` from the parent chain (no `\\?\`) |
| 340-345 | `FilePath` | `DirPath` + `\` + file name |

### Categorisation and safe-to-delete rules
| Lines | Function | What it does |
|---|---|---|
| 107-112 | `SplitPat` | split a pattern on `\` into segments |
| 113-120 | `CompileRules` | compile both rule tables; mark `g_fileDepth[segments-1]` for file rules |
| 121-125 | `Glob` | recursive `*` match within one segment (no `?`, case-sensitive; callers lower-case first) |
| 126-134 | `MatchSegs` | segment matcher; `**` matches zero or more whole segments |
| 135-138 | `DirSafe` | first dir rule matching the root-relative segment list, else `SF_NONE` |
| 139-146 | `FileSafe` | file rules, only tried when the dir depth is in `g_fileDepth`; pushes/pops the file name on `segs` |
| 348-351 | `In` | exact match against a NULL-terminated string list |
| 352-356 | `Lower` | copy (truncate to cap-1) and `CharLowerBuffW` |
| 358-375 | `kTempDirs`, `kGameDirs`, `kRootWin`, `kRootProg`, `kRootDrv`, `kDevDirs` | directory-name lists (section 3) |
| 377-397 | `DirCtx` | directory context from parent context, lower-case name and depth |
| 399-420 | `kVideo` .. `kTmp` | extension lists (section 3) |
| 422-442 | `FileCat` | file category: context wins, then root system files, then extension |

### Scan (worker thread)
| Lines | Function | What it does |
|---|---|---|
| 445-509 | `ScanDir` | recursive enumeration of one directory with `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)`; publishes the current path; records files, then recurses into subdirs |
| 511-541 | `Finalize` | per-dir sums, sort files by size desc, bottom-up roll-up of totals/cats/reasons/counts, dominant category, sort kids by total desc |
| 543-557 | `ScanThread` | thread proc: create root `X:\`, scan `\\?\X:\`, finalize unless cancelled, post `WM_APP_SCANDONE` |
| 559-565 | `ClearData` | free all scan data and hover state |
| 567-584 | `RefreshDrives` | enumerate fixed/removable drives with size, free and label |
| 586-638 | `ComputeLargest` | sidebar list: 14 largest files under the view (Categories tab) or 14 biggest flagged spots (Safe tab) |
| 640-647 | `SetView` | change the viewed directory (only when done) |
| 649-671 | `StartScan` | cancel and join any running scan, reset state, reserve arrays, start the thread (256 MB stack reservation) and the 100 ms timer |

### Treemap layout
| Lines | Function | What it does |
|---|---|---|
| 674-675 | `Item`, `RF` | layout input (size, id, kind) and float rect |
| 677-713 | `Squarify` | squarified treemap of items (pre-sorted desc) into a rect |
| 715-719 | `FileMatch` | does a file pass the current filter |
| 720-723 | `FileColor` | file block colour (section 3, Map colouring) |
| 724-731 | `MixColor` | aggregate colour for a dir leaf or lump under a filter |
| 741-744 | `ToRect` | float rect to rounded `RECT` |
| 756-841 | `DrawDir` | recursive: frame + header for a dir, lump small items, squarify, recurse into dirs, draw file and lump leaves, push hits |
| 843-863 | `RenderMap` | (re)create the cached map bitmap at map size, clear hits/lumps/hover, draw the view |

### Painting
| Lines | Function | What it does |
|---|---|---|
| 733-739 | `Leaf` | one block: 1-px gap at right/bottom if ≥3 px, gradient if area ≥40 px², else flat |
| 746-754 | `LeafLabel` | name + size inside a file block if big enough and not dimmed |
| 866-873 | `Layout` | compute toolbar, sidebar, breadcrumb, map and status rects |
| 875-882 | `Button` | primary/secondary rounded button with hover, optional disabled state |
| 884-890 | `DrawLogo` | four coloured rectangles (games, programs, images, video colours) |
| 892-939 | `DrawToolbar` | logo, title, drive buttons, Run as admin, Cancel/Rescan |
| 941-945 | `SectionLabel` | small caps-style section header, advances y |
| 947-1156 | `DrawSidebar` | legend (not done) or current view, drive breakdown, tabs, category or safe rows, explanation box, largest list |
| 1158-1199 | `DrawCrumb` | Up button and breadcrumb with left elision |
| 1201-1235 | `DrawProgress` | scan progress card |
| 1237-1259 | `DrawEmpty` | idle screen with drive tiles |
| 1261-1292 | `HoverText` | status-bar text for the hovered block |
| 1266-1269 | lambda `safeInside` | appends "… safe to delete inside" |
| 1294-1319 | `DrawStatus` | status bar |
| 1321-1335 | `DrawMapArea` | progress / empty / blit cached map + hover outlines |
| 1337-1358 | `Paint` | full-window double buffer per paint; calls Layout, clears `g_clicks`, draws everything |
| 1453-1465 | `MakeFonts` (lambda `mk` 1454-1457) | create the seven fonts once |

### Input and hit testing
| Lines | Function | What it does |
|---|---|---|
| 1361-1391 | `UpdateHover` | hit-test clickables (last added wins) and map hits (deepest wins), find the top-level ancestor rect, set cursor, invalidate on change |
| 1393-1396 | `ShowInExplorer` | `ShellExecuteW("open", "explorer.exe", "/select,\"path\"")` |
| 1398-1401 | `OpenFolder` | `ShellExecuteW("open", "explorer.exe", "\"path\"")` |
| 1403 | `GoUp` | view parent (when done and not at root) |
| 1405-1409 | `RelaunchAdmin` | `ShellExecuteW("runas", exe)`, close on success |
| 1411-1450 | `OnClick` | dispatch clickables by id; otherwise treemap click / ctrl-click |

### Window proc and WinMain
| Lines | Function | What it does |
|---|---|---|
| 1467-1530 | `WndProc` | 14 messages, see checklist "Window messages" |
| 1532-1571 | `wWinMain` | init lock, system DPI, admin check, fonts, rules, drives; register `DiskMapWnd`; create centred window; dark title bar; optional drive argument; message loop |

Other lambdas (sort comparators): 521-522, 539, 615, 620 (`cmp`, min-heap by size), 817, 1035.

---

## 2. Parity checklist

### Layout regions (`Layout`, 866-873; all values through `S()`)
- [ ] Toolbar across the top, height 60 (`rcTool`).
- [ ] Sidebar on the left, width 300, from toolbar bottom to status-bar top (`rcSide`); 1-px `BORDER` line on its right edge (949).
- [ ] Status bar across the bottom, height 28 (`rcStatus`); 1-px `BORDER` line on its top edge (1296).
- [ ] Breadcrumb strip right of the sidebar, height 40 (`rcCrumb`).
- [ ] Map area: left = sidebar right + 10, right = client right − 12, top = breadcrumb bottom, bottom = status top − 10 (`rcMap`).
- [ ] Window background `BG` (1346); toolbar, sidebar, status bar `PANEL`.
- [ ] Minimum track size 900 × 600 (scaled) (1474).
- [ ] Initial size min(1400 scaled, 90 % of work area) × min(880 scaled, 90 %), centred in the primary work area (1550-1556).
- [ ] Window title "DiskMap (Administrator)" when admin, else "DiskMap" (1553).
- [ ] Dark title bar via `DwmSetWindowAttribute(20, TRUE)` (1557-1558). (Host job in the port.)

### Toolbar (`DrawToolbar`, 892-939)
- [ ] Logo at x 18, size 22, vertically centred (896); title "DiskMap" in `g_fTitle`, `TEXT1`, from x 50 (897-898).
- [ ] Drive buttons start at x 160, each 160 × 46, gap 8 (900-923). A drive whose button would pass `toolbar right − 260` is not shown, and no later drive is either (`break`, 904). No overflow menu.
- [ ] Drive button text line 1: `"%lc:  %ls"` = letter, colon, two spaces, label (909), `g_fSemi`, `TEXT1`.
- [ ] Drive button line 2: `"<free> free"` (919), `g_fSmall`, `TEXT2`.
- [ ] Mini usage bar (913-917): track `RGB(52,57,67)`, fill `ACCENT`, or `RGB(236,98,98)` when used > 90 %.
- [ ] Active drive (state not idle and letter = scanned letter): bg `RGB(30,44,66)`, outline `ACCENT` (905-908). Hover: bg `PANEL3`, outline `RGB(70,76,88)`. Else `PANEL2` / `BORDER`.
- [ ] Drive buttons are clickable in every state, including during a scan (922). Clicking one during a scan cancels and restarts on that drive (649-655), even the same drive.
- [ ] "Run as admin" button (`ID_ADMIN` = 3), secondary, 132 × 32, right-aligned 16 from the edge, shown whenever not admin (927-931).
- [ ] "Cancel" (`ID_CANCEL` = 2), secondary, 96 × 32, left of "Run as admin" (or at the right edge), only while scanning (932-934).
- [ ] "Rescan" (`ID_RESCAN` = 1), primary (accent), 96 × 32, only when done (935-938).
- [ ] Button style (875-882): radius 8; primary bg `ACCENT`, hover `Lighten(ACCENT, 0.12)`, text `RGB(10,14,22)`; secondary bg `PANEL2`/hover `PANEL3`, outline `BORDER`/hover `RGB(70,76,88)`, text `TEXT1` (disabled `TEXT3`); font `g_fSemi`, centred. A disabled button is not clickable and shows no hover.

### Idle screen (`DrawEmpty`, 1237-1259; state `ST_IDLE`)
- [ ] Logo 48 px centred above (1239).
- [ ] "Pick a drive to map" in `g_fHuge`, `TEXT1`, centred (1241).
- [ ] "Every file becomes a block sized by how much space it uses, colored by what it is." `g_fUI`, `TEXT2` (1243).
- [ ] When not admin: "Tip: Run as admin to include protected Windows and user folders." `g_fSmall`, `TEXT3` (1244-1247).
- [ ] Up to 8 drive tiles, 76 × 50, pitch 84, text `"%lc:"` in `g_fTitle`; hover bg `PANEL3` + `ACCENT` outline (1248-1258). Centring uses the full drive count, so with more than 8 drives the row is off-centre (1248; bug, not parity-critical).
- [ ] Sidebar while idle or scanning: "CATEGORIES" header and all 12 categories with colour swatches, names in `TEXT2`, no sizes, not clickable (953-962).
- [ ] Status bar while idle or scanning: "Running as administrator" or "Running as standard user \x2014 some system folders can't be read" (em dash), `TEXT3` (1314-1318).
- [ ] No breadcrumb while idle or scanning (1159).

### Scan progress and cancel (`DrawProgress`, 1201-1235; `StartScan` 649-671)
- [ ] Card centred in the map, width min(560, map width − 40), height 220, radius 14, `PANEL` + `BORDER` outline (1203-1206).
- [ ] Title `"Mapping %lc:\\"` → "Mapping C:\" in `g_fTitle` (1208-1209).
- [ ] Progress bar: fraction = scanned bytes ÷ drive used bytes, capped at 0.99, 0 if used is unknown; min fill width 8 (1212-1216). This is an estimate, not a measured percentage.
- [ ] Line `"%ls files  \x00B7  %ls"` → "1,234 files  ·  5.67 GB" (1221), and the percentage `"%d%%"` right-aligned in `g_fSemi`, `ACCENT` (1220, 1223).
- [ ] Current path (without `\\?\`), `g_fSmall`, `TEXT3`, path ellipsis (1225-1230).
- [ ] `"Elapsed %u:%02u"` (minutes not capped, e.g. "75:03") (1232-1234).
- [ ] Repaint every 100 ms via `TIMER_PROGRESS` (669, 1510-1512).
- [ ] Cancel sets the flag (1418); the worker stops at the next check (446, 485, 493), skips `Finalize`, posts done; the UI clears all data and returns to the idle screen (1517). Partial results are discarded.
- [ ] Escape does **not** cancel a scan (Escape only means Up, and Up does nothing unless done).

### Current-view panel (sidebar, state `ST_DONE`, 965-980)
- [ ] "CURRENT VIEW" header (966).
- [ ] Name: the full path at the root (i.e. "C:\"), else the folder name; `g_fSemi`, `TEXT1` (967-969).
- [ ] Total size of the view in `g_fBigNum` (971-973).
- [ ] `"%ls files  \x00B7  %ls folders"` with thousands separators, recursive counts (975-979).

### Drive breakdown (982-1007; only at the root, on the Categories tab, when the drive size is known)
- [ ] Bar 8 high, radius 4, track `RGB(46,51,60)`; segment "mapped" `ACCENT`, segment "not reachable" `RGB(110,118,136)` (984-993).
- [ ] mapped = min(root total, drive used); not reachable = used − mapped; free = total − used (987-989, 996). Drive used/total come from the drive list at scan start (659).
- [ ] Rows with swatch, label, right-aligned size: "Mapped", "Not reachable (protected/system)", "Free" (997-1005).
- [ ] Note: the "Mapped" row is clamped to drive used, while the big number above is not, so the two can disagree (hard links, see section 4).

### Sidebar tabs (1009-1023)
- [ ] Segmented control, height 32, radius 9, `PANEL2`; tabs "Categories" and "Safe to delete" (1014).
- [ ] Selected tab: `PANEL3` pill (radius 7), text `TEXT1`, not clickable. Unselected: `TEXT2`, hover `TEXT1`, clickable `ID_TAB` (600) with data 0/1 (1017-1020).
- [ ] Switching tabs sets the filter to none (Categories) or "all safe" (Safe to delete), marks the map dirty and recomputes the list (1427-1429).
- [ ] The tab survives rescans and drive changes; a new scan starts with filter "all safe" if the Safe tab is selected (662).

### Categories tab (1025-1058)
- [ ] Header "CATEGORIES"; "Show all" link (`ID_CLEARFILTER` = 500), right-aligned, `ACCENT`, hover `TEXT1`, only when a category filter is active (1026-1032).
- [ ] One row per category with a non-zero size in the view, sorted by size descending (1034-1038).
- [ ] Row: 32 high, pitch 34; swatch 10 × 10 radius 4; name (`g_fSemi` when selected, else `g_fUI`); size right-aligned in `g_fUI`; 2-px share bar (track `RGB(40,44,52)`, fill category colour) = category bytes ÷ view total (1039-1056).
- [ ] Selected row bg `PANEL3`, hovered `PANEL2`, radius 8. With a filter active, other rows dim: swatch and bar `Mix(colour, PANEL, 0.6)`, text `TEXT3` (1041-1054).
- [ ] Click a row (`ID_CAT` = 300, data = category): set that filter; click the selected row again: clear (1423). The largest-files list is not recomputed and ignores the category filter.

### Safe-to-delete tab (1059-1118)
- [ ] Header "YOU COULD FREE UP"; "Show all" link only when one reason is selected (1062-1067).
- [ ] Total of all reasons in `g_fBigNum`: `SAFE_COL` when non-zero, else "0 B" in `TEXT2` (1069-1071).
- [ ] One row per reason with non-zero bytes, sorted by bytes descending (insertion sort, 1073-1075); 30 high, pitch 32; green swatch; name; size; share bar = reason ÷ safe total (1077-1098).
- [ ] Click a reason (`ID_SAFE` = 700, data = reason 1..9): filter to it; click again: back to "all safe" (1431). List recomputed.
- [ ] Explanation box (1100-1117): text for the hovered reason, else the selected reason, else "Green = things Windows or apps rebuild on their own, or only keep for troubleshooting. Hover a group for details. DiskMap never deletes anything itself." (when total > 0) or "Nothing in this folder matched the safe-to-delete rules." When not admin and no reason is hovered/selected, append "\n\nRun as admin to find more \x2014 some of these folders are admin-only." Box bg `Mix(PANEL2, SAFE_COL, 0.06)`, 3-px `SAFE_COL` stripe on the left, `g_fSmall`, `TEXT2`, word-wrapped.
- [ ] Reason descriptions verbatim in section 3.

### Largest files / biggest spots list (1120-1155; `ComputeLargest` 586-638)
- [ ] Shown only if there are items and at least 80 px left in the sidebar (1121). Header "LARGEST FILES" (Categories tab) or "BIGGEST SPOTS TO CLEAN" (Safe tab) (1122).
- [ ] Up to 14 items (`K`, 589), cut off when the next row would pass `sidebar bottom − 8` (1124). **No scrolling, no scrollbar, no sorting by column, no keyboard navigation.**
- [ ] Categories tab: the 14 largest files anywhere under the view, by size descending, ignoring any filter (620-637).
- [ ] Safe tab: if the view itself is flagged and matches the filter, its children and its own files; otherwise the top-most flagged folders and individually flagged files under the view that match the filter; 14 largest by size (590-618). Zero-size entries are excluded.
- [ ] Row 34 high, pitch 36: 4 × 20 colour stripe (category colour for files on the Categories tab, else `SAFE_COL`); name (folders get a trailing `\` and `g_fSemi`); size right-aligned `g_fSmall` `TEXT2`; second line the parent folder path in `g_fSmall` `TEXT3` with path ellipsis; hover bg `PANEL2` (1136-1153).
- [ ] Click (`ID_FILE` = 400, data = index): a file opens Explorer with it selected; a folder opens that folder in Explorer (1433-1438).

### Breadcrumb (`DrawCrumb`, 1158-1199; only when done)
- [ ] "\x2190  Up" (left arrow, two spaces) button (`ID_UP` = 4), 64 × 28, at the map's left edge; disabled at the root (1162-1163).
- [ ] Root name ("C:\"), then each folder separated by `"  \x203A  "` (›) in `TEXT3` (1181-1184).
- [ ] The last segment is `g_fSemi` `TEXT1` and not clickable; others `g_fUI` `TEXT2`, hover `TEXT1` on a `PANEL2` radius-6 pill (1187-1196).
- [ ] When too wide, segments are dropped from the left after the root, and the separator becomes `"  \x203A  \x2026  \x203A  "` (… elision); the last segment is always kept (1168-1177, 1181).
- [ ] Click a segment (`ID_CRUMB` = 200, data = dir index): view it (1422).

### Treemap (`DrawDir` 756-841, `RenderMap` 843-863)
- [ ] The current view fills the map area; it has no frame or header itself (765, depth 0).
- [ ] Sub-folders get a frame; if wider than 64 and taller than 40, a 17-high header with the name (`g_fSmallSemi`, `TEXT2`, end ellipsis) and, if wider than 140, the size right-aligned (`g_fSmall`, `TEXT3`); children inset left 2, right 3, bottom 3. Otherwise a 1-px inset (771-782).
- [ ] Folders smaller than 6 px in either dimension are one flat block (763). Inner area under 3 px or empty total: one block or nothing (785-788). (6 and 3 are unscaled pixels.)
- [ ] Items whose area would be under ~30 px² (`thr = total / (iw·ih) · 30`, 791) are folded into one "small items" lump per folder, drawn as `Mix(MixColor(...), PANEL2, 0.35)` (836).
- [ ] Items laid out by squarified treemap, largest first (817-820). Zero-size files and folders are not drawn.
- [ ] File blocks show name (`g_fSmallSemi`, `RGB(16,18,22)`) and size below (`g_fSmall`, `RGB(36,40,48)`) when at least 70 × 32 and not dimmed (746-754).
- [ ] Block shading: 1-px dark gap right/bottom; vertical gradient `Lighten(c,0.14)` → `Darken(c,0.16)` when area ≥ 40 px² (733-739).
- [ ] Colouring rules in section 3.
- [ ] The map is cached in its own bitmap and re-rendered only when dirty (filter, view, size, scan done) (1324).

### Hover states and cursors (`UpdateHover` 1361-1391, `DrawMapArea` 1326-1334, `WM_SETCURSOR` 1492-1498)
- [ ] Every clickable shows a hover state (listed per element above).
- [ ] Hovered map block: 1-px white outline (1332-1333).
- [ ] Its top-level ancestor (the direct child of the view that a click would open), or the block itself if it is a top-level file: 2-px `ACCENT` outline, inflated by 1 (1327-1331, 1379-1385). A top-level lump gets no accent outline (`topKind` 255, 838).
- [ ] Hand cursor over any clickable, or over a map block whose top-level ancestor is a folder; arrow elsewhere, including over top-level files and lumps (1388, 1494-1495).
- [ ] The deepest block under the mouse wins (hits are pushed parent-first, searched last-first) (1371-1372).
- [ ] Hover clears on mouse leave (`TrackMouseEvent(TME_LEAVE)`, 1478-1481, 1485-1491).

### Mouse
- [ ] **Click** on a map block under a top-level folder: open that top-level folder (one level down), not the deepest folder clicked (1449).
- [ ] Click on a top-level file or top-level lump: nothing (1449).
- [ ] **Ctrl+click** a file block: Explorer with the file selected; a folder block: Explorer with the folder selected in its parent (`/select,`, not opened); a lump: nothing (1444-1447).
- [ ] Map clicks use the hover hit from the last mouse move, not the click point (1442-1443); clickables use the click point (1413-1414).
- [ ] **Right-click** anywhere: Up (1503).
- [ ] **Mouse back button** (XBUTTON1): Up; returns TRUE. XBUTTON2: nothing (1504).
- [ ] **Wheel: not handled.** There is no `WM_MOUSEWHEEL` case; nothing scrolls (no scrollable list exists).
- [ ] No double-click handling (class has no `CS_DBLCLKS`; style 0, 1540-1548).

### Keyboard (`WM_KEYDOWN`, 1505-1509) — the complete list
- [ ] `VK_BACK` (Backspace): Up.
- [ ] `VK_ESCAPE`: Up.
- [ ] `VK_F5`: rescan the same drive, only when done.
- [ ] `VK_HOME`: go to the root, only when done.
- [ ] No other keys. No Tab focus, no focus rects, no arrow-key navigation, no high-contrast handling.

### Status bar (`DrawStatus` 1294-1319, `HoverText` 1261-1292)
- [ ] When done: right-aligned hint "Click: open folder   Right-click / Backspace: up   Ctrl+click: show in Explorer" (three spaces between items), `g_fSmall`, `TEXT3` (1297-1302).
- [ ] When done and nothing hovered: `"Mapped %ls in %ls files  \x00B7  %ls safe to delete  \x00B7  scan took %.1fs"` → "Mapped 412 GB in 1,234,567 files  ·  18.2 GB safe to delete  ·  scan took 41.3s" (1305-1307). Values are for the root, not the view.
- [ ] Hover over a file: `"<path>   ·   <size>   ·   <category>"` + `"   ·   Safe to delete: <reason name>"` if flagged (1270-1275).
- [ ] Hover over a folder: `"<path>   ·   <size>   ·   <n> files   ·   mostly <dominant category>"` + either `"   ·   Safe to delete: <reason>"` (folder flagged) or `"   ·   <size> safe to delete inside"` (contains flagged bytes) (1276-1283).
- [ ] Hover over a lump: `"<n> small files grouped together   ·   <size>   ·   mostly <category>"` + optional `"   ·   <size> safe to delete inside"`; n counts files inside folded folders too (799, 808, 1284-1289).
- [ ] The separators in hover text are three spaces, `·` (U+00B7), three spaces; in the summary, two spaces each side.
- [ ] Left text `g_fSmall`, `TEXT2`, path ellipsis, clipped before the hint (1310-1312).
- [ ] When not done: the admin / standard-user line above.

### Formats
- [ ] `FmtSize` (256-263): below 1024 → `"<n> B"` with thousands separators (e.g. "1,023 B"). Otherwise divide by 1024 until < 1024 (max PB); `"%.2f"` if < 10, `"%.1f"` if < 100, else `"%.0f"`, a space, then `KB`/`MB`/`GB`/`TB`/`PB` (binary multiples with decimal names). Always `.` as decimal point (C locale). Edge: values just under a unit boundary print as e.g. "1024 KB" (1048575 B → 1023.999 → `%.0f`).
- [ ] `U64Str` (244-254): `,` every three digits, not locale-aware. Used for file and folder counts and for byte counts below 1 KB.
- [ ] Sizes are logical file sizes (`nFileSizeHigh/Low`, 475), not allocation size.

### Command-line argument (1562-1566)
- [ ] If the command line is non-empty, its first character (or the second, if the first is `"`) is upper-cased; if it is A-Z, a scan of that letter starts immediately. `DiskMap.exe D`, `D:`, `D:\` all work.
- [ ] Not validated against the drive list: any argument starting with a letter (e.g. `Downloads`) scans that letter; a letter with no drive gives an empty map with 0 % progress. Options starting with `-` are ignored.

### Drive list (`RefreshDrives`, 567-584)
- [ ] `GetLogicalDrives`, A-Z; only `DRIVE_FIXED` and `DRIVE_REMOVABLE`; skipped if `GetDiskFreeSpaceExW` fails (e.g. empty card reader).
- [ ] Free = total free bytes (`lpTotalNumberOfFreeBytes`, not the caller's quota).
- [ ] Label from `GetVolumeInformationW`; if that fails or is empty: "Removable" or "Local Disk".
- [ ] Refreshed at startup and after every scan completes or is cancelled (1520); not on device arrival. No folders, network drives, CD drives or letterless mounted volumes.
- [ ] Only drive roots can be scanned; there is no folder picker.

### Admin relaunch (927-931, 1405-1409, 1535)
Recorded for completeness. Under the current rule 4 (all exes `requireAdministrator`, no relaunch) these items are
expected deviations, not parity targets; see section 5, items 2-5.
- [ ] "Run as admin" relaunches the same exe with `runas` and no arguments; on success (`> 32`) closes this window; on failure or UAC cancel nothing visible happens.
- [ ] Admin-dependent text: window title, idle tip, status line, safe-tab hint.

### Explorer integration (1393-1401)
- [ ] Show in Explorer: `explorer.exe /select,"<path>"` (ctrl-click on map, file rows in the list).
- [ ] Open folder: `explorer.exe "<path>"` (folder rows in the list on the Safe tab).
- [ ] Paths are plain `C:\...` (no `\\?\`).
- [ ] Note: the status hint says "Click: open folder", meaning drill into the folder in the map, not Explorer.

### Window messages handled by `WndProc` (1467-1530)
- [ ] `WM_PAINT` → `Paint` (1469): full double buffer each paint.
- [ ] `WM_ERASEBKGND` → 1, no erase (1470).
- [ ] `WM_SIZE` → map dirty, invalidate (1471).
- [ ] `WM_GETMINMAXINFO` → min 900 × 600 scaled (1472-1476).
- [ ] `WM_MOUSEMOVE` → start leave tracking, `UpdateHover` (1477-1484).
- [ ] `WM_MOUSELEAVE` → clear hover, invalidate (1485-1491).
- [ ] `WM_SETCURSOR` → hand/arrow in the client area, else default (1492-1498).
- [ ] `WM_LBUTTONDOWN` → `OnClick(ctrl = MK_CONTROL)`, then `UpdateHover` (1499-1502). Acts on button down, not up.
- [ ] `WM_RBUTTONDOWN` → Up (1503).
- [ ] `WM_XBUTTONDOWN` → Up on XBUTTON1, return TRUE (1504).
- [ ] `WM_KEYDOWN` → Backspace, Escape, F5, Home (1505-1509).
- [ ] `WM_TIMER` → invalidate while scanning (1510-1512).
- [ ] `WM_APP_SCANDONE` (`WM_APP+1`) → kill timer, join thread, record duration, then idle (cancelled) or done with view = root and list computed; map dirty; refresh drives (1513-1523).
- [ ] `WM_DESTROY` → cancel, wait up to 5 s, `PostQuitMessage` (1524-1527).

---

## 3. Data and rules

### Categories (`kCatName` 44-46, `kCatColor` 48-61)
| # | Name | RGB | Line |
|---|---|---|---|
| 0 | Games | 236, 98, 98 | 49 |
| 1 | Programs | 84, 146, 236 | 50 |
| 2 | Windows | 140, 152, 172 | 51 |
| 3 | Drivers | 172, 124, 236 | 52 |
| 4 | Video | 238, 164, 66 | 53 |
| 5 | Audio | 232, 100, 170 | 54 |
| 6 | Images | 72, 196, 164 | 55 |
| 7 | Documents | 226, 208, 96 | 56 |
| 8 | Archives & disk images | 180, 136, 96 | 57 |
| 9 | Developer | 116, 204, 104 | 58 |
| 10 | Temp & cache | 98, 112, 134 | 59 |
| 11 | Other | 120, 124, 134 | 60 |

Safe-to-delete colour `SAFE_COL` = RGB(80, 214, 140) (65).

### Safe-to-delete reasons (`kSafe`, 67-78), verbatim (`\x2014` = —, `\x2192` = →)
| id | Name | Description | Line |
|---|---|---|---|
| 1 `SF_DUMPS` | Crash & error reports | Memory dumps and error reports Windows saves after a crash or driver reset (LiveKernelReports, Minidump, WER). Only useful for debugging — glance at the file names first to see what crashed. | 69 |
| 2 `SF_TEMP` | Temporary files | Scratch files that apps and installers left behind. Anything still in use will refuse to delete — just skip those. | 70 |
| 3 `SF_UPDATE` | Windows Update downloads | Update packages that are already installed, plus the Delivery Optimization cache. Easiest to clear with Disk Cleanup → Clean up system files. | 71 |
| 4 `SF_OLDWIN` | Previous Windows install | A backup of your previous Windows version. Removing it means you can't roll back. Use Disk Cleanup → Clean up system files → Previous Windows installation(s). | 72 |
| 5 `SF_RECYCLE` | Recycle Bin | Files you already deleted. Empty the Recycle Bin to get this space back. | 73 |
| 6 `SF_CACHE` | App & browser caches | Browsers and apps rebuild these automatically. Close the app first; the next launch or page load may be a little slower. | 74 |
| 7 `SF_SHADER` | Shader caches | Compiled GPU shaders. Games and drivers rebuild them — the first launch afterwards may stutter briefly. | 75 |
| 8 `SF_THUMB` | Thumbnail & icon cache | Explorer's thumbnail and icon cache. Rebuilt automatically as you browse folders (Disk Cleanup can clear it too). | 76 |
| 9 `SF_DRVPKG` | Driver installer leftovers | Files AMD/NVIDIA installers extracted during past driver installs. The installed driver doesn't use them. | 77 |

Note: these are user-facing claims. They carry no numbers, so rule 7's number check does not fire, but they are claims
and should go through `kb-auditor` (see section 5).

### Safe-to-delete pattern tables (81-102), verbatim
Patterns are relative to the root of the scanned drive (any drive, not only the system drive), lower case. `*` matches
any run of characters inside one segment, `**` matches zero or more whole segments (comment, line 80). In the C++
source each `\` is written `\\`.

Directory rules (`kSafeDirRules`, 81-97), in table order (first match wins, 136):
| Reason | Pattern | Line |
|---|---|---|
| SF_DUMPS | `windows\livekernelreports` | 82 |
| SF_DUMPS | `windows\minidump` | 82 |
| SF_DUMPS | `programdata\microsoft\windows\wer` | 83 |
| SF_DUMPS | `users\*\appdata\local\crashdumps` | 83 |
| SF_DUMPS | `users\*\appdata\local\microsoft\windows\wer` | 84 |
| SF_TEMP | `windows\temp` | 85 |
| SF_TEMP | `users\*\appdata\local\temp` | 85 |
| SF_UPDATE | `windows\softwaredistribution\download` | 86 |
| SF_UPDATE | `windows\**\deliveryoptimization\cache` | 87 |
| SF_OLDWIN | `windows.old` | 88 |
| SF_OLDWIN | `$windows.~bt` | 88 |
| SF_OLDWIN | `$windows.~ws` | 88 |
| SF_RECYCLE | `$recycle.bin` | 89 |
| SF_SHADER | `users\*\appdata\local\d3dscache` | 90 |
| SF_SHADER | `users\*\appdata\**\dxcache` | 90 |
| SF_SHADER | `users\*\appdata\**\glcache` | 91 |
| SF_SHADER | `**\steamapps\shadercache` | 91 |
| SF_CACHE | `users\*\appdata\local\microsoft\windows\inetcache` | 92 |
| SF_CACHE | `users\*\appdata\**\cache` | 93 |
| SF_CACHE | `users\*\appdata\**\cache2` | 93 |
| SF_CACHE | `users\*\appdata\**\code cache` | 94 |
| SF_CACHE | `users\*\appdata\**\gpucache` | 94 |
| SF_DRVPKG | `amd` | 95 |
| SF_DRVPKG | `nvidia` | 95 |
| SF_DRVPKG | `programdata\nvidia corporation\downloader` | 96 |

File rules (`kSafeFileRules`, 98-102):
| Reason | Pattern | Line |
|---|---|---|
| SF_DUMPS | `windows\memory.dmp` | 99 |
| SF_THUMB | `users\*\appdata\local\microsoft\windows\explorer\thumbcache_*.db` | 100 |
| SF_THUMB | `users\*\appdata\local\microsoft\windows\explorer\iconcache_*.db` | 101 |

How the rules apply:
- A directory's reason is inherited: once a folder matches, every descendant folder and file gets the same reason
  and no further rules are tried (461, 478, 500).
- A file in an unflagged folder is checked against file rules only if its folder depth equals a file rule's depth
  (1 or 6 here; `g_fileDepth`, 117-118, 140).
- Matching is on names lower-cased with `CharLowerBuffW` (352-356, 476, 494).
- Rules match whole segments: `amd` flags only a root folder named exactly `AMD`; `users\*\appdata\**\cache`
  flags **any** folder named exactly `cache` anywhere under any user's AppData (broad; see section 5).
- `**\steamapps\shadercache` also matches `X:\steamapps\shadercache` (`**` matches zero segments).
- A file is "safe" when `f.safe != 0`; per-folder sums `safeR[1..9]` roll up (517, 530).

### Directory-context rules (`DirCtx`, 377-397)
Evaluated for each sub-folder, with `pc` = the parent's context, `n` = lower-case name, `depth` = 1 for root children.
In this order:
1. Parent is Temp → Temp (378).
2. Name in `kTempDirs` → Temp (379). This beats every other context, including Windows, Games and Programs.
3. Parent is Drivers → Drivers (380).
4. Parent is Windows → Drivers if the name is `driverstore` or `drivers`, else Windows (381-384).
5. Parent is Games → Games (385).
6. Name in `kGameDirs` → Games (386). Beats Programs, so `Program Files\...\Games` is Games.
7. At depth 1: `kRootWin` → Windows; `kRootProg` → Programs; `kRootDrv` → Drivers (387-391).
8. Parent is Programs → Programs (392).
9. Parent is Developer → Developer (393).
10. Name in `kDevDirs` → Developer (394).
11. Name `windowsapps` or `programs` → Programs (395).
12. Otherwise no context (`NOCTX`): files are classified by extension (396).

The root itself has no context (548). A file in a folder with a context takes that category regardless of extension (423).

Name lists, verbatim:
- `kTempDirs` (358-361): temp, tmp, cache, caches, cache2, code cache, gpucache, shadercache, dxcache, glcache, inetcache, crashdumps, crashpad, $recycle.bin, d3dscache, nv_cache, webcache, cachestorage, service worker, softwaredistribution, package cache.
- `kGameDirs` (362-365): steamapps, steamlibrary, epic games, gog games, gog galaxy games, riot games, ea games, origin games, xboxgames, games, my games, .minecraft, ubisoft game launcher, battlestate games, rockstar games, blizzard games, bethesda.net launcher, amazon games, itch, wargaming.net, square enix.
- `kRootWin` (366-368): windows, windows.old, system volume information, recovery, $windows.~bt, $windows.~ws, $winreagent, $sysreset, boot, efi, perflogs, $getcurrent, onedrivetemp.
- `kRootProg` (369): program files, program files (x86), programdata.
- `kRootDrv` (370-371): amd, nvidia, intel, drivers, swsetup, dell, realtek.
- `kDevDirs` (372-375): node_modules, .git, venv, .venv, __pycache__, .cargo, .rustup, .nuget, .gradle, .m2, site-packages, .vscode, .android, android, sdk, miniconda3, anaconda3, .conda, source, repos, msys64, cygwin64, .ollama, huggingface, .cache.

### File categorisation (`FileCat`, 422-442)
1. Folder context set → that category (423).
2. In the drive root only: `pagefile.sys`, `hiberfil.sys`, `swapfile.sys`, `dumpstack.log`, `dumpstack.log.tmp`, `bootmgr` → Windows (424-428).
3. No dot, or a trailing dot → Other (429-430). The extension is the text after the **last** dot.
4. Extension lists, checked in this order (first wins):
   - Video (399-400): mp4, mkv, avi, mov, wmv, webm, m4v, flv, mpg, mpeg, ts, m2ts, 3gp, vob
   - Audio (401-402): mp3, flac, wav, ogg, m4a, aac, opus, wma, aiff, alac, mid, midi
   - Images (403-405): jpg, jpeg, png, gif, bmp, webp, heic, heif, tif, tiff, raw, cr2, cr3, nef, arw, dng, psd, svg, ico, avif, kra, xcf
   - Documents (406-408): pdf, doc, docx, xls, xlsx, ppt, pptx, txt, md, odt, ods, odp, rtf, csv, epub, mobi, pages, one, pst, ost
   - Archives (409-411): zip, rar, 7z, tar, gz, tgz, bz2, xz, zst, iso, img, vhd, vhdx, vmdk, vdi, qcow2, wim, esd, cab, dmg
   - Programs (416-417): exe, dll, msi, msix, appx, msp, ocx
   - Drivers (418): sys, inf, cat, drv
   - Temp (419-420): tmp, temp, log, dmp, etl, bak, old, crdownload, part
   - Developer (412-415): c, cpp, cc, h, hpp, py, js, ts, jsx, tsx, java, cs, go, rs, rb, php, lua, sln, vcxproj, pdb, obj, o, lib, a, ipynb, pt, pth, safetensors, gguf, ckpt, onnx, bin, whl, jar
5. Otherwise Other (441).

Note: `ts` is in both Video and Developer; Video is checked first, so TypeScript `.ts` files outside a dev folder count as Video.

Dominant category of a folder or lump = the category with the most bytes; ties keep the lower index; an all-zero folder is Other (536-538, 811-813).

### Map colouring (715-731, 768-769, 836)
- No filter: file = category colour; small folder leaf/lump = dominant-category colour.
- Category filter: matching file = category colour; others `Mix(cat colour, BG, 0.84)` (dimmed, no label).
- Safe filter: matching file = `SAFE_COL`; others dimmed as above.
- Folder leaves and lumps under a filter: `Mix(Mix(dom colour, BG, 0.84), highlight, sqrt(matching bytes ÷ total))`, highlight = category colour or `SAFE_COL`.
- Lumps are additionally `Mix(…, PANEL2, 0.35)`.
- Folder frame: under a safe filter and the folder itself matches → `Mix(PANEL2, SAFE_COL, 0.28)`; else `Mix(PANEL2, dom colour, 0.06 if any filter else 0.16)`.

### Theme palette (149-157)
| Name | RGB | Line |
|---|---|---|
| BG | 17, 19, 23 | 149 |
| PANEL | 24, 27, 33 | 150 |
| PANEL2 | 32, 36, 43 | 151 |
| PANEL3 | 42, 47, 56 | 152 |
| BORDER | 40, 44, 52 | 153 |
| TEXT1 | 232, 235, 240 | 154 |
| TEXT2 | 156, 163, 176 | 155 |
| TEXT3 | 108, 115, 128 | 156 |
| ACCENT | 96, 160, 255 | 157 |

Inline colours: `RGB(70,76,88)` hover outline (879, 908); `RGB(30,44,66)` active drive bg (907); `RGB(52,57,67)` drive usage track (914);
`RGB(236,98,98)` usage > 90 % (917); `RGB(46,51,60)` bar tracks and "Free" swatch (985, 999, 1214); `RGB(110,118,136)` "Not reachable" (993, 998);
`RGB(40,44,52)` row share-bar tracks (1051, 1093); `RGB(10,14,22)` primary button text (880); `RGB(16,18,22)` / `RGB(36,40,48)` file-block
label name / size (751, 753); `RGB(255,255,255)` hover outline (1333).

Radii (scaled): buttons and drive buttons 8, idle tiles 10, progress card 14, tab control 9, selected tab 7, swatches 4, rows 8, crumb pill 6, bars 4.

### Fonts (`MakeFonts`, 1453-1465)
All `CreateFontW(-S(px), …, DEFAULT_CHARSET, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI")` (1455-1456); px is the character height at 96 DPI.
| Font | px | Weight | Line |
|---|---|---|---|
| `g_fUI` | 13 | FW_NORMAL (400) | 1458 |
| `g_fSemi` | 13 | FW_SEMIBOLD (600) | 1459 |
| `g_fSmall` | 12 | FW_NORMAL | 1460 |
| `g_fSmallSemi` | 12 | FW_SEMIBOLD | 1461 |
| `g_fTitle` | 18 | FW_SEMIBOLD | 1462 |
| `g_fHuge` | 28 | FW_LIGHT (300) | 1463 |
| `g_fBigNum` | 30 | FW_LIGHT | 1464 |

Fonts are created once and never deleted or rebuilt.

---

## 4. Edge cases

- **Reparse points.**
  - Directories with `FILE_ATTRIBUTE_REPARSE_POINT` are skipped entirely: not traversed, not counted, not shown
    (468). This covers junctions, directory symlinks and mount points. Their bytes end up in "Not reachable" only
    if they are on the same volume and not reached another way.
  - **Files** with the reparse attribute are not skipped (467-484): file symlinks, AppExecLinks
    (`WindowsApps` aliases), WSL links, dedup and WOF-compressed files are counted with the size `FindFirstFileExW`
    reports. For a symlink that is the link's own size (usually 0); for dedup/WOF files it is the logical size,
    which overstates disk use. The reparse tag in `dwReserved0` is never read.
- **Cloud placeholders.**
  - Files with `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` (0x00400000) or `FILE_ATTRIBUTE_OFFLINE` are skipped
    entirely: not counted, not listed (472-473).
  - `FILE_ATTRIBUTE_RECALL_ON_OPEN` is defined (32-34) but **not used** in the mask, so such files are counted.
  - Directories are not checked for any cloud attribute. Whether a cloud placeholder *directory* (OneDrive Files
    On-Demand) reports `FILE_ATTRIBUTE_REPARSE_POINT` to this process depends on the process's placeholder
    compatibility mode; **unverified**. If it does, line 468 skips the whole OneDrive tree, including fully local
    files. If it does not, enumerating a not-yet-populated cloud directory may make the sync provider fetch its
    listing (not file data). Test both on a fixture.
  - DiskMap never opens or reads any file, so no file *data* is ever recalled.
- **Hard links.** Not detected. Every link is counted at full size in every folder it appears in (no FileId or
  link-count check). `C:\Windows\WinSxS` hard-links much of `System32`, so Windows totals and the root total are
  inflated. The drive bar hides this by clamping "Mapped" to drive used (988), which also pushes "Not reachable"
  toward 0 B. The progress fraction can hit its 0.99 cap early for the same reason.
- **Sizes** are logical (`nFileSize`), never allocation size: compressed and sparse files are overstated, small
  files understated (no cluster rounding). Combined with the above, the root total is not comparable to drive used.
- **Long paths.**
  - Enumeration uses `\\?\X:\...` built by concatenation (550-551, 504); path length is unbounded (`std::wstring`).
    No `GetFullPathNameW` is needed for a drive root, and the scan never takes a user path.
  - `wchar_t ln[MAX_PATH + 1]` (463, 491) holds one lower-cased name component; `cFileName` is at most MAX_PATH, so
    nothing is truncated in practice. `Lower` would truncate at 260 chars anyway (353).
  - `g_curPath[1024]` (189) and `cur[1024]` (1225): the progress display truncates paths over 1023 chars (450). Display only.
  - Hover/status text buffer is 2048 (1300); `extra[160]` (1265). `_snwprintf` truncation is handled by forcing a final NUL (1291, 1308).
  - Paths given to Explorer are plain `C:\...` without `\\?\` (330-345, 1393-1401). For paths over MAX_PATH, Explorer
    may fail to select/open; **unverified**, and the result is unchecked either way.
  - `RelaunchAdmin` uses `wchar_t exe[MAX_PATH]` with no retry and no return check (1406).
- **Access-denied folders.** If `FindFirstFileExW` fails (access denied or anything else), the folder is recorded
  with no files and no children, silently (455-462, 487). There is no count or list of skipped folders, no error
  text; the shortfall shows only as "Not reachable (protected/system)" at the root. `GetLastError` is never read.
- **Deep recursion.** `ScanDir` recurses once per folder level. Each frame holds a `WIN32_FIND_DATAW` (~592 B), two
  `ln` arrays (2 × 522 B), a `subs` vector and locals: roughly 2 KB+. The thread reserves a 256 MB stack
  (`CreateThread(NULL, 256u << 20, …, STACK_SIZE_PARAM_IS_A_RESERVATION)`, 668), which allows on the order of
  100,000 levels. There is no explicit depth limit and no guard. Reparse directories are skipped, so link cycles
  cannot cause infinite recursion. `CreateThread`'s return is not checked: on failure the UI stays in "scanning" forever.
- **Cancel mid-scan.** Flag checked at `ScanDir` entry (446), after each `FindNextFileW` (485) and before each sub-folder (493).
  Latency is one directory entry or one sub-folder entry. Partial data is discarded and the UI goes idle (1517).
- **New scan while scanning** (drive button). `StartScan` sets cancel and waits with `INFINITE` **on the UI thread**
  (650-655), so the window stops painting until the worker unwinds. Pending `WM_APP_SCANDONE` is then purged.
- **Rescan** (button or F5). Same as a new scan of the same letter: data cleared, view reset to root, filter reset to
  none or "all safe" depending on the tab, tab kept (656-662). The drive list (and so used/total for the progress
  bar) is the one from the previous refresh.
- **Close during a scan.** `WM_DESTROY` sets cancel and waits at most 5 s, then posts quit with the thread possibly
  still running; the handle is never closed (1525). Process exit kills the thread.
- **Threading.** The worker writes `g_files/g_dirs/g_names` without locks; the UI reads them only in `ST_DONE`, which is
  set after the worker is joined (1515-1518). Progress counters are `volatile LONG64` read with plain loads (1211).
  The current path is guarded by `g_curLock`.
- **Memory.** Arrays reserve 1 M files, 128 K dirs, 16 M name chars up front (663-665). Indices and name offsets are `u32`.
- **DPI.**
  - System DPI only: `<dpiAware>true</dpiAware>` in the manifest; `g_dpi = GetDeviceCaps(screen DC, LOGPIXELSY)` once at startup (1534).
    No `SetProcessDPIAware` call (contrary to the blanket note in HISTORY.md).
  - No `WM_DPICHANGED`; fonts and metrics never change; on a monitor with another DPI Windows bitmap-stretches the window.
  - Unscaled pixel constants: 6-px folder minimum (763), 3-px inner minimum (785), 3-px leaf gap and 40-px² gradient
    threshold (736-737), 30-px² lump threshold (791), 1-/2-px outlines and inflate (1329-1333), 1-px insets (767, 781), 2-px logo gaps (887-889).
- **Idle-screen centring** off with more than 8 drives (1248). Toolbar drives beyond the width limit are just not shown (904).
- **Lump at top level** has no folder to drill into (`topKind` 255, 838), so clicks do nothing and no accent outline appears.
- **Hover after a filter change**: `RenderMap` resets hover (859) at the next paint, so the outline disappears until the mouse moves.

---

## 5. Rule violations and contract gaps

Each item: what, where, and what the port needs. "Known" items from the spec are marked confirmed or corrected.

### Hard rule 4 (elevation)
Rule 4 changed while this file was being written (the main checkout's CLAUDE.md, 2026-11-11): every exe, suite and
standalone, is `requireAdministrator`; there is no non-admin mode and no elevation relaunch; if the process somehow
runs unelevated (e.g. Wine), say so in the UI and degrade. The items below are judged against that rule.
1. **Confirmed.** `IsUserAnAdmin()` (1535) instead of `GetTokenInformation(TokenElevation)`. Port: `IHost::IsElevated()`.
2. **Confirmed, and now removed entirely.** "Run as admin" button (927-931) and `RelaunchAdmin` via
   `ShellExecuteW(…, L"runas", exe, NULL, …)` (1405-1409): no `--elevated`, `ERROR_CANCELLED` indistinguishable
   (plain `ShellExecuteW`), failures silent, drive argument not forwarded. Under the new rule there is no relaunch at
   all, so the port drops the button, `ID_ADMIN` and `RelaunchAdmin`. This is a deviation from parity the lead should list.
3. **Confirmed (moot once item 2 goes).** `wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH)` with no loop and no check (1406).
4. `app.manifest` line 5 is `asInvoker`. New rule: `requireAdministrator` (generated manifest).
5. The non-admin texts become the "running unelevated, degraded" message only: idle tip (1244-1247), status line
   "Running as standard user \x2014 some system folders can't be read" (1316), safe-tab hint (1106), title suffix
   "(Administrator)" (1553). The port needs one degraded-mode notice instead, with no relaunch offer; the
   "(Administrator)" title suffix is pointless when admin is always on.
6. Docs out of step (for the lead): `docs/ARCHITECTURE.md` (lines 110-112, worktree copy) still gives standalone exes
   `asInvoker` for `Admin::None/Optional`, and `docs/MODULES.md` lists diskmap as `Admin::None`; both predate the new rule 4.

### Hard rule 1 (reparse points, cloud files, hard links) — DiskMap moves nothing, so the `core/fs` rules do not strictly bind it, but the scan should follow them
7. **Confirmed.** Only reparse *directories* are skipped (468); reparse *files* are counted (467-484) and the tag in
   `dwReserved0` is ignored. Port: treat every reparse entry as a leaf, skip or count it explicitly by tag
   (e.g. count a WOF/dedup file, show a symlink as 0 B), never traverse.
8. **Confirmed, with a correction.** `RECALL_ON_OPEN` is defined (32-34) but missing from the skip mask (472). Since
   DiskMap never opens files, this is a counting error (placeholders counted as if local), not a recall risk.
   The real recall-adjacent risk is directories: no cloud attribute is checked on directories at all (467-469).
   Port: check `RECALL_ON_OPEN | RECALL_ON_DATA_ACCESS | OFFLINE` on files and directories; do not enumerate into a
   directory with `RECALL_ON_OPEN`; report how much was skipped.
9. Hard links are double-counted (section 4). The rule ("same (volume serial, FileId) = hard link") is a `core/fs`
   rule, but the port should use it here too: `FindFirstFileExW` does not give FileId, so this needs
   `NtQueryDirectoryFile`/`GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` on the directory handle (no
   per-file open), counting a FileId only once. **Decision for the lead** (approved deviation): totals will drop,
   e.g. for WinSxS.
10. Not a violation (no file data read): DiskMap never opens a file, so it cannot trigger a hydration of file contents.

### Hard rule 7 (honest claims)
11. "Not reachable (protected/system)" (998) is not what the number is: it is drive used − min(mapped, used), which also
   includes skipped reparse folders, skipped cloud files, NTFS metadata, and is shrunk by hard-link double counting.
   Port: label it truthfully (e.g. "Not mapped") with an explanation, and fix the hard-link count first.
12. The progress percentage (1212, 1220) is an estimate (logical bytes ÷ allocated bytes, capped 99 %), shown as a
    plain percentage. Rule 7: never present an estimate as a measurement. Port: label it as approximate, or drop the number.
13. The safe-to-delete descriptions (69-77) are claims shown to the user, and the rules behind them are broad:
    `users\*\appdata\**\cache` (93) flags any folder named `cache` under AppData as rebuildable; `amd` and `nvidia`
    at any drive root (95) flag the whole folder as installer leftovers. Port: send the table and descriptions to
    `kb-auditor`; consider giving each rule a basis.

### Code standards (CLAUDE.md)
14. **UI lists**: the sidebar lists have no wheel scrolling, no scrollbar, no column-header sorting, no keyboard
    navigation, no focus rects, no high-contrast fallback (1120-1155, 1505-1509; no `WM_MOUSEWHEEL`). Port: `core/ui` list view.
    `docs/HISTORY.md` describes "a sortable folder list"; the legacy has none (see "Surprises" below).
15. **Worker**: raw `CreateThread` (668), return unchecked; UI-thread `WaitForSingleObject(INFINITE)` (652, 1515);
    5-s bounded wait then exit with the thread alive (1525). Port: `core/sys` `Worker`; `Shutdown()` must not
    return while the worker is alive; never block the UI waiting for cancel.
16. **Error handling**: no Win32 failure is surfaced anywhere. Unchecked: `FindFirstFileExW` failures (455-462),
    `ShellExecuteW` (1395, 1400, 1407), `CreateThread` (668), `RegisterClassExW` (1548), `CreateWindowExW` (1554),
    `DwmSetWindowAttribute` (1558), `GetModuleFileNameW` (1406), GDI object creation. No crash minidump handler.
17. **DPI**: confirmed system-DPI only (manifest line 7, 1534) — note it is the manifest, not `SetProcessDPIAware`.
    Port: per-monitor V2, `OnDpiChanged` rebuilds fonts and the map bitmap; scale the unscaled constants in section 4.
18. **Thousands separator and decimal point** hard-coded (`,` 249; C-locale `.` 261). Consistent with "English only"; keep or move to a core formatter.
19. **Long paths**: compliant for the scan. `GetModuleFileNameW` loop missing (1406, see 3).

### ARCHITECTURE.md module contract
20. **Confirmed.** Window class `L"DiskMapWnd"` (1546). Port: `Hadron.diskmap.<name>`.
21. Own top-level window (`CreateWindowExW(… WS_OVERLAPPEDWINDOW …, NULL parent)`, 1554), own message loop
    (1568-1569), centring and min size (1472-1476, 1550-1556), dark title bar (1557-1558), icon (1544-1545).
    Port: `Module::Create(IHost&, parent, rect)` creates a child; the host owns the frame, min size, title bar and icon.
22. `PostQuitMessage(0)` in `WM_DESTROY` (1526) and `PostMessageW(g_hwnd, WM_CLOSE)` after relaunch (1408). Modules
    never call `PostQuitMessage`; the relaunch is the host's.
23. `WM_APP_SCANDONE = WM_APP + 1` (36) is in the host's range. Port: `WM_APP + 0x100` or above.
24. Title set with `CreateWindowExW` text (1553-1554). Port: `IHost::SetTitle`. The status bar is drawn by the module
    itself with rich hover text (1294-1319); whether it maps to `IHost::SetStatus` or stays a module-drawn strip is
    a decision for the lead (see Ambiguities).
25. All state is global (172-236). Port: members of the module instance. Startup I/O (`RefreshDrives` 1538,
    `IsUserAnAdmin` 1535, fonts 1536) must move out of the constructor ("constructors do no I/O") into `Create`.
26. No `RunCli`, `--selftest` or `--screenshot --fixture`. The legacy can only scan a drive root by letter (544, 1563-1565);
    the port needs a folder scan to load fixtures, and the safe rules are drive-root-relative (80), so a fixture root
    must be treated as a drive root for the rules to fire.
27. Command line: the bare drive letter (1563-1566) does not fit the shared table; any argument starting with a letter
    triggers a scan. Port: pass it through `RunCli`/an explicit option (e.g. `--drive D`) and validate it against the drive list.
28. Keyboard: `WM_KEYDOWN` reaches the module only when its HWND has focus. In the suite, Backspace/Escape/F5/Home
    should go through focus or `PreTranslateMessage`; none collide with the host's Ctrl+Tab / Ctrl+1..9.
29. Wheel: the host forwards `WM_MOUSEWHEEL`; the legacy ignores it, so the port's lists must handle it.
30. Resources: `res.rc` owns `ICON 1` and `RT_MANIFEST` id 1 (res.rc lines 1-2) and has no VERSIONINFO; the manifest
    is hand-written (no PerMonitorV2). Port: generated exe `.rc` and manifest; module fragment uses `DISKMAP_` names only.
31. COM: `ShellExecuteW` is called without COM init by the module (fine); the host initialises COM.

### Ambiguities for the lead
- `Admin` level for diskmap and the asInvoker rows in ARCHITECTURE.md vs the new rule 4 (items 2-6); dropping "Run as admin" is a parity deviation.
- Status bar: module-drawn hover strip vs `IHost::SetStatus` (item 24).
- Hard-link de-duplication and reparse-file counting change totals; they need to be listed as approved deviations (items 7-9).
- Escape currently means Up, not Cancel scan; Cancel on Escape would be a deviation.
- Whether OneDrive placeholder folders are skipped by line 468 is unverified (section 4).

### Surprises and contradictions with docs/HISTORY.md
- HISTORY.md says DiskMap "shows a treemap and a sortable folder list". There is no sortable list and no folder list:
  the sidebar has a fixed, unsortable top-14 "LARGEST FILES" / "BIGGEST SPOTS TO CLEAN" list and per-category rows sorted by size.
- HISTORY.md says all tools use `SetProcessDPIAware`; DiskMap uses the manifest `dpiAware` instead. Same effect (system DPI).
- DiskMap has a whole "Safe to delete" feature (9 reasons, 28 rules) that HISTORY.md doesn't mention. It deletes
  nothing, but its claims fall under rule 7.
- `.ts` files are classified as Video before Developer.
