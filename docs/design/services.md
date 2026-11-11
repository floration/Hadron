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
- Offers five presets: "Default: as first seen", "Default: Windows stock", Pure bloat removal, Stripped and Minimum.
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
| `backend_fake.cpp` | In-memory backend for the selftest, `--dump-plan` and screenshots. Supports fault injection |
| `store.*` | The data dir, the snapshot and undo-log formats, `CREATE_NEW` writers, the apply lock |
| `recovery.*` | `restore.cmd`, `restore-online.cmd`, `README.txt` and `.reg` files, plus a stored-only zip writer |
| `plan.*` | Guards, presets, single/default/undo/resume/control plans, dependency checks, apply order, `RunApply` |
| `ti.*` | TrustedInstaller impersonation (B6). Token work only; it starts and stops TI through the `Backend` |
| `module.cpp` | The UI (`hx::Module`) |
| `selftest.cpp`, `cli.h` | `--selftest`, `--dump-plan` and `--live-test` |
| `standalone.cpp` | `HadronServices.exe`. Intercepts `--dump-plan` and `--live-test` before the GUI starts |
| `tools/services/kb.py`, `genkb.py` | The KB source and the rule-7 gate (see `tools/services/README.md`) |
| `tests/fixtures/services/` | The fixture machine (`make_services_fixture.py` writes it; CI compares) |
| `tests/golden/services/` | 10 plan goldens (`plan-<services\|drivers>-<default\|bloat\|stripped\|minimum\|stock>.txt`), `plan-services-default-delayedlinger.txt`, `restore.cmd`, `restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt` |

