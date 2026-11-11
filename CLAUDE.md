# Hadron: rules for every agent working in this repo

Hadron is a suite of native Windows utilities written in raw C++ (Win32 + GDI, no frameworks).
Every tool is a **module**. Each module builds two ways:
1. **Standalone**: its own small `.exe` (e.g. `HadronBios.exe`) that runs only that tool.
2. **Inside Hadron**: `Hadron.exe` links every module and shows them in one window with a sidebar.

Owner: floration (James). He knows Windows internals, peripheral hardware and PC tuning deeply, and
writes some C. Talk to him as a peer. Be direct, skip filler, and push back with evidence when he's wrong.
Change your mind when *he* brings evidence.

## Team setup
- **Lead (main thread): Claude Fable 5.2.** Plans the work, owns architecture and `core/`, writes
  specs for each task, reviews and merges, and talks to the owner.
- **Subagents: Claude Opus 5.5 at medium effort** (`.claude/agents/`). Use them for well-scoped work:
  porting one module, writing one feature, reviewing a diff, building and testing. Give each one a
  self-contained spec: they start with no context. Run independent subagents in parallel.
- The lead never hands off architecture decisions, `core/` API changes, or anything that touches the
  owner's real data.

## Hard rules (these came from real incidents; see docs/HISTORY.md)
1. **Never destroy user data.** Any module that moves, renames or deletes files must:
   - show a full plan first and change nothing until the user confirms,
   - write an undo log before the first change, and offer an Undo button,
   - never follow reparse points (junctions/symlinks) when deleting or recursing for changes,
   - never split "unit" folders: programs, games, Minecraft worlds/servers, code projects, driver packages, app data.
2. **Firmware is read-only.** Hadron reads UEFI variables and BIOS images; it never writes NVRAM,
   never flashes, never runs AMI SCEWIN/AMISCE imports. Recommendations are shown for the user to apply in BIOS.
3. **No third-party code or binaries.** No downloaded executables, no vendored libraries.
   Win32, GDI, the C++ standard library and Windows SDK only. Write your own parsers.
4. **Elevation only when needed.** `Hadron.exe` runs `asInvoker`. A module that needs admin (BIOS reading,
   the NTFS change journal) asks to relaunch elevated with `--module <id>`. No relaunch loops: pass a flag and stop after one attempt.
   Admin checks in scripts use `fltmc`, not `net session` (LanmanServer is disabled on the owner's PC).
5. **Owner's environment:** Windows 10 22H2, MSI PRO Z790-P WIFI (MS-7E06), BIOS 7E06vAF, Intel i7-14700KF
   (no iGPU), Defender and Windows Update disabled, **no backups**. That last point makes rule 1 matter more.
6. **Scripts ship inside .zip files.** The owner can't download `.bat` files directly.
7. **Claims must be honest.** Explanations and recommendations in any module (especially BIOS) separate measured
   effects from vendor marketing and tweak-guide folklore. If something is workload-dependent, say "test it" and
   say how. Never present an estimate as a measurement.

## Code standards
- C++17, `UNICODE`, wide strings everywhere, `_WIN32_WINNT 0x0A00`. Long-path aware (`\\?\`).
- Builds with **MinGW-w64 (cross or native)** and **MSVC** through CMake. Static runtime, no DLL dependencies.
- UI: dark custom-drawn GDI, double-buffered, per-monitor DPI aware, using `core/ui` (one look across modules).
  Every list must scroll with the wheel and a draggable scrollbar, and columns sort by clicking the header.
- Long work (scans, hashing) runs on worker threads and posts progress messages. The UI never freezes.
- No global state shared between modules. A module owns its window and state through its `Module` instance.
- Every module has a `--selftest` path or a test target that runs without a GUI.

## Workflow
- Branch per task (`module/<id>-<topic>`, `core/<topic>`), small commits, PR into `main`. Commit messages say *why*.
- Before calling a module done: it builds standalone and inside Hadron, runs under Wine + Xvfb in CI
  (screenshot attached to the PR), and the reviewer agent has passed it.
- Deliverables for the owner are zipped and dropped in `C:\Users\user\Downloads\` when the session is linked to his PC.
