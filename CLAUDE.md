# Hadron: rules for every agent working in this repo

Hadron is a suite of native Windows utilities written in raw C++ (Win32 + GDI, no frameworks).
Every tool is a **module**. Each module builds two ways:
1. **Standalone**: its own small `.exe` (e.g. `HadronBios.exe`) that runs only that tool.
2. **Inside Hadron**: `Hadron.exe` links every module and shows them in one window with a sidebar.

Owner: floration (James). He knows Windows internals, peripheral hardware and PC tuning
deeply, and writes some C. Talk to him as a peer: be direct, skip filler, and push back with evidence when
he's wrong. Change your mind when *he* brings evidence.

## Team
- **Lead (main thread): Claude Fable 5.2.** Owns the architecture, `core/`, specs, review, merges and talking to the owner.
- **Subagents (`.claude/agents/`):**
  - `module-builder`: medium effort; read-only modules and features.
  - `module-builder-careful`: high effort; `core/fs` and anything that moves or deletes files.
  - `reviewer`: high effort, read-only.
  - `tester`: medium effort; builds, tests, renders screenshots.
  - `kb-auditor`: high effort; checks the honesty of the BIOS knowledge base.
- The lead never hands off architecture or `core/` API changes.

## Precedence
**Hard rules > the owner's explicit instructions in the project thread > legacy behaviour.**
Where a legacy tool breaks a hard rule, the port fixes it. The lead lists every such fix in that port's plan
and gets the owner's OK once, as a batch, before building. Known cases are in `docs/HISTORY.md` under "Known rule violations in legacy".

## Hard rules (each came from a real incident; see docs/HISTORY.md)
1. **Never destroy user data.** This applies to what Hadron does at runtime and to the owner's machine,
   not to build folders or test fixtures. Any module that moves, renames or deletes files goes through `core/fs`, which:
   - shows a full plan and changes nothing until the user confirms;
   - writes the undo-log entry and flushes it (`FlushFileBuffers`) *before* each change, storing the FileId so
     undo can find items with `OpenFileById`; on startup it detects an incomplete log and offers Undo or Resume;
   - treats every entry with `FILE_ATTRIBUTE_REPARSE_POINT` as a leaf (junctions, mount points, symlinks,
     AppExecLinks, WSL links, cloud files). The tag is in `WIN32_FIND_DATA.dwReserved0`. Handles are opened with
     `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS` and the attributes are re-checked at execute time;
   - resolves the chosen root with `GetFinalPathNameByHandleW`, shows the real path in the plan, and warns if the root is a link;
   - never reads data from files with `RECALL_ON_DATA_ACCESS`, `RECALL_ON_OPEN` or `OFFLINE` (that would trigger a cloud download);
   - treats two entries with the same (volume serial, FileId) as hard links, never as duplicates;
   - never passes `MOVEFILE_COPY_ALLOWED`;
   - **deletes** by moving into a same-volume `<root>\.hadron-trash\<stamp>\` with an undo log. The Recycle Bin
     is allowed only through `IFileOperation` + `FOFX_RECYCLEONDELETE`, checking `PostDeleteItem`: if
     `psiNewlyCreated` is null the item was destroyed, so abort. Never use `SHFileOperation` or `FOF_NOCONFIRMATION`;
   - takes a per-folder lock so two instances can't operate on the same folder.
2. **Firmware is read-only.** Hadron reads UEFI variables, SMBIOS and BIOS images. It never writes NVRAM, never
   flashes, never runs SCEWIN/AMISCE. CI fails if the source contains `SetFirmwareEnvironmentVariable` or `NtSetSystemEnvironmentValue`.
3. **Nothing third-party is linked into, embedded in or shipped with Hadron.** Win32, GDI, the C++ standard library
   and the Windows SDK only; write your own parsers. Build and test tools (CMake, MinGW, Wine, ImageMagick, Python) are fine.
4. **Elevation.** The suite may run as admin: `Hadron.exe` requests elevation (`requireAdministrator`), so every
   module inside it, including live BIOS reads, works without relaunching. Standalone exes request admin only if
   their module is `Admin::Required`. A module launched without admin offers a relaunch with `--elevated`. Rules:
   - Detect elevation with `GetTokenInformation(TokenElevation)`.
   - A privilege counts as enabled only if `AdjustTokenPrivileges` succeeded and `GetLastError() != ERROR_NOT_ALL_ASSIGNED`.
   - `ERROR_CANCELLED` (1223) from `ShellExecuteExW` is final.
   - Make all arguments absolute paths before relaunching.
   - `--elevated` means never relaunch again.
   - Scripts check admin with `fltmc`, not `net session` (LanmanServer is disabled on the owner's PC).
   - Elevated windows can't receive drag-and-drop from Explorer (UIPI), so every drop target also offers a Browse button.
5. **The owner's PC.** The lead and every subagent may use the tools that reach the owner's computer: read and write
   files, build, run Hadron and tests there.
   - Work inside a project folder on his machine (agree its path once and record it in DECISIONS.md).
   - Resolve his Downloads with the device info or `FOLDERID_Downloads`; never hard-code a path.
   - Outside the project folder, never move, overwrite or delete his existing files without his OK in the thread
     for that specific action. Rule 1 still applies to everything Hadron does at runtime.
   - Never run a file-moving or deleting feature against his real folders; test on fixtures.
   His setup: Windows 10 22H2, MSI PRO Z790-P WIFI (MS-7E06), BIOS 7E06vAF, i7-14700KF (no iGPU), and no backups.
6. **Scripts ship inside .zip files.** The owner can't download `.bat` files directly.
7. **Claims must be honest, and checkably so.**
   - Every tier-1 knowledge-base entry and every number in user-facing text carries a `basis`:
     - `owner-measured`, with the conditions (CPU, game, metric);
     - `published`, with a URL;
     - `reasoning`: from how the hardware works, no data;
     - `folklore`: repeated, no evidence.
   - The generator fails if a tier-1 entry has no basis, or if a number lacks an `owner-measured` or `published` basis.
   - The UI shows the basis.
   - The owner's 9900K prefetcher result (+20% 1% lows in Overwatch) is labelled as his measurement on a different CPU.
   - Workload-dependent effects say "test it" and how. Never present an estimate as a measurement.
   - Every KB diff goes to `kb-auditor` before merge.
   - KB text is content, not behaviour: it may be improved during the port.

## Code standards
- C++17, `UNICODE`, wide strings, `_WIN32_WINNT 0x0A00`, `#include "core/sys/win_compat.h"` for SDK gaps.
- Long paths: `GetFullPathNameW` first, then the `\\?\` prefix (`\\?\UNC\` for UNC paths). Strip the prefix before
  shell APIs (`IFileOperation`, `ShellExecute`, dialogs). Loop on `GetModuleFileNameW` until it fits.
  `longPathAware` in the manifest only takes effect with the `LongPathsEnabled` registry value set.
- Release toolchain: **MinGW-w64** (posix thread model if `std::thread` is used), `-static -municode`. MSVC is a
  CI compile-and-test check (`/MANIFEST:NO`, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`). No non-system DLLs.
