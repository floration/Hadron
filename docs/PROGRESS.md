# Progress

| Task | Status | Owner | Branch | PR |
|---|---|---|---|---|
| Phase 0: legacy baselines + fixtures + goldens | done (not yet PR'd) | lead | claude/project-thread-7nnuw6 | - |
| docs/legacy/diskmap.md | done | module-builder | merged into branch | - |
| docs/legacy/bios-explorer.md | done | module-builder | merged into branch | - |
| Phase 1: core skeleton (host/sys, hello, CMake, CI) | merged | lead | claude/project-thread-7nnuw6 | #1 |
| Port `diskmap` (Baseline + Console) | merged; owner's-PC checks pending | module-builder | module/diskmap | #1 |
| Port `bios` + snapshots + KB v2 | merged; owner confirmed live read works | module-builder ×2 | module/bios, kb/bios | #3 |
| `bios` guarded writes (rule 2 changed) | review passed (4 rounds, 1 waived); owner's first real write worked (build f) | module-builder-careful | module/bios-write | #5 |
| `core/fs` header + design note | not started | lead | - | - |
| `runas` module | merged; owner's-PC launch checks pending | module-builder | module/runas | #2 |
| In-app layout/theme switch, settings.ini merge-on-save | merged | lead | claude/project-thread-7nnuw6 | #6 |
| Auto-update from GitHub releases, Reading Room theme, anti-aliased shapes | merged; owner approved moving the old exe | lead | claude/project-thread-7nnuw6 | #7 |
| Core follow-ups from svcdrv (PR #8, docs/pipeline/svcdrv/7-finalize.md): TrustedInstaller helper into core, `--size`/`--dpi` fixture-render flag, ListView and theme APIs, shared F-key bar, opt-in per-list font | backlog, after the owner's current asks | lead | - | - |
| `services` module (pipeline `svcdrv`, full run) | merged; read-only owner's-PC checks pending | pipeline | module/services-svcdrv | #8 |
| `services` KB follow-ups (svcdrv-kb: stock table, Fax, drivers, OEM) | merged | Small fixes | fix/svcdrv-kb | #9 |
| `services` custom presets + Measure (svcdrv-presets) | merged | Small fixes | fix/svcdrv-presets | #10 |
| `services` Windows-stock from WinSxS + tables (svcdrv-stock) | merged | Small fixes | fix/svcdrv-stock | #11 |
| `services` Signer, event 7040, interface evidence, registry fallback (svcdrv-evidence) | merged | Small fixes | fix/svcdrv-evidence | #12 |
| `services` selftest use-after-free fix (svcdrv-selftest-crash) | merged | Small fixes | fix/svcdrv-selftest-crash | #14 |
| `services` every Services key, StartOverride, TI read pass, protected-items override (svcdrv-visibility) | merged | Small fixes | fix/svcdrv-visibility | #15 |
| `services` red protected rows in the Baseline plan list, visible override check mark (svcdrv-protected-red) | merged | Small fixes | fix/svcdrv-protected-red | #16 |

## Handoff notes
(Newest first. The lead adds a dated note at the end of every session: what changed, what's next, what's blocked.)

### 2026-11-11 (Push thread) — svcdrv
- `services` module (idea `svcdrv`) reached Push after all eight stages; every category 1-4 finding closed
  (Review R1-R25 and the Finalize mini passes). Main (e085b61, #6) merged into the branch; the one conflict
  (`core/host/cmdline.h`) kept both new fields. MinGW release build of the merge passes.
- Docs: `docs/modules/services.md` (new), README and MODULES rows, an ARCHITECTURE fix (`hx::ui::Confirm()` never
  existed), and six DECISIONS rows from Finalize.
- First windows-latest run is the first MSVC compile, `--live-test`, real-cmd `restore-dryrun.cmd` and real
  folder-owner check for this module.
- Still wanted on the owner's PC, read-only and never applying a preset: class filters, boot path, VBS, DPI, high
  contrast, keyboard, the minimum-size clamp at 1366x768 125% (list in docs/pipeline/svcdrv/7-finalize.md).
- Ten follow-up ideas from Finalize sent to the Ideas thread.

### 2026-11-11 (lead, Opus) — update 5
- Owner chose guarded BIOS writes; hard rule 2 rewritten, CI allows the write API only in `modules/bios/fwwrite.cpp`.
  Spec `docs/design/bios-write.md`. Backups: `%ProgramData%\Hadron\bios-backups` (admin-only, handle-checked).
  Owner wrote a setting with build f (backup of that write is in the old `%LOCALAPPDATA%` location, not restorable).
- Follow-ups: (1) bios selftest scratch folder under %TEMP% is changed by path while elevated: create it admin-only
  and hold a no-share-delete handle for the run (PR #5 review round 3 finding 1, waived for the PR);
  (2) trust refusal headline "isn't verifiably admin-only" (finding 4); (3) Global mutex access-denied path untested;
  (4) diskmap Windows CI shows an informational "links not followed = 2 (want 1)" FAIL before the passing run: find out why.

### 2026-11-11 (lead, Opus) — update 4
- PR #1 (core, core/ui, diskmap) and PR #2 (runas) merged.
- `bios` port: module (live UEFI read on a Worker, thread-only SeSystemEnvironmentPrivilege, SMBIOS board/BIOS
  guard, in-app Save/Load/Compare snapshots with own zip+inflate, export with basis lines) and KB v2
  (`tools/bios/genkb.py` fails on unsourced numbers; kb-auditor passed after 3 rounds, 42 findings fixed).
  Reviewer passed after 2 rounds. CI now gates the generator output and runs bios selftests + screenshots.
- Owner's-PC checks still open: diskmap selftest on real NTFS; runas SYSTEM/TI/another user/limited; bios live read
  and whether SMBIOS reports `A.F0` (guard maps it to 7E06vAF).
- Core gaps raised by builders: ListView per-cell colour hook; CreateModuleContent forwarding WM_MODULE_BASE+n,
  timers, WM_MOUSELEAVE, WM_GETDLGCODE; a per-module CLI argument hook.

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
