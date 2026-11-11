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
- **Custom presets** (fix `svcdrv-presets`): the plan's ticks are saved under a name, listed on their own page, and
  opened like a built-in preset. They are exported and imported as plain text. An opened or imported custom preset only
  fills a plan; the apply, snapshot and undo path is the same one every plan takes.
- **Measured before/after** (fix `svcdrv-presets`): Measure records RAM in use, committed memory, the process count
  and the `svchost.exe` count, with when and how long after boot. A "before" taken at plan time and an "after"
  taken after the next restart are shown side by side, labelled `owner-measured` with their conditions.

## Files
| File | Role |
|---|---|
| `model.*` | Pure data: `SvcItem`, `StartCfg` (start + delayed, where delayed is -1 when absent), names, image paths, folding |
| `kb.*` | Loads the embedded `services.db` (RCDATA `SERVICES_KB`) |
| `hw.*` | Hardware evidence. The pure part reads a dump; the live part uses CfgMgr32 over every devnode, present or not, with its last-arrival date, and SetupAPI for the devices of each device-interface class a service's arrival trigger names (`AddLiveIfaceEvidence`). `HwEvidenceEx` / `IfaceEvidence` may only keep more |
| `evidence.*` | Read-only evidence: the Authenticode signer of an item's file (`ReadSigner`, offline, cached) and the System log's event 7040 (`ReadLive7040`, `ParseEvent7040`), plus their fixture formats |
| `evidence_view.*` | The page's two evidence Worker passes (signer, event 7040), so `module.cpp` holds one member and a few hooks |
| `regfallback.*` | The registry fallback's planning side: `RegRowRefusal`, `MarkRegFallback`, the plan lines; the write is `RunApply`'s through `Backend::WriteRegStart` |
| `backend.h` | The `Backend` interface: Enumerate, Query, CanWrite, SetStart, Control, Arm/Disarm |
| `backend_live.cpp` | **The only file that writes to the SCM or the registry** (`WriteRegStart`, the registry fallback). Every write refuses until `Arm()` |
| `backend_fake.cpp` | In-memory backend for the selftest, `--dump-plan` and screenshots. Supports fault injection |
| `store.*` | The data dir, the snapshot and undo-log formats, `CREATE_NEW` writers, the apply lock |
| `recovery.*` | `restore.cmd`, `restore-online.cmd`, `README.txt` and `.reg` files, plus a stored-only zip writer |
| `plan.*` | Guards, presets, single/default/undo/resume/control plans, dependency checks, apply order, `RunApply` |
| `ti.*` | TrustedInstaller impersonation (B6). Token work only; it starts and stops TI through the `Backend` |
| `custom.*` | Custom presets: the `HXPRESET` text format, its strict parser, the `presets\` store (save, list, tombstone remove), export and import |
| `measure.*` | Measurements: `TakeMeasurement` (GlobalMemoryStatusEx and a Toolhelp32 walk), the `measure` record, the `measure\` store and before/after pairing |
| `module.cpp` | The UI (`hx::Module`) |
| `selftest.cpp`, `cli.h` | `--selftest`, `--dump-plan` and `--live-test` |
| `selftest_evidence.*` | The selftest part for follow-ups 6-9 (signer, 7040, interface evidence, registry fallback), on the fake backend |
| `standalone.cpp` | `HadronServices.exe`. Intercepts `--dump-plan` and `--live-test` before the GUI starts |
| `tools/services/kb.py`, `genkb.py` | The KB source and the rule-7 gate (see `tools/services/README.md`) |
| `tests/fixtures/services/` | The fixture machine (`make_services_fixture.py` writes it; CI compares), with `signers.txt`, `events7040.txt`, `sxs\`, `presets\`, `measure\` and `import\` |
| `tests/golden/services/` | 10 plan goldens (`plan-<services\|drivers>-<default\|bloat\|stripped\|minimum\|stock>.txt`), `plan-services-default-delayedlinger.txt`, `plan-services-custom.txt` (the fixture's "Gaming rig"), `restore.cmd`, `restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt` |

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
                                          literal word `start`; identity is `ti`, `admin` or `reg` (planned;
                                          `reg` = the registry fallback)
T  starting                              flushed BEFORE Hadron may start the TrustedInstaller service (R22)
T  started pid pstart                    Hadron's own request started it: its pid and process creation time
T  notstarted                            it was running already, or someone else started it: not Hadron's
T  stopped | stopfailed text             Hadron stopped it again / could not (with the reason)
I  seq observed identity                 flushed BEFORE the SCM call (or the registry write); identity is the
                                          planned `ti|admin|reg`, not a verified one. For `reg`, observed is the
                                          key's own Start/DelayedAutostart read just before
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
- `N` lines may carry a 10th field, the devnode's last-arrival date (`DEVPKEY_Device_LastArrivalDate`, a UTC
  FILETIME in hex); a node without one keeps 9 fields.
- `I guid`: a device-interface class with a device present now. `F guid devId`: a device (present or not) that
  registered an interface of that class; written in that order (V, B, C, I, F, N), so a dump round-trips.

**Signers** (fixture `signers.txt`, kind `signers`): `G name state subject detail`, state one of `signed`,
`catalog`, `unsigned`, `untrusted`, `cantcheck`. Live data never goes to a file.

**Events** (fixture `events7040.txt`, kind `events`): `A sid account` (how the fixture resolves a SID) and
`E <event XML>` (one rendered System-log event per line, bios-escaped).

**Custom preset** (`presets\<yyyyMMdd-HHmmss>.txt`, and every export/import file). Its own format, because the user
edits and shares it, so it has no escaping and a strict reader (`custom.cpp`, `ParsePreset`):
```
HXPRESET<TAB>1
hadron<TAB><Hadron version>
build<TAB><Windows build>
tab<TAB>services|drivers
name<TAB><preset name>
<item><TAB>auto|auto-delayed|manual|disabled      one line per item
```
- UTF-8 (a BOM is accepted), LF or CRLF, trailing empty lines allowed. Written with LF, no BOM.
- Refused (a line's error starts `line N: ...`; the whole-file ones say what is wrong): a different magic or a version other than 1; a missing or misordered header line;
  a line over 512 bytes; invalid UTF-8; a field count other than 2; a start type outside the four words (`boot` and
  `system` get a note that Hadron never sets them); `auto-delayed` on the drivers tab; a duplicate item; an empty line
  between items; an item name that is empty, over 256 characters, or holds a control character, `\` or `/`, or
  starts or ends with a space; a preset name that is empty, over 80 characters, holds a control character, or
  starts or ends with a space; more than 4096 items; an empty file, no items, or a file over 1 MiB.
- `ImportPresetFile` opens one handle on the entry itself (`FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS`)
  and reads its attributes from that handle (`FileAttributeTagInfo`) before any byte. A folder, a link (reparse point,
  so a link to a placeholder can't trigger a recall), or a file with `RECALL_ON_DATA_ACCESS`, `RECALL_ON_OPEN` or
  `OFFLINE` (a cloud placeholder) is refused unread (hard rule 1). It reads in a loop up to one byte past 1 MiB.
- Removing a preset writes a tombstone `presets\removed-<stamp>.txt` (`HXPRESET-REMOVED\t1`, then the file name);
  no file in the data folder is ever deleted. The list skips tombstoned files.
- Export opens with `CREATE_NEW`. If the file exists, it says so and writes nothing; the user picks another name.

**Measurement** (`measure\measure-<yyyyMMdd-HHmmss>[-N].txt`, kind `measure`), in the shared `HXSVC` format:
- `M` lines: `boot` (UTC boot time, from now minus `GetTickCount64`), `build`, `created` (UTC), `machine`, `phase`
  (`before` or `after`), `plan` (the plan's title, or empty), `uptime` (whole minutes after boot).
- `V` lines: `ram_used` and `ram_total` (`ullTotalPhys - ullAvailPhys`, `ullTotalPhys`: the physical memory Windows
  can use, a little less than installed), `commit` and
  `commit_limit` (`ullTotalPageFile - ullAvailPageFile`, `ullTotalPageFile`), `processes` and `svchost` (a Toolhelp32
  snapshot; `svchost.exe` by image name, case-insensitive). Bytes for memory.
- The reader refuses a record that is missing any of these values.
- **Pairing:** the newest `after`, with the newest `before` of the same machine taken before that after's boot. "Measure
  after" is offered only when the newest before was taken before the current boot.

## Safety mechanics (hard rules 1, 4 and 5; spec §5)
- **Write path.** `ChangeServiceConfigW` and `ChangeServiceConfig2W` (delayed flag), `StartServiceW`,
  `ControlServiceExW` and `RegSetValueExW` (the registry fallback) appear only in `backend_live.cpp`. Each refuses
  until `Arm()`. A stop whose reason code `ControlServiceExW` refuses (`ERROR_INVALID_PARAMETER`) is retried with
  plain `ControlService`.
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
- **Registry fallback** (follow-up 9; `regfallback.*`, `RunApply`, `backend_live.cpp` `WriteRegStart`). For start
  types the SCM refuses to administrators (`CanWrite` denied, e.g. a service whose security descriptor withholds
  `SERVICE_CHANGE_CONFIG`):
  - Off by default, behind the plan screen's "Registry fallback" switch (Console F9; F7 and F8 are Save as preset and Measure before). The switch is per apply: never
    persisted, off at every launch, and off again when the plan closes or an apply ends. It is shown only on a plan
    with rows the SCM refuses (or registry rows to undo). With the TrustedInstaller toggle on, TI's SCM write is used
    instead.
  - Each such row gets its own plan line: "SCM refused; will write registry Start directly, takes effect after
    restart". Nothing is written until Apply.
  - Never for: a per-user instance, an item whose configuration can't be read, never-touch items (`CheckGuard`: the
    floor, the boot path, filters, Critical, Boot start), launch-protected services (`protect != 0`, read again from
    the SCM inside `WriteRegStart`, failing closed), the named never-preset set (Defender/Security, TrustedInstaller,
    VSS, swprv, the file-system filters), Boot/System-start drivers, a service whose registry Start is Boot or System,
    a Boot or System target, a key with no `Start` value, and a key whose Start already differs from the SCM's (a
    change waiting for a restart). `RegRowRefusal` is checked when the plan is built and again on the live item at
    write time.
  - Same apply order as an SCM row: the pre-apply snapshot, recovery folder and zip, `B` and every `P` (identity
    `reg`) are written first. Per row: the key's Start and DelayedAutostart are read again right before the write and
    the row is skipped if they differ from the plan's "now"; the `I` record with those old values is written and
    flushed (`FlushFileBuffers`); then `WriteRegStart`; then the key is read back, and anything other than what was
    written is a `D fail` ("Undo puts back the value recorded before"). There is no automatic rollback, as for an SCM
    row.
  - `WriteRegStart(name, op, expect)`: the launch-protection check through the SCM first, then the key is opened and
    its values are read again under the write handle; unless they are exactly `expect` (the values logged in the `I`
    record; raw, so an absent DelayedAutostart is not 0) it refuses, "changed since the re-read", writing nothing.
    Then DelayedAutostart (when the op sets it) and `Start`, `REG_DWORD`. If `Start` fails after DelayedAutostart was
    written, DelayedAutostart is put back on the same handle (deleted with `RegDeleteValueW` when it was absent before,
    the only value delete in Hadron). Then `RegFlushKey`; a failed flush is noted in the `D` text and the write still
    counts, because the values are in the live hive and the read-back decides.
  - `WriteRegStart` opens `HKLM\SYSTEM\ControlSet00N\Services\<name>` (N from `Select\Current`, not through the
    `CurrentControlSet` link) with `REG_OPTION_OPEN_LINK`, refuses a key with a `SymbolicLinkValue` (a link is a
    leaf), a name with `\` or `/`. It never creates a key or a `Start` value; DelayedAutostart may be created (as 0
    when Undo restores an absent one, which has the same effect, or as the planned value).
  - "Takes effect after restart" is reasoning (the service manager reads `Start` when Windows starts), not tested:
    the plan note, the banner and the row detail say so, and to check it after a restart (Windows Update Medic can put
    some of these back).
  - Undo and Resume of a `reg` row go back through the registry, read "now" from the key, and need the switch on too
    (left out with a note while it is off). Drift on the list is judged against the registry Start for an item whose
    last write was a registry write, until a restart makes the SCM agree.
  - The selftest exercises all of it on the fake backend only (its in-memory registry); the one live call is an
    unarmed `WriteRegStart`, refused before the registry is opened.
- **Signer and event 7040 are read only.** The signer pass opens each file with
  `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS` only after its path passed the Company lookup's
  checks (a local drive, not a network drive, no reparse point on the path, no `RECALL_ON_DATA_ACCESS`,
  `RECALL_ON_OPEN` or `OFFLINE`), checks the attributes again on the handle, and verifies with `WTD_REVOKE_NONE` and
  `WTD_CACHE_ONLY_URL_RETRIEVAL` (no network). The event pass is `EvtQuery` on the System log, newest first.

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
     template. The result is cached in the data folder as `sxs-v<scanner version>-<build>.<UBR>-<EditionID>.txt` (temp + rename, never
     overwritten), only after `EnsureRoot` passes again on that worker (A1); an unusable cache is renamed aside and
     the manifests are read again. Basis: `owner-measured`, condition "read from this PC's WinSxS component
     manifests, Windows build N.UBR (edition)".
  2. **Hadron's KB** (19045 only): five Microsoft-page values (`STOCK`) and the values where two third-party tables
     agree (`stock_src` `table`); `stock_disagree` where the tables differ (tools/services/README.md).
  - How they combine (`StockFor` in plan.cpp): manifest and KB agree, or only the manifest has a value: applied
    from the manifest (the KB source is named when it agrees). Only the KB has one: applied from the KB. They
    disagree, the tables disagree with each other, or only a setting default gives the manifest value (setting
    defaults can be Server values, e.g. Themes) and no KB value agrees: shown with both sources, never applied.
    Services the probe flagged as changed after setup (a probe-session judgement, not a source: start type for
    WlanSvc, stisvc, camsvc, IKEEXT, PolicyAgent, iphlpsvc, ssh-agent, WinHttpAutoProxySvc; DelayedAutostart for BITS,
    WinRM, dcsvc, dmwappushservice) are always shown, never applied. So are manifest values from an incomplete scan
    (failed or skipped manifests, a cut-short listing) and, when the edition file can't be read, values its settings
    can set; such scans are never cached. While the pass runs the stock plan is empty ("still reading this PC's
    component manifests") and can't be applied. An applied manifest row keeps rule 7's `owner-measured` kind but is
    labelled "read on this PC by Hadron (component manifests, build N.UBR, edition)"; fixture rows say they come
    from the test fixture's hand-written manifests. Drivers take a manifest value only when their KB entry has
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

- **Custom presets (fix `svcdrv-presets`).**
  - *The guard.* A custom preset gets the full built-in preset guard: `CheckGuard`'s never-touch and never-preset items,
    items Hadron's KB doesn't know (third-party, as for built-in presets: e.g. `Fax` and `nvlddmkm` on the fixture), and
    drivers the KB doesn't mark safe to change. Each is dropped with one note naming them ("item(s) that every preset
    leaves alone"). Unlike a built-in preset, a custom one may raise a start type (it is the user's own list).
    A stricter start for an item whose KB entry names hardware gets `BuildPreset`'s hardware check (review R2): kept
    (blocked) while the hardware is present or was seen, unticked for a hot-plug class never seen, kept without
    hardware information; the row shows the evidence.
  - *Rows Hadron can't do.* Missing items, items of the other tab and unreadable items are counted in notes. A build
    other than the PC's adds a note. TrustedInstaller-owned rows are blocked unless the TI toggle is on, as in every plan.
  - *Where a preset comes from.* "Save ticks as custom preset" on a preset or single plan saves the included,
    unblocked rows' targets (a plan's row set, not the whole tab). An import opens straight into a plan; to keep it,
    save it from that plan. The undo log's `B` source is `custom:<tab>:<name>`, shown as `the custom preset "<name>"`.
  - *Names.* A duplicate name (case-insensitive) is refused at save; the default name is `<plan> (<Tab>)`.
  - *Removing* writes a tombstone instead of deleting a file (hard rule 1 applies to Hadron's own data too).
  - *Export* never replaces: `CREATE_NEW` only, without an overwrite prompt, so there is one path and no race.
  - Saving, removing, exporting and importing run on a `Worker`; the Open/Save dialogs run on the UI thread before it
    starts. Writes are off in fixture mode, read-only mode, without elevation, and when the data folder was refused (A1).
- **Measure (fix `svcdrv-presets`).**
  - The only numbers on the page are recorded values and their difference (`After - before`, computed from the two
    records). No estimate anywhere; the page says Hadron never predicts what a change saves.
  - "Measure after" requires a before from an earlier boot; the banner offering it shows only in live mode and can
    be dismissed for the session.
  - The fairness text is "test it" guidance without numbers: same apps, same minutes after boot, repeat the pair,
    and test frame rates in the game itself.
  - Measurements of another machine (machine GUID) are never paired. A pair whose minutes after boot, Windows build
    or usable physical memory differ is shown with a warning saying so.
  - "Between them" (review R6) names every Hadron apply of that machine whose undo log has a `B` record and at least
    one row, stamped after the before was recorded and before the after's boot (`AppliesBetween`). When there is
    none it says "No Hadron apply was recorded between these two: the difference is not from a Hadron change."
  - The basis reads "owner-measured (measured on this PC by you)".

## Deviations from the spec
- **Hardware evidence** (spec §5.5; Analyze docs D3; DECISIONS.md, proxy). The devnode-only deviation is closed by
  follow-up 8 (docs/pipeline/fixes/svcdrv-evidence.md):
  - a service's device-interface arrival triggers (`SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL`, the `ifaces`
    field) are mapped to the devices that registered that interface class (SetupAPI, present or not). A match keeps
    the item: a hinted row the hint alone would offer, and a Stripped/Minimum row with no hint whose use "cannot be
    detected". It never frees a row;
  - the devnode last-arrival date (`DEVPKEY_Device_LastArrivalDate`) is read and shown with "present" and "seen
    before" ("last arrived 2026-03-02"). It is text only: an old date never loosens a keep.
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
| `--presets` | module | Open the custom presets page |
| `--import <file>` | module | Import that file as the Import button does (no dialog): a refusal shows on the presets page, a valid file opens its plan |
| `--plan custom:<name>` | module | Open the fixture's saved custom preset of that name as a plan |
| `--save <name>` | module | With `--plan`: open the "save ticks as custom preset" form with that name typed (fixture mode refuses the write) |
| `--measure` | module | Open the measure page (the fixture's `measure\` pair) |
| `--ti on` | module | Start with the TrustedInstaller toggle on (fixture renders only; live it is off at every launch) |
| `--regfallback on` | module | Start with the registry fallback switch on (fixture renders only; live it is off at every launch and after every apply) |
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
`--update-golden` rewrites the 10 plan goldens, `plan-services-default-delayedlinger.txt`, `plan-services-custom.txt`, `restore.cmd`,
`restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt`. Then run `tests/tools/services_check.sh`, which
compares them again (with `--dump-plan`) and fails on any difference.

## Acceptance
`tests/tools/services_check.sh <build> <out>` runs:
1. the genkb selftest, plus a `cmp` of `services.db`;
2. the fixture regeneration, plus `cmp`;
3. both selftests;
4. the 10 `--dump-plan` comparisons (5 plans on each tab, `stock` included) against `tests/golden/services`, plus
   `plan-services-default-delayedlinger.txt` (the `delayedlinger` fixture);
5. the screenshots (including the `forged` Undo plan and the `multi` banner, and the presets page, the import
   refusal, the custom plan, the save form and the measure page in both layouts).

Regression fixtures from Analyze (written by `make_services_fixture.py`, compared by CI): `delayedlinger/` (A7),
`forged/` (A4), `multi/` (an interrupted undo of an interrupted apply).
Hand-written fixtures for custom presets and measurements (not generated, and not compared by step 2): `presets/`
(two saved presets), `import/bad-start.txt` (refused at line 8) and `measure/` (a before/after pair). The selftest
compares the custom plan with `plan-services-custom.txt`. The selftest also writes and compares
`restore.cmd`, `restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt`.

CI runs it in the Wine job. The MSVC job runs:
- the selftests, the golden plans and the screenshots;
- `restore-dryrun.cmd C:` on real cmd.exe, which must reach `Done: 5 restored` with no syntax error and skip the injected row whose Start reads 0 (Boot);
- `--live-test HadronTestSvc` (in the test build) on a service it creates and deletes with `sc.exe`;
- the TI variant, with `continue-on-error`.