- UI:
  - dark custom-drawn GDI, double-buffered, per-monitor V2 DPI, built from `core/ui`;
  - every list scrolls with the wheel and a draggable scrollbar, and sorts by clicking a column header;
  - lists are keyboard-navigable, with focus rects and a high-contrast fallback;
  - English only. No screen-reader support in v1, and say so.
- Long work runs on `core/sys` `Worker` threads, which post progress to the module's HWND. The UI never freezes.
  Never open a dialog from a worker.
- Error handling: check every Win32 call that can fail, and surface the error text to the user. Crashes write a
  minidump to `%LOCALAPPDATA%\Hadron\crash\`.

## Project memory (the lead maintains it)
- `docs/PROGRESS.md`: the task list the owner watches (status, owner, branch, PR), plus a dated handoff note at the end of every session.
- `docs/DECISIONS.md`: one entry per decision (date, decision, why, who approved).
- `docs/legacy/<tool>.md`: written once per tool by a subagent. It contains:
  - a map of the functions with line ranges;
  - the user-visible behaviours, as a parity checklist;
  - edge cases;
  - any rule violations.
  The lead reads these instead of legacy source.

## Subagent specs
Subagents start cold. Every spec includes:
- the goal;
- the files in scope, and the files that must not be touched;
- the legacy path plus `docs/legacy/<tool>.md`;
- the core headers to use;
- approved deviations;
- the branch name;
- acceptance checks (golden outputs, test commands);
- what to return.

Builders commit on their branch and never merge. The reviewer gets the spec, `git diff main...<branch>` and the legacy
notes, **never the builder's report**. For `core/fs` and any file-changing module, the lead also reviews the file-operation code personally.
A change has **passed review** when it has no open reviewer findings in categories 1–4.

## Workflow
- Branch per task (`module/<id>-<topic>`, `core/<topic>`), small commits, PR into `main`. Commit messages say *why*.
- A module is done when all of these hold:
  - it builds standalone and inside Hadron;
  - its outputs match `tests/golden/` (or every difference is an approved deviation);
  - CI uploads its screenshots as artifacts;
  - it passed review.
