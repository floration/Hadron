# What exists so far (Nov 2026)

Everything in `legacy/` was built in one long session before Hadron existed. Each tool is a single-file Win32 + GDI
program built with MinGW: `x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows -static -s`.
They work, and the owner uses them. Porting them to the module contract is the first job.

| Legacy tool | Folder | What it does | Notes for the port |
|---|---|---|---|
| DiskMap | `legacy/diskmap` | Drive-space visualiser: scans a drive, shows a treemap and a sortable folder list | Read-only. Worker thread + treemap; the first port and the proof of the contract |
| BIOS Settings Explorer | `legacy/bios-explorer` | All 5,368 setup settings of the owner's BIOS (vAF): live values, defaults, explanations, per-goal advice | Details below. `Admin::Optional` |
| UEFI dump | `legacy/uefi-dump` | Read-only PowerShell dump of every UEFI variable to a zip | Becomes "Save snapshot" / "Compare snapshots" in `bios`. Retire the script after that (it uses `net session`) |
| OrganizerRescue | `legacy/organizer-rescue` | Restores file moves by reading the NTFS change journal (USN) | Built to undo the old Organizer tool's moves. `rescue_core.h` is platform-independent and unit-tested. Status: waiting on the owner (see MODULES.md) |

The old Organizer (sorts files into category folders) and SaveFinder (finds game saves in a Steam library) were
dropped from Hadron by the owner. They are not in this repo.

## BIOS Settings Explorer in detail
Pipeline (`legacy/bios-explorer/pipeline`, Python 3, no dependencies):
1. `unpack.py`: parses the 32 MB SPI image (`E7E06IMS.AF0` from MSI; **not committed**): firmware volumes, FFS files, sections, LZMA. 563 modules.
2. `ifr.py`: reads the HII string and form packages of the Setup module (GUID 899407d7-...) and parses the IFR
   opcodes into questions, options, defaults, varstores, SUPPRESS_IF/GRAY_OUT_IF and form refs.
3. `builddb.py`: dedupes by (varstore, offset, width), computes menu paths and visibility, and decodes a UEFI dump.
   Real defaults come from AMI's `StdDefaults` NVRAM store. The IFR defaults were wrong for about 95 settings.
4. `kb.py`, `kb_impact.py`, `kb2.py`: hand-written knowledge base.
   - Tier 1: about 57 in-depth entries covering 501 settings. Each has per-goal advice (Power saving / Balanced /
     Single-core / Multi-core / Low latency), the effect in FPS titles, other games and apps, and a claim check.
   - Tier 2: name families and menu-level entries. All 5,368 settings are covered.
   - **The numbers in the KB have no `basis` yet.** Adding a basis is part of the port (CLAUDE.md rule 7).
5. `gen.py`: writes `settings.db` (tab-separated UTF-8, `V`/`T`/`K`/`S` lines), which the app embeds as RCDATA.

App (`legacy/bios-explorer/app/bsx.cpp`):
- Reads live variables after enabling `SeSystemEnvironmentPrivilege`, or loads a dump folder.
- Search; filters (All / Key settings / Changed from default / Hidden / Differs from goal); goal selector.
- Sortable columns, draggable scrollbars, a detail pane, and an "Export changes" checklist.
- Verified against the owner's dump: 150 variables, and all varstore sizes match.

The owner's BIOS state (from his dump):
- Fixed 53x all-core at 1.25 V override; CPU Cooler Tuning = MSI Unlimited.
- Off: HT, Turbo, EIST, Speed Shift, C-states, both prefetchers, ReBAR, Above 4G. CSM is on.
- These are deliberate low-latency choices. The app advises; it doesn't nag.

## Known rule violations in legacy (fix them in the port; get the owner's OK once, as a batch)
- `bsx.cpp`:
  - fixed 64 KB variable buffer;
  - window class `L"BSX"`;
  - `requireAdministrator` (should be `Admin::Optional` + asInvoker);
  - "Export changes" writes next to the exe (should use a Save dialog);
  - the CSV parser ignores `""` escapes;
  - no SMBIOS version guard (the BIOS-version warning lives only in README.txt).
- `uefi-dump/Run-Dump.bat`: uses `net session`, which is incident 3.
- `rescue.cpp`:
  - moves files with no undo log and no reparse checks;
  - writes `_organizer_rescue_preview.txt` to the drive root;
  - `Long()` (line ~96) turns UNC paths into `\\?\\\server`;
  - `GetModuleFileNameW` is called with `MAX_PATH`.
- All tools: system-DPI only (`SetProcessDPIAware`); per-monitor V2 is new work.

## Incidents and the rules they produced
1. **The old Organizer scattered a Minecraft server.** It sorted the server's files into category folders by type,
   which broke the server. Repaired with scripts and OrganizerRescue. Rule: plan, confirm, undo log (`core/fs`).
2. **A cleanup deleted most of C:\ProgramData.** Deleting an old Windows install on H:\ followed symlinks
   (`H:\Users\All Users` -> `C:\ProgramData`), and `robocopy /XJ` doesn't skip symlinks. Rule: reparse points are
   leaves; never traverse links.
3. **A .bat elevation loop spawned endless cmd windows.** `net session` fails because LanmanServer is disabled.
   Rule: use `fltmc` in scripts, `TokenElevation` in code; relaunch once at most.
4. **SCEWIN doesn't work on this board.** The 5.x and "for Intel" builds fail with HII export error 4 and platform
   error 49. That's why Hadron parses the BIOS image itself.
5. **Overstated advice was corrected by the owner, repeatedly.** Examples:
   - prefetchers "always help";
   - C-states off "degrades the CPU";
   - Speed Shift mattering under a fixed ratio;
   - snoop-latency settings called "hidden plumbing" when 282 of 322 are visible.
   Rule 7: every claim carries a basis; `kb-auditor` reviews them.
