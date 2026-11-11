# svcdrv review: Logic group, lead developer

Branch `module/services-svcdrv` at b7cc215, `git diff origin/main...HEAD`. Spec: docs/design/services.md and
docs/pipeline/svcdrv/1-idea.md. I also used the scope list in 2-research.md §3–4, because the design note points to it.
Most severe first.

## Category 1: data safety / system-breaking

### 1.1 `--data-dir` (and `--live-test --data-dir`) silently rewrites the DACL of any existing folder owned by Administrators or SYSTEM, and the change spreads to its children
- `modules/services/module.cpp:554,573` → `store.cpp:357-470` (`EnsureRoot`, `SetSecurityInfo` at `store.cpp:459`); `selftest.cpp:1765-1767`.
- **What's wrong.** Every live load runs `EnsureRoot(true)` (`module.cpp:625`), unless `--readonly` is set.
  - For an existing root, it checks only that the root is not a reparse point and that its owner is BA or SYSTEM.
  - It then applies `O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)` as a **protected** DACL.
  - `SetSecurityInfo` pushes inheritable ACEs down to every existing child whose DACL isn't protected.
  - It saves no record of the old DACL, so there is no undo.
  - The holding-folder check is skipped when the root's parent is a volume root (`i > 0`).
- **Failure scenarios.** `HadronServices.exe --data-dir C:\Users` (owner SYSTEM) or `--data-dir C:\ProgramData` (owner SYSTEM) passes every check.
  - Users, Authenticated Users, CREATOR OWNER, LocalService and NetworkService lose access to `C:\Users\Public`, `Default` and every ProgramData app folder. Apps and services break, and nothing can undo it.
  - Same with `--data-dir D:\Games`: folders an elevated process creates are owned by Administrators, so a Steam library becomes admin-only.
  - The owner has no backups (rule 5).
- **Fix.** Only (re)apply the DACL to a folder Hadron created itself, for example one holding a marker file Hadron wrote with `CREATE_NEW`.
  - For an existing folder, check that its DACL already matches, and refuse otherwise.
  - Or require the folder to be missing or empty.

## Category 2: correctness

### 2.1 Stop/Restart ignore every guard: floor and never-touch services can be stopped live
- `plan.cpp:405-442` (`BuildControl`, no `CheckGuard`); `module.cpp:2171-2175` and `2880-2884` (buttons enabled on `can && stoppable`, never `g.neverTouch`).
- **What's wrong.** The "floor" (RPC, DCOM, LSA, event log, audio, networking, BFE/MpsSvc, Winmgmt, Dhcp, CryptSvc, Hadron's own dependencies) is guarded only against start-type changes.
- **Failure scenario.** Pick BFE and press Stop. The plan lists the running dependents (MpsSvc, IKEEXT, PolicyAgent) and stops them all. The firewall is down until reboot.
  - Stopping Dhcp, Winmgmt, nsi, AudioSrv or CryptSvc the same way breaks networking, WMI or audio until reboot.
- **Fix.** Run `CheckGuard` in `BuildControl` (on the target and on each dependent row): `neverTouch` → blocked row. Disable the buttons on the same test.

### 2.2 The "TrustedInstaller left running" marker never expires, so Hadron later offers to stop a TI that Windows started
- `plan.cpp:1048-1051` (`TiLeftRunning`: any log ever with `T started` and no `T stopped`); `module.cpp:640-650` (`TakeData`: banner whenever TI is RUNNING); `module.cpp:1513-1515`.
- **What's wrong.** Nothing resolves the marker once TI has been seen STOPPED, or once a later apply finished.
- **Failure scenario.**
  1. An apply crashes after `T started`.
  2. TI idles out on its own.
  3. Weeks later Windows Update runs, TI is RUNNING, and the banner says "Hadron started TrustedInstaller … still running. … Stop it".
  4. One click stops TrustedInstaller in the middle of a servicing operation.
- This is also a false claim (cat 6).
- **Fix.** Resolve the marker the first time a load sees TI not RUNNING (that needs a record, so write a small `T resolved` file or log line). Or tie it to the interrupted log only, so it is cleared when that log is resolved.

