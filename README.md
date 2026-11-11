# Hadron

A suite of small, fast, native Windows utilities: raw C++, Win32 + GDI, no frameworks, no installers.

Run **`Hadron.exe`** for everything in one window, or any single tool on its own (`HadronBios.exe`, `HadronDiskMap.exe`,
`HadronRunAs.exe`, `HadronServices.exe`).
Both are built from the same module code.

| Module | What it does |
|---|---|
| BIOS Settings | Every setting in your BIOS, including hidden ones: current value, default, a plain-English explanation, real-world effect and per-goal advice. Snapshots and snapshot compare. Guarded writes, off by default. |
| DiskMap | See what's using your drive space. Read-only. |
| Run As | Launch anything as this admin, another user, SYSTEM or TrustedInstaller (SYSTEM/TI behind an off-by-default switch). |
| Services | Every service and driver with its state, start type and an explanation with its basis; start/stop; presets (as first seen, pure bloat, stripped, minimum) with a plan, undo and a WinRE restore script. See `docs/modules/services.md`. |

Status: DiskMap and BIOS Settings are ported from `legacy/`; Run As and Services are new. See `docs/MODULES.md`.
See `docs/` for architecture and history, and `CLAUDE.md` for the project rules. `KICKOFF.md` describes how the AI team is set up.

## Safety
- Firmware: BIOS Settings changes setup values only behind a never-saved "Allow BIOS writes" switch, with a plan,
  a backup and read-back. It never flashes.
- Services: every change is a plan first, with an undo log and a recovery script written before anything changes.
  Presets take effect at the next restart; Hadron never restarts the PC.
- Anything that moves or deletes files shows a plan first and keeps an undo log.

## Running unsigned builds
Windows marks downloaded zips. Before extracting, right-click the zip > Properties > tick **Unblock**, or SmartScreen will warn on every exe.
