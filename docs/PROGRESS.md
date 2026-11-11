# Progress

| Task | Status | Owner | Branch | PR |
|---|---|---|---|---|
| Phase 0: legacy baselines + fixtures + goldens | done (not yet PR'd) | lead | claude/project-thread-7nnuw6 | - |
| docs/legacy/diskmap.md | done | module-builder | merged into branch | - |
| docs/legacy/bios-explorer.md | done | module-builder | merged into branch | - |
| Phase 1: core skeleton (host/sys, hello, CMake, CI) | built; compiles MinGW, renders in Wine | lead | claude/project-thread-7nnuw6 | #1 |
| Port `diskmap` (Baseline + Console) | not started | module-builder | - | - |
| Port `bios` + snapshots | not started | module-builder | - | - |
| `runas` module (new) | design written | lead | claude/project-thread-7nnuw6 | - |
| `core/fs` header + design note | not started | lead | - | - |
| `runas` module build | blocked on core/ui list widget | lead | - | - |

## Handoff notes
(Newest first. The lead adds a dated note at the end of every session: what changed, what's next, what's blocked.)

### 2026-11-11 (lead, Opus) — update 3
- **core/ui built** on the same branch (PR #1): the theme system and the shared widgets.
  - `core/ui/theme.{h,cpp}`: four themes (Light/Dark/Fun/Boring) with neutral tokens, the two layouts
    (Baseline/Console), and the accent derivation — main accent from the live Windows accent (registry/DWM),
    three more at +90/+180/+270 hue (own HSL maths), per-theme fallback when the accent is achromatic. Deterministic
    `Resolve()` for screenshots, `ResolveLive()` for the app.
  - `core/ui/widgets.{h,cpp}` + `core/ui/draw.h`: `ScrollBar` (draggable), `ListView` (sortable headers, wheel +
    scrollbar, keyboard nav, focus rect, high-contrast fallback), `SearchBox`, `DetailPane`. Plain objects painted
    from `Module::Paint`; input arrives via the new `Module::OnInput`, forwarded by the content window.
  - Host owns theme + layout, chosen by `--theme`/`--layout`; modules read `IHost::Theme()/Fonts()/Layout()`; the
    suite chrome themes too.
  - `HadronUikit.exe` (`modules/uikit`) is the gallery/screenshot target (not in the suite catalogue). CI renders all
    eight theme×layout combinations on Linux (Wine) and Windows (MSVC) and uploads them; both run a `--selftest`.
  - Verified: clean MinGW build, hello goldens byte-identical (no regression), eight gallery screenshots render.
- **Fixed CI** earlier this session (PR #1 went green): MinGW enum-vs-`#define` clash, the windows-msvc job couldn't
  find VS so it now builds via vswhere+vcvars+Ninja, and two MSVC-only source issues (`small` macro, missing
  `<objbase.h>`). See [[hadron-ci-build]].
- **Next**: diskmap port (Baseline + Console), then bios, then core/fs header + design note, then runas.

### 2026-11-11 (lead, Opus) — update 2
- **Core skeleton built and on the branch.** `core/sys` (elevation, privileges, long paths, Worker, Settings,
  logging + minidump, a dependency-free PNG writer) and `core/host` (the Module contract, one command-line parse,
  the shared Host with a sidebar, and the deterministic `--screenshot` compose path). A `hello` module proves the
  contract: `HadronHello.exe` and `Hadron.exe --solo hello` render identically, and `Hadron.exe` adds the sidebar.
- **Builds both ways.** CMake with a MinGW toolchain file (release) and MSVC (CI). `hadron_add_module()` generates
  each exe's manifest (requireAdministrator, PerMonitorV2, longPathAware, Common-Controls v6) and VERSIONINFO.
  Verified: clean MinGW compile, `--selftest` PASS, and three Wine/Xvfb screenshots (committed as hello goldens).
- **CI** (`.github/workflows/ci.yml`): a firmware-write guard (rule 2), a Linux MinGW build + selftests + Wine
  screenshots, and a windows-latest MSVC build doing the same.
- **ARCHITECTURE.md reconciled** with rule 4: `Prefs()` not `Settings()`, no `RequestElevation`, no asInvoker,
  screenshot via compose not PrintWindow, `LoadFixture` added.
- **Next**: core/ui widgets (list view, detail pane, search, scrollbar, the four themes + Windows accent), then the
  diskmap port, then runas.

### 2026-11-11 (lead, Opus) — update 1
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