### 2.3 After Resume, the rows the crashed apply had already changed can no longer be undone
- `plan.cpp:1039-1046` (`FindUndoable`), `999-1023` (`ResolvedBy`), `490-527` (`BuildResume`).
- **Failure scenario.**
  1. Apply A changes rows 1–5 (D ok), then crashes.
  2. The user picks Resume. Resume log R finishes rows 6–10, and A is now resolved.
  3. `FindUndoable` returns R: Undo restores only rows 6–10.
  4. After that, A is skipped (`!hasE`), so rows 1–5 are never offered for Undo. The only ways back are Default (first snapshot) or A's offline restore.cmd.
- **Fix.** Undo of a `resume:A` log should also undo A's `D ok` rows (one plan across both logs), or `FindUndoable` should offer A after R is undone.

### 2.4 "Default: restore first snapshot" doesn't check the snapshot's build
- `plan.cpp:283-310`. Only the machine GUID is compared; the GUID check is also skipped when `c.machine` is empty or first.txt has no `machine`.
- **What's wrong.** Undo and Resume refuse a log from another build (`LogRefusal`). The first-snapshot restore doesn't, and KB guards are deliberately off for it.
- **Failure scenario.** After an in-place upgrade (19045 → 22631; ProgramData survives), Default writes 19045-era start values onto the new build's services. Where Microsoft changed the stock start type, this reverts Windows' new defaults, and the plan text calls them "as recorded".
- **Fix.** Refuse (or show only, unticked) when `first.meta["build"] != c.build`, and refuse an empty machine GUID, the same way `LogRefusal` does.

### 2.5 Restart can leave dependents stopped
- `plan.cpp:955` (`RunControl`: `if (row.ctl == Op::Stop) break;`).
- **Failure scenario.** Restart X with dependents D1 and D2. D1 stops, D2's stop fails, and the loop breaks. D1 stays stopped and X keeps running. The result lists `ok stop D1`, but nothing says D1 is now down and was not restarted.
- **Fix.** On a failed stop in a Restart plan, start the rows that were stopped again, in reverse order. Or at minimum add an explicit "left stopped: …" line.

### 2.6 `FileCompany` uses a truncated expansion
- `backend_live.cpp:394-395`.
- **What's wrong.** `ExpandEnvironmentStringsW` returns the required size when the buffer is too small, and the code treats any non-zero return as success. The company column then reads the version info of a truncated path. This is display only.
- **Fix.** Loop on the returned size.

## Category 3: resources / threading

### 3.1 Cancel and Shutdown can block for up to about 60 s inside TrustedInstaller handling
- `ti.cpp:159-176`: `Ensure` polls STOP_PENDING and START_PENDING for 30 s each, ignoring cancel.
- `ti.cpp:165,185`: `Control` is called with `cancel = nullptr`, so `WaitState` waits up to 30 s more.
- **Failure scenario.** The user presses Cancel during a stuck TI start. Nothing happens for up to about a minute, and `Shutdown()` (`module.cpp:214-218`) joins the apply thread on the UI thread for the same time.
- **Fix.** Pass `env.cancel` through `TiProvider::Acquire` into `Ensure` and `Control`.

### 3.2 The TrustedInstaller toggle stays clickable during an apply in Baseline
- `module.cpp:1185-1190`. The Console layout guards it at `module.cpp:3157`; Baseline doesn't.
- **What's wrong.** A click flips `tiOn_` and starts `planW_`, which replaces `plan_` while the apply runs. The apply isn't affected, because `Job` holds a copy and `env` is copied. But the plan screen then shows a rebuilt plan for a toggle state that does not match what is being applied, and `planW_` runs concurrently with an armed backend. It only reads today, but nothing enforces that.
- **Fix.** Ignore `ID_TI` while `applyW_.Running()`, as Console does.

### 3.3 The `T started` record is written after TrustedInstaller is started
- `plan.cpp:852-854`.
- **What's wrong.** If the process crashes between `Control(Start)` and the record, TI is left running with no marker. This is low impact: TI is Manual and idles out.
- **Fix.** Write a `T starting` intent record before `Acquire`.

## Category 4: contract

