# Decisions

| Date | Decision | Why | Approved by |
|---|---|---|---|
| 2026-11-11 | Organizer, OrganizerRescue and SaveFinder dropped from Hadron | Owner doesn't need them | owner |
| 2026-11-11 | The suite runs as admin (`requireAdministrator`); drop targets also offer Browse because of UIPI | Owner's call; live BIOS reads need admin | owner |
| 2026-11-11 | All subagents may reach the owner's PC (read/write files, build, run) inside an agreed project folder | Owner's call | owner |
| 2026-11-11 | MinGW ships releases; MSVC is a CI check | MinGW is the proven toolchain | design review |
| 2026-11-11 | Every KB claim and number carries a basis, audited by kb-auditor | Incident 5 | design review |
| 2026-11-11 | Follow the repo `KICKOFF.md` (Phase 0 baselines, `diskmap` as proof port), plus a small `hello` module as a contract test | The uploaded kickoff was an older version listing dropped tools | owner |
| 2026-11-11 | `core/fs` gets a header and design note only in phase 1; code is built with its first file-moving consumer | No module in the catalog moves or deletes files | owner |
| 2026-11-11 | `diskmap` is `Admin::Optional` (keeps legacy's run-as-admin button) | Legacy parity | owner |
| 2026-11-11 | Two layouts per module: Baseline (default) and Console; four colour themes each: light, dark, fun, boring. Main accent = Windows accent colour, others at +90/+180/+270 hue; achromatic accent falls back to a per-theme colour | Owner's UI choice from 10 explored directions | owner |
| 2026-11-11 | Every project thread runs on Opus 5.5; only the project coordinator runs on Fable 5.2 | Owner's call; supersedes the Team section of CLAUDE.md | owner |
| 2026-11-11 | Every exe is `requireAdministrator`; no `Admin::Optional` mode and no elevation relaunch. SYSTEM/TrustedInstaller-level features sit behind an off-by-default toggle, built only when a module needs one. Supersedes the `diskmap` Admin Optional entry and changes hard rule 4 | Owner's call, for ease of production | owner |
| 2026-11-11 | `diskmap` enables `SeBackupPrivilege` and opens folders with `FILE_FLAG_BACKUP_SEMANTICS`, so protected folders (System Volume Information etc.) are sized instead of "Not reachable". Approved deviation from legacy totals | It runs as admin anyway; sizing what uses the space is the tool's job. Still read-only | owner |
