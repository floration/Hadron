# Progress

| Task | Status | Owner | Branch | PR |
|---|---|---|---|---|
| Phase 0: legacy baselines + fixtures + goldens | done (not yet PR'd) | lead | claude/project-thread-7nnuw6 | - |
| docs/legacy/diskmap.md | done | module-builder | merged into branch | - |
| docs/legacy/bios-explorer.md | done | module-builder | merged into branch | - |
| Phase 1: core skeleton (host/sys, hello, CMake, CI) | not started | lead | - | - |
| Port `diskmap` (Baseline + Console) | not started | module-builder | - | - |
| Port `bios` + snapshots | not started | module-builder | - | - |
| `runas` module (new) | design written | lead | claude/project-thread-7nnuw6 | - |
| `core/fs` header + design note | not started | lead | - | - |

## Handoff notes
(Newest first. The lead adds a dated note at the end of every session: what changed, what's next, what's blocked.)

### 2026-11-11 (lead, Opus)
- **Decisions this session** (all in DECISIONS.md): repo KICKOFF.md + a `hello` contract module; `core/fs`
  header-only in phase 1; two layouts per module (Baseline default, Console) x four themes (light/dark/fun/boring)
  with accent from the Windows accent colour; every exe `requireAdministrator` (rule 4 rewritten, no Optional mode,
  no relaunch); `diskmap` may use `SeBackupPrivilege` to size protected folders (approved deviation); threads run on
  Opus 5.5, only the coordinator on Fable 5.2.
- **Phase 0 done**: `tests/tools/build_legacy.sh` builds both legacy tools unchanged; fixture generators and
  Wine/Xvfb goldens under `tests/golden/legacy/`; `docs/legacy/{diskmap,bios-explorer}.md` written by subagents.
- **New module `runas`** requested and approved; design in `docs/design/runas.md`. Three open choices for the owner
  listed there (non-admin-user support, default SYSTEM integrity, button set). Token code waits for the core skeleton
  and is only testable on the owner's PC / windows-latest CI, not Wine.
- **Doc debt**: ARCHITECTURE.md still describes `Admin::Optional`, `RequestElevation`, `--elevated`; reconcile it in
  the core skeleton PR. The ARCHITECTURE.md fixes from the phase 1 plan doc still need applying.
- **Next**: open the Phase 0 PR, then build the core skeleton.
