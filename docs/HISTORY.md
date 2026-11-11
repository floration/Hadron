# What exists so far (Nov 2026)

Everything in `legacy/` was built in one long session before Hadron existed. Each is a single-file Win32 + GDI
program built with MinGW: `x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static -s`.
They work and the owner uses them. Porting them to the module contract is the first job.

| Legacy tool | Folder | What it does | Notes for the port |
|---|---|---|---|
| DiskMap | `legacy/diskmap` | Drive space visualiser: scans a drive and shows a treemap plus a sortable folder list | Scanning on a worker thread; reuse its treemap renderer in core/ui later |
| Organizer | `legacy/organizer` | Sorts loose files in a folder into category subfolders, with a preview, per-item skip, undo log | See incident 1. Unit-folder detection (games, Minecraft, code projects, drivers, app data) is the important part |
| OrganizerRescue | `legacy/organizer-rescue` | Puts back files Organizer moved by reading the NTFS change journal (USN), no undo log needed | `rescue_core.h` is platform-independent and unit-tested (`test_core.cpp`); needs admin |
| SaveFinder | `legacy/savefinder` | Before deleting a Steam library, finds saves, worlds and personal data inside game folders | Read-only |
| BIOS Settings Explorer | `legacy/bios-explorer` | Lists all 5,368 setup settings of the owner's BIOS (vAF), live values, defaults, explanations, per-goal advice | Details below; needs admin |
| UEFI dump | `legacy/uefi-dump` | Read-only PowerShell dump of every UEFI variable to a zip (`Dump-UEFI.ps1`, `Run-Dump.bat`) | Should become part of the BIOS module ("Save snapshot") |

## BIOS Settings Explorer in detail
Pipeline (`legacy/bios-explorer/pipeline`, Python 3, no dependencies):
1. `unpack.py`: walks the 32 MB SPI image (`E7E06IMS.AF0` from MSI's site; **not committed**). Parses firmware
   volumes, FFS files and sections (LZMA via the stdlib), and writes the modules to a pickle (563 modules).
2. `ifr.py`: finds HII string packages (en-US) and form packages in the Setup module (GUID 899407d7-...),
   and parses the IFR opcodes into questions: ONE_OF / NUMERIC / CHECKBOX, options, defaults, varstores,
   SUPPRESS_IF / GRAY_OUT_IF, and form references.
3. `builddb.py`: dedupes questions by (varstore, offset, width), computes menu paths and visibility
   (always hidden / conditional / visible), and optionally decodes a UEFI dump. Real defaults come from AMI's
   `StdDefaults` NVRAM store, not from the IFR defaults, which were wrong for about 95 settings.
4. `kb.py`, `kb_impact.py`, `kb2.py`: hand-written knowledge base.
   - Tier 1: about 56 in-depth entries (501 settings). Each has an explanation, per-goal recommendations
     (Power saving / Balanced / Single-core / Multi-core / Low latency), real-world effect in FPS titles,
     other games and apps, and a claim check.
   - Tier 2: name families and menu-level entries.
   Every one of the 5,368 settings is covered.
5. `gen.py`: packs everything into `settings.db` (tab-separated UTF-8; `V`/`T`/`K`/`S` lines), which the C++ app embeds as RCDATA.

App (`legacy/bios-explorer/app/bsx.cpp`):
- Reads live variables with `GetFirmwareEnvironmentVariableExW` after enabling `SeSystemEnvironmentPrivilege`.
  Can also load a dump folder.
- Features: search, filters (All / Key settings / Changed from default / Hidden / Differs from goal), goal
  selector, sortable columns, draggable scrollbars, a detail pane, and "Export changes" to a text checklist.
- Verified against the owner's dump: 150 variables, all varstore sizes match exactly.

Known facts about the owner's current BIOS state (from his dump):
- Fixed 53x all-core at 1.25 V override, CPU Cooler Tuning = MSI Unlimited.
- Hyper-Threading, Turbo, EIST, Speed Shift, C-states, both prefetchers, ReBAR and Above 4G are off. CSM is on.
- These are deliberate low-latency choices. He measured +20% 1% lows in Overwatch on a 9900K with the
  prefetcher off. Respect them; the app advises, it doesn't nag.

## Incidents and the rules they produced
1. **Organizer scattered a Minecraft server.** Files were sorted by type out of a server folder, which broke it.
   Repaired with scripts and OrganizerRescue. Rule: unit folders are never split; plan, confirm, undo log.
2. **A cleanup deleted most of C:\ProgramData.** Deleting an old Windows install on H:\ followed symlinks
   (`H:\Users\All Users` -> `C:\ProgramData`); `robocopy /XJ` doesn't skip symlinks. The Claude MSIX install then
   failed until `C:\ProgramData\Microsoft\Windows\AppRepository\Packages` was recreated.
   Rule: check `FILE_ATTRIBUTE_REPARSE_POINT` before every recursive delete, and never traverse links.
3. **A .bat elevation loop spawned endless cmd windows.** `net session` fails because LanmanServer is disabled,
   so the "am I admin?" check always failed and relaunched.
   Rule: use `fltmc`, pass a relaunch flag, and never relaunch twice.
4. **SCEWIN doesn't work on this board.** Both the 5.x and "for Intel" builds fail (HII export error 4,
   platform error 49). That's why Hadron parses the BIOS image itself.
5. **Overstated advice got corrected by the owner, repeatedly.** Examples: prefetchers "always help",
   C-states-off "degrades the CPU", Speed Shift mattering under a fixed ratio, snoop-latency settings being
   "hidden plumbing" when 282 of 322 are visible.
   Rule: check the data before claiming, and frame workload-dependent effects as "test it".
