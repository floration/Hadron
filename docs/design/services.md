# services: design note

The Services page (`modules/services`, idea `svcdrv`). The spec is `docs/pipeline/svcdrv/2-research.md`; this note
records how the build met it: files, formats, safety mechanics and the calls the spec left open. When the two
disagree, the spec wins.

## What it does
- Lists every Win32 service and kernel driver on two tabs, sortable and filterable. Per-user instances
  (`CDPUserSvc_4f2a1`) are folded onto their template.
- Shows a detail pane for the selected item:
  - Hadron's KB text, with its `basis`, preset membership and `pbasis`;
  - hardware evidence;
  - the configuration, with the first-snapshot and Windows-stock values;
  - triggers, dependencies and failure actions;
  - the protection state (PPL, the security-descriptor check, Tamper Protection);
  - the service's own registered description, labelled as not Hadron's.
- Offers five presets: Default (first snapshot), Default (Windows stock), Pure bloat removal, Stripped and Minimum.
  It also offers single-item start-type changes, and Start/Stop/Restart.
- Every change becomes an in-view **plan** first. Nothing is written until **Apply** is pressed: Enter never applies,
  and fixture mode never applies.
- Presets change only `Start` and `DelayedAutostart`. They take effect at the next restart, and Hadron never restarts
  the PC.

## Files
| File | Role |
|---|---|
| `model.*` | Pure data: `SvcItem`, `StartCfg` (start + delayed, where delayed is -1 when absent), names, image paths, folding |
| `kb.*` | Loads the embedded `services.db` (RCDATA `SERVICES_KB`) |
| `hw.*` | Hardware evidence. The pure part reads a dump; the live part uses CfgMgr32 over every devnode, present or not |
| `backend.h` | The `Backend` interface: Enumerate, Query, CanWrite, SetStart, Control, Arm/Disarm |
| `backend_live.cpp` | **The only file that writes to the SCM.** Every write refuses until `Arm()` |
| `backend_fake.*` | In-memory backend for the selftest, `--dump-plan` and screenshots. Supports fault injection |
| `store.*` | The data dir, the snapshot and undo-log formats, `CREATE_NEW` writers, the apply lock |
| `recovery.*` | `restore.cmd`, `restore-online.cmd`, `README.txt` and `.reg` files, plus a stored-only zip writer |
| `plan.*` | Guards, presets, single/default/undo/resume/control plans, dependency checks, apply order, `RunApply` |
| `ti.*` | TrustedInstaller impersonation (B6). Token work only; it starts and stops TI through the `Backend` |
| `module.cpp` | The UI (`hx::Module`) |
| `selftest.cpp`, `cli.h` | `--selftest`, `--dump-plan` and `--live-test` |
| `standalone.cpp` | `HadronServices.exe`. Intercepts `--dump-plan` and `--live-test` before the GUI starts |
| `tools/services/kb.py`, `genkb.py` | The KB source and the rule-7 gate (see `tools/services/README.md`) |
| `tests/fixtures/services/` | The fixture machine (`make_services_fixture.py` writes it; CI compares) |
| `tests/golden/services/` | 8 plan goldens and a `restore.cmd` golden |

