# svcdrv: Build (stage 3)

Stage 3 (Build), 2026-11-11. Branch `module/services-svcdrv`, from the stage 2 commit 7c3ae90. Built by
`module-builder-careful` from `2-research.md`; checked by the Build thread; KB audited by `kb-auditor`.

## What it does
A new module `services` ("Services and drivers"), `HadronServices.exe` standalone and inside `Hadron.exe`, `Admin::Required`.
- **Two tabs**, Services and Drivers: every Win32 service (per-user `_LUID` instances folded under their template, read-only)
  and every kernel/file-system driver, with state, start type (+trigger, pending restart), type, account or group, company
  (VERSIONINFO, the file's own claim), "vs first" drift, lowest Hadron preset level, and basis. Filter, sort by any header,
  wheel and draggable scrollbar, keyboard navigation.
- **Detail pane**: the service's registered description (labelled as not Hadron's), Hadron's KB text with its basis, preset
  membership with its pbasis, dependencies, protection reason.
- **Single-item actions**: start type (Auto, Auto (delayed), Manual, Disabled) through plan, confirm, snapshot, undo log and
  recovery; Start/Stop/Restart for services with a confirm and log line, no undo entry. Nothing for drivers' live state.
- **Presets**, same four levels per tab: Default (restore first snapshot; Windows stock, which is empty, see below), Pure bloat
  removal, Stripped, Minimum. Start type only, effective after restart; Hadron never reboots. The plan is an in-view screen with
  per-row include toggles, dependency warnings and hardware/use evidence.
