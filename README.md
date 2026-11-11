# Hadron

A suite of small, fast, native Windows utilities: raw C++, Win32 + GDI, no frameworks, no installers.

Run **`Hadron.exe`** for everything in one window, or any single tool on its own (`HadronBios.exe`, `HadronDiskMap.exe`, ...).
Both are built from the same module code.

| Module | What it does |
|---|---|
| BIOS Settings Explorer | Every setting in your BIOS, including hidden ones: current value, default, a plain-English explanation, real-world effect and per-goal advice. Read-only. |
| DiskMap | See what's using your drive space |
| Organizer | Sort a messy folder into categories, with a preview and undo |
| Organizer Rescue | Undo file moves using the NTFS change journal |
| SaveFinder | Find game saves and personal data before deleting a Steam library |

Status: the tools exist as single-file programs in `legacy/` and are being ported to the module system.
See `docs/` for architecture and history, `CLAUDE.md` for the project rules, and `KICKOFF.md` for how the AI team is set up.

## Safety
- Nothing that moves or deletes files acts without showing a plan and keeping an undo log.
- Nothing writes to firmware. The BIOS module reads and explains; you apply changes in the BIOS yourself.
