# diskmap port — build spec

Lead-owned spec for the `module-builder` subagent. Legacy reference: `legacy/diskmap/diskmap.cpp`
(read-only) and **`docs/legacy/diskmap.md`** (the parity checklist, rules and data tables — read that,
not the legacy source). Status: **pending owner OK** on the deviation batch and the Console-layout
choice (thread decision card). Do not start building until both are confirmed.

## Goal
Port DiskMap onto the Hadron platform as the `diskmap` module: a disk-space visualiser that scans a
drive (or a fixture folder), classifies every file, flags safe-to-delete space, and shows it two ways
— **Baseline**: the squarified treemap + sidebar + breadcrumb; **Console**: a dense, keyboard-first
sortable list over the same scan data (pending owner confirmation of the Console choice). Read-only:
the module never moves, deletes or opens a file's data.

## Files in scope
- `modules/diskmap/module.cpp` — the module (new).
- `modules/diskmap/standalone.cpp` — `wWinMain` → `RunStandalone(MakeDiskMapModule())` (new).
- `modules/diskmap/scan.{h,cpp}` — scan engine + data model, so it is unit-testable headless (new).
- `modules/diskmap/classify.{h,cpp}` — categories, directory-context and safe-to-delete rules (new).
- `core/host/registry.{h,cpp}` — add `MakeDiskMapModule()` and put it in `MakeAllModules()` (edit).
- `CMakeLists.txt` — `hadron_add_module(diskmap NAME HadronDiskMap DESC "...")` (edit).
- `docs/MODULES.md` — set diskmap row to `Required` with the real summary (edit).

**Must not touch:** anything under `core/ui`, `core/sys`, `core/host/host.cpp`, `core/host/module.h`
(the contract is fixed — raise a note if you think it needs a change), other modules, `tests/golden/`,
CI. Use the core as-is.

## Core to build on (do not re-implement)
- `core/sys/sys.h`: `IsElevated`, `EnablePrivilege` (for `SeBackupPrivilege`), `ToExtended`/`ToShell`, `ErrorText`.
- `core/sys/worker.h`: `Worker` — run the scan here; `Shutdown()` cancels then joins; never block the UI.
- `core/sys/win_compat.h`: cloud attrs (`RECALL_ON_DATA_ACCESS`, `RECALL_ON_OPEN`), `HX_*` DWM/DPI.
- `core/ui/theme.h`, `core/ui/draw.h`, `core/ui/widgets.h`: palette (from `IHost::Theme()`), fonts
  (`IHost::Fonts()`), layout (`IHost::Layout()`), and `ListView`/`DetailPane`/`SearchBox`/`ScrollBar`.
- `core/host/module.h`: implement `Module`; draw in `Paint`, handle input in `OnInput`, long work in a
  `Worker`, progress posted to the module HWND (`WM_MODULE_BASE` + n, never `WM_APP+1`).

## Approved deviations from the legacy (pending owner OK — see thread)
1. **Elevation (rule 4):** no "Run as admin" button, no relaunch, no `ID_ADMIN`/`RelaunchAdmin`; always
   `requireAdministrator`. If somehow unelevated (Wine), one degraded notice, no relaunch offer. No
   "(Administrator)" title suffix. Detect with `IHost::IsElevated()`.
2. **Hard-link de-dup (rules 1/7):** count each (volume serial, FileId) once. Use
   `GetFileInformationByHandleEx(FileIdBothDirectoryInfo)` on the directory handle (no per-file open);
   fall back to per-link counting only if the ID is unavailable. Lowers totals vs legacy (e.g. WinSxS).
3. **Reparse points are leaves:** never traverse; read the tag in `WIN32_FIND_DATA.dwReserved0`; show a
   symlink/junction/mount as its reported size (usually 0 B), count a WOF/dedup file at its logical size.
4. **Cloud placeholders:** skip files/dirs with `RECALL_ON_DATA_ACCESS | RECALL_ON_OPEN | OFFLINE`; never
   enumerate into a recall dir; never read file data. Report how much was skipped.
5. **Protected folders:** enable `SeBackupPrivilege` for the scan so admin-readable-only folders size.
6. **Honest labels (rule 7):** "Not reachable" → **"Not mapped"** with a one-line explanation of what it
   includes (hard-link de-dup, skipped reparse/cloud, NTFS metadata). Progress % labelled **approximate**.
7. **Safe-to-delete claims:** the 9 reasons + 28 rules (docs/legacy/diskmap.md §3) go through `kb-auditor`
   before merge; each rule carries a `basis`. KB text may be improved during the port.