## Formats
All formats share these rules:
- UTF-8, LF line ends, TAB-separated fields, with bios escaping (`\` doubled, TAB becomes a space, CR is dropped,
  LF becomes `\n`).
- Line 1 is `HXSVC\t1\t<kind>`.
- Unknown lines are skipped, and there is a 16 MB cap.

**Snapshot** (`first.txt`, `snap-<stamp>.txt`, fixture `snapshot.txt`, kind `snapshot`):
- `M key value` meta lines, sorted by key:
  - `kind`: `first`, `baseline (not first run)` (a corrupt first.txt was renamed aside), `pre-apply`
    (`snap-<stamp>.txt`) or `fixture`;
  - `created`: in `first.txt` the **local** time `yyyy-MM-dd HH:mm`; in `snap-<stamp>.txt` the apply's **UTC**
    stamp (`yyyyMMdd-HHmmss`);
  - `build` and `machine` (the machine GUID);
  - `source`: only in `snap-<stamp>.txt`, the plan source (as in the undo log's `B`).
  (`store.h` also names a `hadron` key; nothing writes it.)
- One `S` line per item, 22 fields: name, display, type (hex), cfg (`2/1`, `3/-`), trigger, state, errctl,
  account, image, group, deps (`;`-joined), description, regStart, protect, controls (hex), pid, failure,
  triggers, interfaces (`;`-joined), company, readError.
- `X name acl=ti` lines exist only in fixtures: they make the fake backend refuse an admin write.

**Undo log** (`undo-<stamp>.log`, kind `undo`). Every record ends with TAB and the CRC-32 (`%08X`) of the text
before it. A record with a bad CRC (a torn write) is ignored.
```
B  stamp source count machine build order  begin; source = preset:<lvl>:<tab> | single | default-first:<tab> |
                                          default-stock:<tab> | undo:<stamp> | resume:<stamp> |
                                          dismiss:<stamp> | ti-resolved:<stamp>
P  seq name start before after identity  every plan row, written before any change; the 4th field is the
                                          literal word `start`; identity is `ti` or `admin` (planned)
T  starting                              flushed BEFORE Hadron may start the TrustedInstaller service (R22)
T  started pid pstart                    Hadron's own request started it: its pid and process creation time
T  notstarted                            it was running already, or someone else started it: not Hadron's
T  stopped | stopfailed text             Hadron stopped it again / could not (with the reason)
I  seq observed identity                 flushed BEFORE the SCM call; identity is the planned `ti|admin`,
                                          not a verified one
D  seq ok|skip|fail afterObserved text   flushed after the call and a re-read; on `ok` the text is the
                                          identity the write ran as, otherwise the reason
N  text                                  a resolution record: what happened, in words
E  stamp ok skipped failed done|cancelled
```
(`store.h` documents the same records; `plan.cpp` `RunApply` and `WriteResolution` write them.)
- **Resolution logs** have no rows: `B` with source `dismiss:<stamp>` (the user dismissed that interrupted log,
  R9) or `ti-resolved:<stamp>` (that log's TrustedInstaller claim is closed, R4), one `N`, then `E`. Logs are never
  appended to.
- Start/Stop/Restart (control plans) write no undo log: one host log line per operation (spec §5.2).
  - A Stop or Restart whose stop fails starts again, newest first, what that run already stopped (R13), and never
    goes on to start the target. That clean-up is sent even after Cancel and waited for at most 10 s; a start the
    SCM accepted but didn't confirm in time is reported as "requested, not confirmed", not as a failure to send.
  - The TrustedInstaller banner's "Stop it" is a control plan too. It stops TI only while TI is still the process
    Hadron started (pid and creation time, checked again just before the stop), and ends by writing a
    `ti-resolved:<stamp>` log either way (R4).
- Stamps are UTC (`yyyyMMdd-HHmmss`, `-N` when taken). `order` is 1 + the highest `order` of the logs already
  there, so logs sort by `order`, then by stamp with `-N` compared as a number; a clock change can't reorder them.
  Logs without `order` (none were ever shipped) sort first. Orders are capped at 1,000,000 (`kOrderCap`): a log
  claiming more sorts as 1,000,001 and the next log takes at most that, so a forged huge `order` ties with the logs
  written after it and the stamp decides, instead of staying newest for good (mini-analyze A4, fixture run 4).
- Banners, plan titles and the undo plan name an apply by its start time as "YYYY-MM-DD HH:MM UTC" (with "-N"
  when the stamp has one), not the raw stamp.
- A log with `B` and no `E` is **interrupted**. The page offers Undo (the rows with `I`), Resume (the rows
  without `D`) or Dismiss. Dismiss writes a `dismiss:<stamp>` resolution log (R9): the banner goes, and the
  dismissed log stays undoable from "Undo last apply" (mini-analyze A2). A Dismiss pressed while a load runs is kept
  and recorded by the next load. Dismissing an interrupted resume settles the apply it was resuming too.
- A finished `undo:<stamp>` or `resume:<stamp>` log resolves that log, wherever it sorts. A finished
  `resume:<U>` where U is `undo:<X>` resolves X too. A finished `dismiss:<stamp>` settles that log, and through
  its resume chain the logs it was resuming, for the banner only (Undo still reaches it). An unfinished undo or
  resume of a resolved log is resolved.
- For an interrupted `undo:<X>` log the banner says so: Undo starts again from X's log (never "undo the undo",
  which would re-apply X), Resume finishes the undo from its own log.
- Logs from an undo are never offered for undo themselves.
- Undo and Resume refuse a log whose machine GUID or build differs from the PC's (empty plan, reason shown), and
  show how many records failed their CRC and were ignored, and a planned-vs-read row count mismatch.

**hw dump** (fixture `hw.txt`, kind `hw`): devnodes with class, service, filters, the present flag and the
parent; class filters; the boot-disk chain; and the hypervisor and VM flags.

## Safety mechanics (hard rules 1, 4 and 5; spec §5)
- **Write path.** `ChangeServiceConfigW` and `ChangeServiceConfig2W` (delayed flag), `StartServiceW` and
  `ControlServiceExW` appear only in `backend_live.cpp`. Each refuses until `Arm()`. A stop whose reason code
  `ControlServiceExW` refuses (`ERROR_INVALID_PARAMETER`) is retried with plain `ControlService`.
  - The UI arms the backend after the user confirms a plan, and disarms it when the apply ends.
  - Selftest and screenshot modes use the fake backend.
  - CI fails on `DeleteService`, `CreateService`, `RegDeleteKey`, `RegDeleteTree` or a firmware-write API under
    `modules/services/`.
- **Apply order** (`RunApply`). If anything in steps 1–3 fails, nothing changes.
  1. Take the lock: named mutex `Global\Hadron.services.apply`, plus in-process tracking, because a mutex is
     recursive for its owner. Check the data folder again (`EnsureRoot`). Every apply except an Undo then refuses
     with no `first.txt` (R12), or with one that can't be read or whose machine GUID isn't this PC's (mini-analyze
     A6): Hadron would have no record of how this PC started.
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
  With the TrustedInstaller toggle on, `T starting` is written and flushed before TI may be started (R22), then
  `T started pid pstart` or `T notstarted`.
  6. Write `E`, and stop TI if this apply started it (`T stopped` or `T stopfailed`). From the moment the TI token
     is taken, every return path (a failed log write or flush included) puts TI back the same way. That stop is
     sent even after Cancel and waited for at most 10 s (mini-analyze A5); one the SCM accepted but didn't confirm
     is noted as "requested, not confirmed".
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

  System-start drivers are not on that list (spec §5.2), but every plan treats them alike (R6): a single change,
  preset, Default or Undo row may lower one only with a curated `sysok` entry, checked against the live start
  type. Without it the row is blocked: Hadron could never put that System start back (Undo, a single change and
  Default all refuse to set System start without `sysok`). Hadron never *sets* Boot or System start, except an
  Undo putting back a System start it changed under `sysok`.

  These items are never in a preset:
  - protected (PPL) services;
  - the Defender/Security stack, TrustedInstaller, VSS and swprv (spec §8.4).

  Preset rows also need a KB entry for this build, and a driver needs `sysok`. A preset only lowers: an item
  already stricter than the level (Disabled where it says Manual) is left alone and counted in a note.

  Undo and Resume rows pass the same guards on the live item (a log has a CRC, not an author): never-touch
  items, Boot start, Boot/System on a service and a System-start driver without `sysok` are blocked.

  An item whose configuration could not be read (R2) is shown as "unknown (could not be read)", never as a start
  type, and is in no plan: every guard refuses it, Default skips it with a note, and the first-run recovery leaves
  it out.
- **Data dir.** `%ProgramData%\Hadron\services\`, owner Administrators, protected DACL (Administrators + SYSTEM).
  - `EnsureRoot` runs at every load and every apply, whoever made the folder. No component below the volume or
    share root may be a reparse point. The root is opened with `FILE_FLAG_OPEN_REPARSE_POINT |
    FILE_FLAG_BACKUP_SEMANTICS` and checked again; its owner must be Administrators or SYSTEM (a standard user can
    create folders under ProgramData).
  - Hadron sets permissions only on a root it creates (`kRootSddl`, the protected admin-only DACL). It never
    changes the security of a folder that already exists (R1: `--data-dir C:\ProgramData` would otherwise have
    re-secured it, children included). An existing root must already be admin-only, or it is refused.
  - Every folder above the root, from the volume or share root down (mini-analyze A1), is opened as itself and read
    by handle before anything inside it is created or opened: its owner must be Administrators, SYSTEM or
    TrustedInstaller, and its DACL must let no one else rename, delete or re-permission it (`DELETE`,
    `FILE_DELETE_CHILD`, `WRITE_DAC`, `WRITE_OWNER`, generic all; inherit-only entries grant nothing there and are
    skipped). Whoever can do that could swap the data folder for their own. So a `--data-dir` under a user profile
    (`%TEMP%`, Documents) is refused; the default under `%ProgramData%` passes.
  - If any of that fails, nothing is read from or written to the folder: no first.txt, no undo logs, changes off,
    and a banner gives the reason. Hadron never changes these permissions itself.
  - Missing parents are created up to the volume or share root (UNC and `\\?\` roots included), each with owner
    Administrators, and the root with the protected descriptor.
  - Test hooks: Wine can't set a folder owner or make a reparse point, so `Store::AcceptTokenUserAsOwner()` and
    `Store::InjectedReparsePoint()` exist only in a build configured with `-DHADRON_SERVICES_TEST_HOOKS=ON`
    (`HX_SERVICES_TEST_HOOKS`). Release builds, and the CI builds that are not the Wine services run, have no
    such code; the Windows CI selftest runs the owner and reparse checks for real.
  - Every file is `CREATE_NEW` and flushed. Nothing is overwritten.
  - A corrupt `first.txt` is renamed aside.
  - `--data-dir` overrides the location; it is kept as a full path without the `\\?\` prefix (README shows it).
    `--readonly` never arms the backend.
- **TrustedInstaller (B6).** Off by default, and never remembered.
  - Rows the SCM refuses to an administrator (`CanWrite` reports access denied) are marked. While the toggle is
    off they are left out.
  - With the toggle on, the apply thread:
    1. starts TI only if it is STOPPED (through the Backend; STOP_PENDING is waited out first). It counts as
       "Hadron started it" only when `Control` reports that Hadron's own start request started it, not
       `ERROR_SERVICE_ALREADY_RUNNING` (Windows won a race); START_PENDING is someone else's start, waited for and
       never claimed. The claim is reset at every Acquire and Release, so a later apply never stops a TI that
       Windows started;
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
  - `:put` reads the key's current `Start` first and skips a key that is Boot or System start now (0 or 1),
    counting it in the Done line: what a script restores was judged at an earlier state, and a storage driver that
    became the boot path since must keep its value.
  - `:put` has no `( )` block: the name goes into `N` on a plain line and every branch is a `goto` (real cmd.exe
    parses a whole block first, so a `)` in a name ended it). CI runs an echo-only variant
    (`tests/golden/services/restore-dryrun.cmd`) on real cmd.exe with an extra `Intel(R) ... (x86)` row and a
    row whose `Start` reads 0, which must be skipped.
  - `CmdSafeName` is an allow-list (ASCII letters, digits, space, `_ - . { } # $ @ +  ~`). Every other name,
    `Intel(R) ...` included, goes to `.reg` files (README section C).
  - README section E says rows that needed TrustedInstaller may print FAILED in WinRE and Safe Mode.
  - `recovery\first` holds only what presets and Default may change: the same exclusions as Default (never-touch,
    Boot/System start, storage, named never-preset, launch-protected, System-start drivers without `sysok`, every
    driver when the hardware checks couldn't run, and items whose configuration could not be read). README section
    F lists the names left out, and says a single change may still reach some of them (Undo or that change's own
    recovery folder puts it back).
  - When `recovery\first` or its zip is missing or incomplete (the zip is parsed and every file's CRC and bytes
    compared with the folder), it is written again as `first-2` .. `first-9`; a partial folder is kept as it is.
    It is written only from a `first.txt` of this PC and this Windows build (mini-analyze A3): after an upgrade the
    old build's values are not what WinRE should write back, and a notice says so. README "Which folder to use"
    names `first-2` .. `first-9`.

## Spec ambiguities resolved in the build
- **Hardware evidence.**
  - Never-seen hot-plug classes are offered unticked, at every level, and the user can tick them. DECISIONS.md
    (svcdrv research row) says Stripped removes "never hot-plug classes"; Hadron reads that as never ticked by
    default, which is what 2-research.md §8 Q9 says ("never auto-removed").
  - Never-seen non-hot-plug hardware is ticked.
  - Present or phantom ("seen before") hardware is blocked as "Kept".
  - Use-based Stripped items (no hardware hint) are offered unticked at Stripped with "use cannot be detected: tick if you don't use it" (kb-auditor M16, spec §5.3). At Minimum the same rows are ticked and say "use cannot be detected: ticked at Minimum, untick if you use it" (R18, proxy Q1: Minimum is only what is needed to boot, network, play games and run Hadron); plan, confirm and undo still gate them.
- **"No guidance"** is a keep-the-default rating (DECISIONS.md, proxy): an IoT No guidance keeps an item out of Pure bloat; a WS16 No guidance alone does not, but the pbasis says so.
- **Windows stock** (fix/svcdrv-stock; DECISIONS 2026-11-11, owner). Two sources, in this order:
  1. **This PC's WinSxS component manifests** (`sxsstock.cpp`). On a Worker after the list is shown, once per
     session: every `%windir%\WinSxS\Manifests\*.manifest` ("DCM\x01" + a delta) is expanded with msdelta's
     `ApplyDeltaB` (`DELTA_APPLY_FLAG_ALLOW_PA19`) against the dictionary in the newest servicing-stack `wcp.dll`
     (resource type 614, name 1). wcp.dll is loaded `LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE`
     (no code runs); msdelta.dll by `LoadLibraryEx(LOAD_LIBRARY_SEARCH_SYSTEM32)` + `GetProcAddress`. Both are
     Windows system DLLs (rule 3). A manifest with a reparse-point, recall or offline attribute is skipped
     unopened, and the attributes are checked again on the handle (`FILE_FLAG_OPEN_REPARSE_POINT |
     FILE_FLAG_BACKUP_SEMANTICS`). Hadron's own XML scanner takes, in precedence order: (1) the edition file's
     override (`%windir%\servicing\Editions\<EditionID>Edition.xml`, `<component name><Start_X>n</Start_X>`,
     mapped through the manifest setting of that component and name); (2) `<serviceData name start>` or the
     `registryKey ...\Services\X` `Start` value (and `DelayedAutostart`); (3) a configuration-setting default
     (`<xsd:element default wcm:handler="regkey('...\Services\X')" wcm:legacyName="Start|DelayedAutostart">`).
     Two manifests with different values for a name are a conflict: no value. A per-user instance maps to its
     template. The result is cached in the data folder as `sxs-<build>.<UBR>-<EditionID>.txt` (temp + rename, never
     overwritten), only after `EnsureRoot` passes again on that worker (A1); an unusable cache is renamed aside and
     the manifests are read again. Basis: `owner-measured`, condition "read from this PC's WinSxS component
     manifests, Windows build N.UBR (edition)".
  2. **Hadron's KB** (19045 only): five Microsoft-page values (`STOCK`) and the values where two third-party tables
     agree (`stock_src` `table`); `stock_disagree` where the tables differ (tools/services/README.md).
  - How they combine (`StockFor` in plan.cpp): manifest and KB agree, or only the manifest has a value: applied
    from the manifest (the KB source is named when it agrees). Only the KB has one: applied from the KB. They
    disagree, the tables disagree with each other, or only a setting default gives the manifest value (setting
    defaults can be Server values, e.g. Themes) and no KB value agrees: shown with both sources, never applied.
    Services Windows sets at runtime from hardware or software (WlanSvc, stisvc, camsvc, IKEEXT, PolicyAgent,
    iphlpsvc, ssh-agent, WinHttpAutoProxySvc; and BITS, WinRM, dcsvc, dmwappushservice, whose DelayedAutostart is
    written at runtime) are always shown, never applied. Drivers take a manifest value only when their KB entry has
    `sysok`; storage drivers never get one. On a build other than 19045 only the manifests give values.
  - Shown rows are unticked and can't be ticked ("Not applied: <why>"); the plan notes count them.
  - An explanation-only (`never`) KB entry doesn't block its stock row: `never` means not in a preset level.
    Never-touch items, protected services and the named never-preset list still block (DECISIONS 2026-11-11).
  - With no stock value the plan is empty, with no Apply button and no sum, and never a bare "Nothing to change."
    (R20, proxy Q2). Without manifests on a build the KB has no values for: "Hadron has no Windows-stock data for
    build N yet: nothing will be changed." and the chip "(no data for this build yet)". A tab with no value says
    "Hadron's KB has no Windows stock value for any item on this tab" (or "Hadron has no ..." when manifests were
    read) and the chip "(no data on this tab)". The detail's Windows stock line gives the value and its source
    ("this PC's component manifests, build N.UBR", "two agreeing third-party tables", "the page names no build"),
    "not applied: <why>" with both sources, "still reading this PC's component manifests", or the chip's words.
  - Tests: the scanner and edition mapping on hand-written XML in the selftest; the fake backend reads
    `tests/fixtures/services/sxs/` (hand-written manifests in the decompressed shape, an edition file, build and
    edition). Live decompression is checked on a real Windows PC only, never under Wine.
  - Stock values are never applied to storage drivers.
- **"Default: as first seen"** (the first-run snapshot).
  - KB guards don't apply: it restores what the PC had.
  - Never-touch items, the named never-preset list, protected items and Boot/System targets are still blocked,
    and so is lowering a live System-start driver without `sysok`.
  - It refuses (an empty plan with the reason) a `first.txt` with no machine GUID, from another PC, or from another
    Windows build (R11: an upgrade sets new start types). Its first note says what it restores and that it is not
    Windows' default; a `baseline` first.txt (a corrupt one was renamed aside) is named as recorded after earlier
    Hadron changes (R12, Q3). Items unreadable now or recorded as unreadable are skipped with a note (R2).
  - Each row's basis reads "Hadron's first-run snapshot of this PC (<created>), read from its service manager".
- **Restores compare the raw delayed value.** Default (both), Undo and Resume compare and write
  `DelayedAutostart` itself when the target is Auto, as a single change does: a 1 left behind under Manual or
  Disabled is written as 0 when the recorded value is plain Auto (absent or 0), instead of turning into Auto
  (delayed).
- **Dependency check** (spec §5.3) works on the start every item ends with if the ticked rows are applied, in
  both directions (disabling X while an enabled item depends on it; raising X while something it needs stays
  Disabled), iterates to a fixed point, and runs again after every toggle. A row it unticked is ticked again when
  the reason goes; a row the user toggled keeps the user's choice, and a row the user ticks while it has a warning
  stays ticked with the warning until the warning goes.
  - Every plan source runs it: presets, Default, stock, Single, Resume and Undo, and `MarkTi` runs it again after
    leaving rows out for TrustedInstaller (a row left out keeps its item at its current start). A TI toggle change
    rebuilds the plan, so it runs again then too.
  - Plans that both raise and lower can go round in a circle (raising Y needs X; another row disables X; each
    warning exists only while the other row is ticked). When a state repeats, or at the round bound, it finishes
    with untick-only rounds, which always settle, and adds a plan note ("The dependency check did not settle ...").
    Rows left unticked that way say so.
  - Invariant (selftest, including Analyze's fuzzer with its seed and a richer run with groups, user instances,
    blocked rows and toggle sequences): no ticked row disables an item that an item enabled at the end depends on,
    and no ticked row raises an item whose dependency ends Disabled, except a row the user ticked over its shown
    warning.
  - Dependents and group members are indexed once per run; a round costs rows x (dependents + dependencies).
- **Auto (delayed)** is not offered for a service in a load-order group: Windows doesn't allow it (docs D1).
- **`--ti on`** is honoured only for fixture renders (`--fixture`, `--screenshot`); live, the toggle is off at
  every launch (spec §5.6).
- **Start/Stop/Restart** are control plans, with a confirm and a log, and no undo entry (spec §8.8).
- **Undo** restores an absent `DelayedAutostart` as 0, and the row says so.
- **Drift.** If Hadron wrote a value and Windows later changed it, the undo row is unticked with a warning.
- **TI marker (R4).** A log claims "Hadron may have started TrustedInstaller" when it has `T started` (or
  `T starting` with no `T notstarted`) and no `T stopped`, and no `ti-resolved:<stamp>` log closes it. The banner
  ("TrustedInstaller was left running") shows only while TI runs as the very process Hadron started: same pid and
  process creation time. A claim whose process is gone (TI stopped, or another pid or creation time) is closed at
  load with a `ti-resolved:` log; that is decided again under the apply lock after re-querying TI and re-reading
  the logs, so a concurrent apply's fresh claim is never closed (mini-analyze A4).
- **List mode** has no "selected level", so the Preset column (`KbVerdict`, the same tests `BuildPreset` runs)
  shows, in this order: `protected` (a never-touch guard); `not in KB` (not in Hadron's KB); `no preset` (a
  not-in-presets guard, a KB entry with no level, or a driver without `sysok`); `kept (hw)` (its hardware hint
  matched hardware that is present or was seen before); otherwise the first preset that lists it. That level is not a promise the preset
  changes the item: a row already at its target is skipped, and hot-plug or undetectable-use rows can be offered
  unticked. Opening a preset starts on the selected item when the plan contains it.
- **`--live-test`** exists only in a test build (`-DHADRON_SERVICES_TEST_HOOKS=ON`); a release build prints that
  it is not in this build (R25). It refuses any name not starting with `HadronTest`, refuses drivers, and refuses
  when not elevated.
- **List columns.** When the list is narrow (the suite, a small window) optional columns drop in this order:
  Type, Image path, Account, Basis, vs first, Display name, Company (Drivers: Type, Image path, Basis, vs first,
  Load group, Display name, Company); Name, State, Start type and Preset always stay. Company stays longest
  because third-party is how bloat stands out (`baseline.md` §3); Display name is in the detail pane's title. When
  "Display name" would be cut its header says "Display"; a Start type cell that doesn't fit says `+t` for
  `+trigger`; the filter's placeholder says "Filter" when the full text doesn't fit.
- **Pending marker.** A start type that waits for a restart is `(pending)` in Baseline and `!` in Console, by
  design: each layout's legend explains its own mark (Console's cells are fixed-width).

