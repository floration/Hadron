# Hadron

A suite of small, fast, native Windows utilities: raw C++, Win32 + GDI, no frameworks, no installers.

Run **`Hadron.exe`** for everything in one window, or any single tool on its own (`HadronBios.exe`, `HadronDiskMap.exe`).
Both are built from the same module code.

| Module | What it does |
|---|---|
| BIOS Settings Explorer | Every setting in your BIOS, including hidden ones: current value, default, a plain-English explanation, real-world effect and per-goal advice. Snapshots and snapshot compare. Read-only. |
| DiskMap | See what's using your drive space |

Status: both tools exist as single-file programs in `legacy/` and are being ported to the module system.
See `docs/` for architecture and history, and `CLAUDE.md` for the project rules. `KICKOFF.md` describes how the AI team is set up.

## Safety
- Nothing writes to firmware. The BIOS module reads and explains; you apply changes in the BIOS yourself.
- Anything that moves or deletes files shows a plan first and keeps an undo log.

## Running unsigned builds
Windows marks downloaded zips. Before extracting, right-click the zip > Properties > tick **Unblock**, or SmartScreen will warn on every exe.