### 4.1 `core/` edits the spec explicitly forbade, with no DECISIONS entry
- `core/ui/widgets.cpp`: selection-text rule (WCAG instead of Rec. 601), focus-rect colour and inset, sort-arrow geometry.
- `core/host/cmdline.{h,cpp}` and `core/host/host.cpp`: the new `--accent`.
- **Contract.** 2-research.md §3 says "Out of scope (must not touch): everything else under core/". CLAUDE.md says core changes are the lead's.
- **What's wrong.**
  - `widgets.cpp` changes the selection text and focus-rect colours in every module (bios, diskmap, runas, hello). Their screenshots shift, and no other module's goldens or screenshots were re-checked in this diff.
  - `--accent` is noted in ARCHITECTURE.md, but nothing in DECISIONS.md records either change or who approved it.
  - The history (a core change, its revert, then two more tweaks) shows it was iterated on inside a module branch.
- **Fix.** Split these into a `core/ui-…` branch with its own review and DECISIONS entry, or get the lead's sign-off recorded before merge.

### 4.2 `--live-test` ships in the release exe and is a CLI write path with no plan or confirm UI
- `standalone.cpp:18-24`, `selftest.cpp:1750-1818`.
- **What's wrong.** It is limited to names starting `HadronTest` (case-sensitive) and to non-drivers, so the blast radius is small. But it arms the live backend from the command line, can run the TI path, and accepts `--data-dir` (see 1.1).
- **Fix.** Compile it only into a CI or test-hook build, or at least drop `--data-dir` and always use a fresh temp dir.

### 4.3 A third private CRC-32 and zip writer
- `model.cpp` (`Crc32`), `recovery.cpp` (`MakeZip`/`ParseZip`), alongside the ones in `modules/bios/snapshot.*` and `core/sys/png.cpp` (static `Crc32`).
- Allowed by the spec ("no shared zip writer"). It's listed as a follow-up for the lead: consolidate into `core/sys`.
- The TI helper (`ti.cpp`, duplicating the runas technique) is the same: approved as module-local, with a follow-up recorded.

## Checked and sound (no finding)
- **Backend split.**
  - `ChangeServiceConfig*`, `StartServiceW` and `ControlService*` appear only in `backend_live.cpp`, and every one is gated by `armed_`.
  - Arm and Disarm bracket the apply worker (Arm before `CreateThread`, Disarm after `Join`), so there is no data race on `armed_`.
  - `MarkTi`/`CanWrite` opens `SERVICE_CHANGE_CONFIG` and closes it, and writes nothing.
- **Fixture and test-hook paths.**
  - `--fixture`, `--screenshot` and `--selftest` set `fixture_`, and then `backend_` is never created.
  - `--plan`, `--keys`, `--sort`, `--focus` and `--select` are consumed only in `LoadFixture`.
  - `CanApply` refuses in fixture mode, the Apply button is never a tab stop, and Enter never applies.
  - `--ti on` takes effect only in fixture mode.
  - `HX_SERVICES_TEST_HOOKS` code doesn't exist in a default build (CMake option OFF; CI turns it on only in `build-svctest`).
  - `crashAfterBegin`, `failFlushAfter` and `fixedStamp` are set only by the selftest.
  - `--dump-plan` uses only `FakeBackend`.
- **Apply state machine.** It follows the order lock → EnsureRoot → snapshot → recovery → B/P (flushed) → per row: re-read, skip if changed, `I` flushed, write, re-read, `D` flushed → E → TI release.
  - Any failure before B changes nothing.
  - `OnExit` releases TI on every early return.
  - `ImpersonationFlag` blocks Hadron file writes while impersonating.
  - The mutex is acquired and released on the same worker thread, and the in-process `Held()` set covers the mutex being recursive for its owner.
- **TI token handling.**
  - `SeDebugPrivilege` is enabled on the thread only, and the `ERROR_NOT_ALL_ASSIGNED` check is correct.
  - The winlogon SYSTEM swap is scoped; the token is duplicated as `SecurityImpersonation`.
  - The level and the TI SID are verified before each write, and each write is reverted.
  - The token is closed in Release and in the destructor and never reused across applies.
  - The claim is reset at every Acquire and Release; TI is stopped only when Hadron's own start started it.
  - The toggle is off by default and never persisted.
- **Workers.**
  - Load, plan and apply each run on their own `sys::Worker` and post to the HWND.
  - No dialogs anywhere.
  - Paint never calls the SCM (write access is cached by the load worker).
  - `CanClose` blocks while an apply runs, and `Shutdown` cancels and joins all three workers.