## Formats
All formats share these rules:
- UTF-8, LF line ends, TAB-separated fields, with bios escaping (`\` doubled, TAB becomes a space, CR is dropped,
  LF becomes `\n`).
- Line 1 is `HXSVC\t1\t<kind>`.
- Unknown lines are skipped, and there is a 16 MB cap.

**Snapshot** (`first.txt`, `snap-<stamp>.txt`, fixture `snapshot.txt`, kind `snapshot`):
- `M key value` meta lines (machine GUID, build, created), sorted.
- One `S` line per item, 22 fields: name, display, type (hex), cfg (`2/1`, `3/-`), trigger, state, errctl,
  account, image, group, deps (`/`-joined), description, regStart, protect, controls (hex), pid, failure,
  triggers, interfaces, company, readError.
- `X name acl=ti` lines exist only in fixtures: they make the fake backend refuse an admin write.

**Undo log** (`undo-<stamp>.log`, kind `undo`). Every record ends with TAB and the CRC-32 (`%08X`) of the text
before it. A record with a bad CRC (a torn write) is ignored.
```
B  stamp source count machine build order  begin; source = preset:<lvl>:<tab> | single | default-first:<tab> |
                                          default-stock:<tab> | undo:<stamp> | resume:<stamp>
P  seq name start before after identity  every plan row, written before any change
T  started|stopped                       Hadron started / stopped the TrustedInstaller service
I  seq observed identity                 flushed BEFORE the SCM call
D  seq ok|skip|fail afterObserved text   flushed after the call and a re-read
E  stamp ok skipped failed done|cancelled
```
- Start/Stop/Restart (control plans) write no log file: one host log line per operation (spec §5.2).
- Stamps are UTC (`yyyyMMdd-HHmmss`, `-N` when taken). `order` is 1 + the highest `order` of the logs already
  there, so logs sort by `order`, then by stamp with `-N` compared as a number; a clock change can't reorder them.
  Logs without `order` (none were ever shipped) sort first.
- A log with `B` and no `E` is **interrupted**. The page offers Undo (the rows with `I`) or Resume (the rows
  without `D`).
- A finished `undo:<stamp>` or `resume:<stamp>` log resolves that log, wherever it sorts. A finished
  `resume:<U>` where U is `undo:<X>` resolves X too. An unfinished undo or resume of a resolved log is resolved.
- For an interrupted `undo:<X>` log the banner says so: Undo starts again from X's log (never "undo the undo",
  which would re-apply X), Resume finishes the undo from its own log.
- Logs from an undo are never offered for undo themselves.
- Undo and Resume refuse a log whose machine GUID or build differs from the PC's (empty plan, reason shown), and
  show how many records failed their CRC and were ignored, and a planned-vs-read row count mismatch.

**hw dump** (fixture `hw.txt`, kind `hw`): devnodes with class, service, filters, the present flag and the
parent; class filters; the boot-disk chain; and the hypervisor and VM flags.

## Safety mechanics (hard rules 1, 4 and 5; spec §5)
- **Write path.** `ChangeServiceConfigW` and `ChangeServiceConfig2W` (delayed flag), `StartServiceW` and
  `ControlServiceExW` appear only in `backend_live.cpp`. Each refuses until `Arm()`.
  - The UI arms the backend after the user confirms a plan, and disarms it when the apply ends.
  - Selftest and screenshot modes use the fake backend.
  - CI fails on `DeleteService`, `CreateService`, `RegDeleteKey`, `RegDeleteTree` or a firmware-write API under
    `modules/services/`.
- **Apply order** (`RunApply`). If anything in steps 1–3 fails, nothing changes.
  1. Take the lock: named mutex `Global\Hadron.services.apply`, plus in-process tracking, because a mutex is
     recursive for its owner.
  2. Write `snap-<stamp>.txt`.
  3. Write `recovery\<stamp>\` and `recovery-<stamp>.zip`.
  4. Write the log header, `B` and every `P` row.
  5. For each row:
     - re-read the item, and skip it if it vanished or changed (start, or the raw delayed value);
     - when lowering, skip it if a plan row that depends on it failed or was skipped and is not Disabled; the
       skipped row then protects its own dependencies too;
     - write `I` and flush;
     - make the SCM write (as TI where marked). `SetStart` writes the delayed flag before `Start`: the SCM ignores
       it unless the start is Auto, and when it refuses the flag nothing has changed yet;
     - re-read and verify;
     - write `D` and flush.
  6. Write `E`, and stop TI if this apply started it. From the moment the TI token is taken, every return path
     (a failed log write or flush included) puts TI back the same way.
- **Guards** (`CheckGuard`). These items are never touched:
  - Boot-start drivers;
  - Win32 services whose registry `Start` is Boot or System (not a start type Windows uses for services);
  - every driver while the device list or the boot disk can't be read (fail closed: the filter and boot-path
    checks can't run; a banner and a plan note say so);
  - filters;
  - the boot storage chain;
  - drivers with ErrorControl Critical;
  - the floor (RPC, DCOM, LSA, the event log, the audio stack, networking, Hadron's own dependencies);
  - Hyper-V drivers while the hypervisor runs.

  System-start drivers are not on that list (spec §5.2): a single change may set one where Windows allows, and a
  preset, Default or Undo row may change one only with a curated `sysok` entry, checked against the live start
  type. Hadron never *sets* Boot or System start, except an Undo putting back a System start it changed under
  `sysok`.

  These items are never in a preset:
  - protected (PPL) services;
  - the Defender/Security stack, TrustedInstaller, VSS and swprv (spec §8.4).

  Preset rows also need a KB entry for this build, and a driver needs `sysok`. A preset only lowers: an item
  already stricter than the level (Disabled where it says Manual) is left alone and counted in a note.

  Undo and Resume rows pass the same guards on the live item (a log has a CRC, not an author): never-touch
  items, Boot start, Boot/System on a service and a System-start driver without `sysok` are blocked.
- **Data dir.** `%ProgramData%\Hadron\services\`, owner Administrators, protected DACL (Administrators + SYSTEM).
  - `EnsureRoot` runs at every load and every apply, whoever made the folder. No component below the volume or
    share root may be a reparse point. The root is opened with `FILE_FLAG_OPEN_REPARSE_POINT |
    FILE_FLAG_BACKUP_SEMANTICS` and checked again, its owner must be Administrators or SYSTEM (a standard user can
    create folders under ProgramData), and the protected DACL is applied again. If any of that fails, nothing is
    read from or written to the folder: no first.txt, no undo logs, changes off, and a banner gives the reason.
  - Missing parents are created up to the volume or share root (UNC and `\\?\` roots included).
  - Every file is `CREATE_NEW` and flushed. Nothing is overwritten.
  - A corrupt `first.txt` is renamed aside.
  - `--data-dir` overrides the location; it is kept as a full path without the `\\?\` prefix (README shows it).
    `--readonly` never arms the backend.
- **TrustedInstaller (B6).** Off by default, and never remembered.
  - Rows the SCM refuses to an administrator (`CanWrite` reports access denied) are marked. While the toggle is
    off they are left out.
  - With the toggle on, the apply thread:
    1. starts TI only if it is STOPPED (through the Backend). Only a start that succeeded from STOPPED counts as
       "Hadron started it"; START_PENDING is someone else's start, waited for and never claimed. The claim is
       reset at every Acquire and Release, so a later apply never stops a TI that Windows started;
    2. enables SeDebugPrivilege on the thread only (checking `ERROR_NOT_ALL_ASSIGNED`);
    3. impersonates winlogon's SYSTEM token from the same session;
    4. duplicates TI's token;
    5. verifies the impersonation level and the TI SID before each write;
    6. reverts after each write.
  - Hadron's own files are never written while impersonating (a thread-local guard in `store.cpp`).
- **Recovery.**
  - `restore.cmd` targets `%~d0` or `%1` only. It refuses if `Windows\System32\config\SYSTEM` is missing, and it
    never searches other drives.
  - It loads the offline hive, reads `Select\Current`, and writes `Start`/`DelayedAutostart` only for keys that
    exist. Every `reg add` and the `reg unload` are checked; a failed load also names a still-loaded HXOFF.
  - `:put` has no `( )` block: the name goes into `N` on a plain line and every branch is a `goto` (real cmd.exe
    parses a whole block first, so a `)` in a name ended it). CI runs an echo-only variant
    (`tests/golden/services/restore-dryrun.cmd`) on real cmd.exe with an extra `Intel(R) ... (x86)` row.
  - `CmdSafeName` is an allow-list (ASCII letters, digits, space, `_ - . { } # $ @ +  ~`). Every other name,
    `Intel(R) ...` included, goes to `.reg` files (README section C).
  - README section E says rows that needed TrustedInstaller may print FAILED in WinRE and Safe Mode.
  - `recovery\first` holds only what Hadron may change: the same exclusions as Default (never-touch, Boot/System
    start, storage, named never-preset, launch-protected, System-start drivers without `sysok`, and every driver
    when the hardware checks couldn't run). README section F lists the names left out.

## Spec ambiguities resolved in the build
- **Hardware evidence.**
  - Never-seen hot-plug classes are offered unticked.
  - Never-seen non-hot-plug hardware is ticked.
  - Present or phantom ("seen before") hardware is blocked as "Kept".
  - Use-based Stripped items (no hardware hint) are offered unticked with "use cannot be detected: tick if you don't use it" (kb-auditor M16, spec §5.3).
- **"No guidance"** is a keep-the-default rating (DECISIONS.md, proxy): an IoT No guidance keeps an item out of Pure bloat; a WS16 No guidance alone does not, but the pbasis says so.
- **Windows stock.** The seed has no stock values, because no measured or published table was available.
  - The plan says how many items have a stock value (0 today).
  - The mechanism is selftested with an in-memory KB.
  - Stock values are never applied to storage drivers.
- **Default (first snapshot).**
  - KB guards don't apply: it restores what the PC had.
  - Never-touch items, the named never-preset list, protected items and Boot/System targets are still blocked,
    and so is lowering a live System-start driver without `sysok`.
- **Restores compare the raw delayed value.** Default (both), Undo and Resume compare and write
  `DelayedAutostart` itself when the target is Auto, as a single change does: a 1 left behind under Manual or
  Disabled is written as 0 when the recorded value is plain Auto (absent or 0), instead of turning into Auto
  (delayed).
- **Dependency check** (spec §5.3) works on the start every item ends with if the ticked rows are applied, in
  both directions (disabling X while an enabled item depends on it; raising X while something it needs stays
  Disabled), iterates to a fixed point, and runs again after every toggle. A row it unticked is ticked again when
  the reason goes; a row the user toggled keeps the user's choice.
- **Auto (delayed)** is not offered for a service in a load-order group: Windows doesn't allow it (docs D1).
- **`--ti on`** is honoured only for fixture renders (`--fixture`, `--screenshot`); live, the toggle is off at
  every launch (spec §5.6).
- **Start/Stop/Restart** are control plans, with a confirm and a log, and no undo entry (spec §8.8).
- **Undo** restores an absent `DelayedAutostart` as 0, and the row says so.
- **Drift.** If Hadron wrote a value and Windows later changed it, the undo row is unticked with a warning.
- **TI marker.** The undo log's `T started` with no `T stopped` marks a crash while TI was running. The banner shows
  only while TI is still running.
- **List mode** has no "selected level", so the Hadron column shows the item's lowest preset level, or
  `protected` / `no preset` / `—`. Opening a preset starts on the selected item when the plan contains it.
- **`--live-test`** refuses any name not starting with `HadronTest`. It also refuses drivers, and refuses when
  not elevated.

## Deviations from the spec
- **Hardware evidence is devnode-based only** (spec §5.5; Analyze docs D3; DECISIONS.md, proxy). Two things the
  research named are not done in this run:
  - services are not mapped to hardware through their device-interface triggers (`ifaces` is read and shown but
    not used as evidence);
  - the devnode last-arrival date is not read, so "seen before" has no age.

  Both only make the evidence less specific: a hardware-bound KB entry still needs a devnode hint, and
  present or phantom hardware still keeps the item. Follow-up idea.

## Acceptance
`tests/tools/services_check.sh <build> <out>` runs:
1. the genkb selftest, plus a `cmp` of `services.db`;
2. the fixture regeneration, plus `cmp`;
3. both selftests;
4. the 8 `--dump-plan` comparisons against `tests/golden/services`, plus `plan-services-default-delayedlinger.txt`
   (the `delayedlinger` fixture);
5. the screenshots (including the `forged` Undo plan and the `multi` banner).

Regression fixtures from Analyze (written by `make_services_fixture.py`, compared by CI): `delayedlinger/` (A7),
`forged/` (A4), `multi/` (an interrupted undo of an interrupted apply). The selftest also writes and compares
`restore.cmd`, `restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt`.

CI runs it in the Wine job. The MSVC job runs:
- the selftests, the golden plans and the screenshots;
- `restore-dryrun.cmd C:` on real cmd.exe, which must reach `Done: 5 restored` with no syntax error;
- `--live-test HadronTestSvc` on a service it creates and deletes with `sc.exe`;
- the TI variant, with `continue-on-error`.