## Deviations from the spec
- **Hardware evidence is devnode-based only** (spec §5.5; Analyze docs D3; DECISIONS.md, proxy). Two things the
  research named are not done in this run:
  - services are not mapped to hardware through their device-interface triggers (`ifaces` is read and shown but
    not used as evidence);
  - the devnode last-arrival date is not read, so "seen before" has no age.

  Both only make the evidence less specific: a hardware-bound KB entry still needs a devnode hint, and
  present or phantom hardware still keeps the item. Follow-up idea.
- **TrustedInstaller is started before the token work** (2-research §B6 lists SeDebug, impersonate SYSTEM, then
  start TI). Hadron starts TI through the `Backend` as the administrator first, then enables SeDebugPrivilege and
  impersonates SYSTEM only to duplicate TI's token (reasoning: starting a service through the SCM needs no SYSTEM
  token, so the thread holds SYSTEM for less time).
- **Company** is read in a second Worker pass after the list shows (2-research:139), as the spec says: the detail
  pane says "still being read" until it is done. (Before Finalize round 2 it was read in the load pass.)

## Command line
Every flag below except `--data-dir`, `--readonly` and `--tab` acts only with `--fixture` or `--screenshot`
(fixture mode never applies anything).

| Flag | Where | What it does |
|---|---|---|
| `--fixture <dir>` | module, selftest | Load a fixture machine (`snapshot.txt`, `hw.txt`, `first.txt`, logs) instead of live data |
| `--tab services\|drivers` | module, `--dump-plan` | Start on that tab (live, a saved preference overrides it) |
| `--select <name>` | module | Select that item and centre it |
| `--plan <p>` | module | Open a plan: `bloat`, `stripped`, `minimum`, `default`, `stock`, `undo` (the interrupted log), `undo-last` (the newest undoable log), `resume`, `ti-stop` |
| `--focus list\|search\|detail\|<n>` | module | Draw that focus state, or focus the n-th tab stop (design screenshots) |
| `--sort <key>[:desc]` | module | Sort the list by a column key, e.g. `kb:desc` |
| `--keys F1,F2,text:abc,...` | module | Replay keys after the fixture loads (`F1`-`F12`, `up`, `down`, `pgup`, `pgdn`, `home`, `end`, `tab`, `esc`, `space`, `enter`, `text:<chars>`) |
| `--ti on` | module | Start with the TrustedInstaller toggle on (fixture renders only; live it is off at every launch) |
| `--data-dir <dir>` | module, `--live-test` | Use another data folder instead of `%ProgramData%\Hadron\services\` |
| `--readonly` | module | Never arm the backend. Without `--data-dir` it opens the default data folder read-only, so the first-run snapshot, undo logs and interrupted banner still show |
| `--selftest [--update-golden] [--golden <dir>]` | selftest | Run the selftest. `--golden` points at another golden folder (default `<fixture>\..\..\golden\services`); `--update-golden` rewrites every golden it compares instead of comparing |
| `--dump-stock-live [--out <file>]` | `HadronServices.exe` (Release and test builds) | Read this PC's WinSxS manifests with the live reader and write UTF-8 TSV: `#` header lines (build.UBR, EditionID, manifests, decompress failures, skipped, elapsed ms), then name, start, delayed, layer (`edition`, `serviceData`, `registry`, `settingDefault`), conflict, source file. Read only: no backend, no SCM or registry writes, no data folder, no cache. Exit 1 with the reason when wcp.dll or msdelta.dll can't be loaded or no manifest could be read |
| `--dump-plan <default\|stock\|bloat\|stripped\|minimum> --tab <t> --fixture <dir> [--out <file>]` | `HadronServices.exe` | Print a plan as golden text. `stock` has goldens `plan-<tab>-stock.txt` (Services: manifest, table and Microsoft rows and the shown-not-applied rows from the fixture's `sxs\`; Drivers: no data, the plan says so) |
| `--live-test HadronTest<name> [--ti] [--data-dir <dir>]` | `HadronServices.exe`, test build only (R25) | CI's round trip on a dummy service (see below) |

**Regenerating goldens** (only when a change is meant to alter them, and review every diff):
```
python3 -I tools/services/genkb.py                     # the KB first: plan text carries pbasis notes
wine build/HadronServices.exe --selftest --fixture tests/fixtures/services --update-golden
```
`--update-golden` rewrites the 10 plan goldens, `plan-services-default-delayedlinger.txt`, `restore.cmd`,
`restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt`. Then run `tests/tools/services_check.sh`, which
compares them again (with `--dump-plan`) and fails on any difference.

## Acceptance
`tests/tools/services_check.sh <build> <out>` runs:
1. the genkb selftest, plus a `cmp` of `services.db`;
2. the fixture regeneration, plus `cmp`;
3. both selftests;
4. the 10 `--dump-plan` comparisons (5 plans on each tab, `stock` included) against `tests/golden/services`, plus
   `plan-services-default-delayedlinger.txt` (the `delayedlinger` fixture);
5. the screenshots (including the `forged` Undo plan and the `multi` banner).

Regression fixtures from Analyze (written by `make_services_fixture.py`, compared by CI): `delayedlinger/` (A7),
`forged/` (A4), `multi/` (an interrupted undo of an interrupted apply). The selftest also writes and compares
`restore.cmd`, `restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt`.

CI runs it in the Wine job. The MSVC job runs:
- the selftests, the golden plans and the screenshots;
- `restore-dryrun.cmd C:` on real cmd.exe, which must reach `Done: 5 restored` with no syntax error and skip the injected row whose Start reads 0 (Boot);
- `--live-test HadronTestSvc` (in the test build) on a service it creates and deletes with `sc.exe`;
- the TI variant, with `continue-on-error`.