- **Safety**: data dir `%ProgramData%\Hadron\services\` (Admin/SYSTEM DACL); `first.txt` on first live load; per apply a
  snapshot, a recovery folder + zip (`restore.cmd` for WinRE, `.reg` files for non-ASCII names, README with BitLocker unlock
  and the Safe Mode variant checking admin with `fltmc`), and a flushed, CRC'd undo log (`B`/`P`/`I`/`D`/`E`). Undo last apply,
  interrupted-apply banner with Undo/Resume, apply lock (named mutex + share-0 log).
- **Guards** (§6, pure, each selftested): computed never-touch set (Boot-start, class/device filters, boot-disk chain,
  ErrorControl=3, curated floor, core services), never-in-preset set (Defender stack, TrustedInstaller, VSS, swprv, listed
  filters, unknown/third-party, wrong build), `sysok` gate on System-start drivers, `_LUID` never written, vanished/changed rows
  skipped.
- **TrustedInstaller toggle** (B6): off at launch, never persisted. Rows the SCM refuses to admin are marked and left out
  while it is off. With it on, the apply Worker impersonates TI per SCM call, verifies the impersonation level and TI SID,
  never impersonates for Hadron's own files, and stops TI afterwards only if it started it.

## How and why
- `backend_live.cpp` is the only file that calls SCM/registry write APIs, and its writes are inert until a confirmed plan
  `Arm()`s it. The fake backend (fixture + fault injection) drives the selftest, goldens and screenshots, so no test touches
  real services or ProgramData.
- Pure code (model, plan, store, recovery, kb, hw evidence) is separated from Win32 so every guard and format is unit-tested
  under Wine.
- The TI helper is module-local (`ti.cpp`), written fresh from the runas technique; no `core/` change beyond the one-line
  registration in `core/host/registry.*`.
- Design note: `docs/design/services.md` (lists every spec ambiguity the builder resolved).

## Build steps
| Step | Status |
|---|---|
| B1 read-only page | done |
| B2 first-run snapshot + recovery | done |
| B3 KB generator + 99-entry seed | done, kb-auditor **pass** (second round) |
| B4 single-item writes, undo, resume, lock, `--live-test` | done (live-test untested, CI only) |
| B5 preset engine, all four levels | done |
| B6 TI toggle | done, untested on Windows |

## Commits
eef6b0d (B3), 4656e65 (B1), d2fce03 (B2), 6ad50f7 (B4+B5), 35eac16 (B6), 19c3431 (page, selftest, exe, registration),
7dd288e (CI guard, acceptance script, design note), 16cf2f5 (KB audit fixes, No-guidance rule), 593c6e5 (unticked
use-undetectable Stripped rows, audit text fixes).

## Checks (re-run by the Build thread on a clean build)
| Check | Result |
|---|---|
| MinGW build, `HadronServices.exe` + `Hadron.exe` | ok, no new warnings |
| `genkb.py --selftest`; regenerate and `cmp services.db` | pass; identical |
| `wine HadronServices.exe --selftest --fixture tests/fixtures/services` | PASS, 137 checks |
| `wine Hadron.exe --solo services --selftest ...` | PASS, 137 checks |
| 8 golden plans (`--dump-plan <level> --tab <tab>`) | all match |
| `tests/tools/services_check.sh` | rc 0, 10 screenshots |
| Source guard (`DeleteService`, `CreateService`, `RegDeleteKey`, `RegDeleteTree`, firmware writes) | clean |
| MSVC compile | **not run** (no MSVC here); first CI run is the first MSVC compile |
| windows-latest `--live-test HadronTestSvc` and TI variant | **not run**; wired in ci.yml |

Selftest live part under Wine: read-only enumerate of 24 items, and the live backend refuses writes until armed.

## kb-auditor
Round 1 at 7dd288e: pass after fixes (7 blockers, 16 minors). Proxy decision logged in DECISIONS.md: IoT Enterprise's
"No guidance" blocks Pure bloat; a Server 2016-only "No guidance" doesn't when IoT rates the item OK, but the pbasis says so.
Round 2 at 593c6e5: **pass**, all 23 fixed. Optional cosmetics left for Finalize: add @IOT to NgcSvc's and CaptureService's
basis notes; WdFilter's expl and if_off repeat a sentence; WdiServiceHost sits under the Minimum comment in kb.py.

## Screenshots (Baseline, dark, fixture only)
Shared files `pipeline/svcdrv/build/`: services-list, services-drivers, services-detail-kb, services-detail-protected,
services-plan-stripped, services-interrupted, services-ti-list, services-ti-on, services-light, services-suite.

## Not done
Deferred by the spec (Push files these as ideas):
- Windows stock start types: the 19045 table is **empty** (no sourced data), so "Default: Windows stock" shows 0 rows and says so; other builds not started.
- Custom preset export/import file.
- Measured before/after RAM and process count.
- Authenticode signer column.
- Event 7040 account in drift.
- SeRestorePrivilege registry-write fallback for SCM-refused items.

For Analyze (spec §10, verify on Windows):
- TI-impersonated `ChangeServiceConfigW` passing the SCM check; per-user templates on 19045; `reg load` of `SYSTEM` from WinRE;
  the owner's class filters, boot storage path, LAN/Wi-Fi/BT names and VBS state (read-only, project folder only); en-in/en-au
  KB URLs vs en-us.
- MSVC compile risks the builder flagged: `std::max<size_t>`, `wcschr` const-ness, the `Hx*` trigger structs.
- `restore.cmd` does not check the error level of the `DelayedAutostart` `reg add` (it does for `Start`).

Lead follow-ups (core, not this pipeline): move the TI helper into `core/sys`; `ui::ListView` EnsureVisible and per-cell
style; a core word-wrap helper (Wine's `DT_WORDBREAK` overflows long words); `SERVICE_TRIGGER_INFO` into `win_compat.h`;
runas's missing TI-state restore.

## Round 2: fixes for Analyze (2026-11-11)
Analyze (b97f5d0) sent back A1–A7. Fixed in 8c66aa7 and c427726; the Build thread re-ran everything on a clean build.

| # | Fix | Test |
|---|---|---|
| A1 | `:put` uses gotos with no `( )` block around names; `CmdSafeName` is an allow-list, so `Intel(R) ...` goes to `.reg`; DelayedAutostart, `reg unload` and `fltmc` errorlevels checked | `restore.cmd` golden with an `Intel(R)` row, block-free check, `restore-dryrun.cmd` golden; new windows-latest step runs the echo-only script on real cmd.exe |
| A2 | First-run recovery applies Default's exclusions (never-touch, Boot/System, storage, never-preset, launch-protected, System without sysok); README section F lists them | `first-restore.cmd`, `first-README.txt` goldens; no stornvme on disk |
| A3 | `TiService` claims "started" only for its own start from STOPPED, reset in Acquire/Release; START_PENDING never claimed | two-apply selftest with TI already running; START_PENDING case |
| A4 | Undo/Resume refuse a log from another machine GUID or build; per-row guard (never-touch, no Boot target, no Boot/System on a service, sysok) | committed `forged/` log: 3 rows blocked; mismatch refusals; `services-forged-undo` screenshot |
| A5 | Data dir: no reparse point on any component, owner must be Administrators or SYSTEM, protected DACL re-applied; otherwise nothing is read, writes are off, red banner | default-owner refusal under Wine; injected reparse point (Wine can't make one); prefixed missing-parent root |
| A6 | Dependency check on the end state, both directions, to a fixed point, re-run on every toggle and at apply time | aaa←bbb←ccc chain, toggles, enable direction, apply-time cascades |
| A7 | Default, stock, Undo, Resume and the apply re-check compare and write the raw DelayedAutostart value | `plan-services-default-delayedlinger.txt` golden (`Spooler 3/1 -> 2/0`) |

Minors fixed: TI released on any failure after Acquire; `--ti on` fixture-only; delayed flag before Start=2 and no delayed for grouped
services; hardware guards fail closed with a banner; dropped-CRC count shown; UTC stamps plus an order sequence; interrupted undo
no longer hides the original apply; TiLeftRunning checks all logs; logic M5/M6 and Win32 services with Start 0/1 guarded; WaitState
and stop-reason fallback; `--data-dir` without `\\?\`; HwEvidence unknown hint = kept; BuildSingle cleanups; plan building and
CanWrite moved to Workers, load worker honours cancel; precomputed sort keys; design note D4/D5; D3 (trigger GUID→hardware mapping
and last-arrival date not built) recorded as a deviation in DECISIONS.md, tagged proxy.

Not fixed: canonical en-us KB URLs (learn.microsoft.com still blocked by the container proxy; no KB text changed, so no kb-auditor
round); logic M3 (Resume orphans rows from undo/drift tracking); UNC data dir untested under Wine; the UI items, coverage and stock
table are left for Design and Ideas as 4-analyze.md says.

For Analyze to look at: two static test hooks in `Store` (`AcceptTokenUserAsOwner`, needed because Wine ignores SD owners, and
`InjectedReparsePoint`) are set only by the selftest; check nothing else can set them. The live worker path, real cmd.exe, the live
write order, WaitState and the stop fallback run only in CI or on Windows.

Checks: MinGW build 0 new warnings; `services_check.sh` rc 0 (genkb selftest + cmp, fixtures cmp, selftest 178/178 standalone and
`--solo`, 8 plan goldens + delayedlinger, 12 screenshots in shared `pipeline/svcdrv/build/`). MSVC still CI-only.