## Data model (`scan.h`)
Flat arrays, `u32` indices, one name pool (legacy §1). `FileE{size,nameOff,dir,cat,safe}`,
`DirE{name,parent,file range,kids,recursive totals, per-cat and per-reason sums, own safe reason,
recursive counts, context, dominant cat}`. Add per-(volume,FileId) seen-set for de-dup, and skipped-byte
counters (reparse, cloud, access-denied). Sizes are logical (`nFileSize`), matching the fixture expected.

## Scan (`scan.cpp`, on a `Worker`)
`FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)`, `\\?\` paths built by concatenation.
Reparse/cloud handling per deviations 3–4. Hard-link de-dup per deviation 2. Publish current path +
byte/file counters for progress. Cancel latency ≤ one entry. Finalize: per-dir sums, sort files by size
desc, bottom-up roll-up, dominant category, sort kids by total desc (legacy §1 Finalize). On
access-denied, record the folder empty and add to the skipped tally (surface it, unlike legacy).

## Classification (`classify.cpp`)
Port categories, `DirCtx`, `FileCat`, and the safe-to-delete dir/file rule tables **verbatim** from
docs/legacy/diskmap.md §3 (the glob/`**` matcher, lower-casing with `CharLowerBuffW`, inherited dir
reasons, `g_fileDepth`). Keep `.ts`→Video-before-Developer and the exact name/extension lists. The
fixture root is treated as a drive root so the drive-root-relative rules fire (`--fixture`).

## UI
- **Host-owned chrome:** the module draws only inside its child window; `IHost` owns the frame, title
  (`SetTitle`), min size, dark title bar, icon. Status/hover text goes through `IHost::SetStatus`
  (compose the legacy's rich hover string: `path · size · category` etc.). No module top-level window,
  no `PostQuitMessage`.
- **Baseline:** toolbar (logo, title, drive buttons, Cancel/Rescan — no admin button), left sidebar
  (current view, drive breakdown, Categories/Safe tabs, rows, explanation box, largest list → as a
  `core/ui` `ListView`/`DetailPane`, scrollable + keyboard), breadcrumb, treemap (squarified, cached
  bitmap, hover outline + accent top-level outline), status bar. Colours/metrics from the theme, scaled
  by DPI (scale the legacy's unscaled constants, §4). Parity target: docs/legacy/diskmap.md §2.
- **Console (pending choice — default "Dense list view"):** same scan data shown as a `core/ui`
  `ListView` of the current folder (Name/Type/Size/Modified or %), sortable, wheel + scrollbar + keyboard
  nav + focus rects; breadcrumb above; detail pane for the selection; no treemap. Monospace via the
  Console fonts the host supplies.
- **Keyboard:** Backspace/Escape = Up, F5 = rescan, Home = root (legacy §2). In the suite these arrive
  via `OnInput`/`PreTranslateMessage`; don't collide with host Ctrl+Tab/Ctrl+1..9. Wheel scrolls the
  lists (legacy ignored it).

## Headless modes
- `RunCli --selftest`: scan the fixture (below) and assert the de-duped totals and a few category/safe sums.
- `--screenshot <png> --fixture <dir> [--theme .. --layout ..]`: `LoadFixture` scans `<dir>` synchronously
  (no Worker, deterministic) and `Paint` renders. The host already composes the DIB.

## Acceptance
- Builds standalone (`HadronDiskMap.exe`) and in the suite (`Hadron.exe --solo diskmap`).
- On `tests/fixtures/make_diskmap_tree.py` output, the scan matches `tests/golden/legacy/diskmap-tree-expected.json`
  **with de-dup**: total = `bytes_unique_fileid` = **17,992,005 B** (hard link counted once), the outside
  symlink is a leaf so the 10 MB sentinel is **excluded**, the 382-char long path is handled. (Junction
  case is Windows-CI only.) `--selftest` asserts these.
- Categories and safe-to-delete sums match the rules; `kb-auditor` passes on the KB text.
- `--screenshot` renders Baseline and Console under Wine; CI uploads them. No golden pixel-diff is enforced
  yet (the port looks different from the legacy PNG, which is reference only).
- Passed `reviewer` (no open findings in categories 1–4) and `tester`.

## Return
The branch diff, the parity checklist (docs/legacy/diskmap.md §2) with each item ticked or noted as an
approved deviation, the `--selftest` output, the Wine screenshots (Baseline + Console, a couple of themes),
and any contract/core gap you hit (do not change the core yourself — report it).
