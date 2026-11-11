# svcdrv Analyze round 2: Win32 re-check (`win32`)

Diff reviewed: `git diff b97f5d0..a8303b7 -- modules core .github tests`, read end to end for the items below.
clang syntax-only (`--target=x86_64-w64-mingw32 -Wall -Wextra`) on every services TU: clean.

## Verdict

| Item | Status | Notes |
|---|---|---|
| A1 (my S1): restore.cmd / restore-online.cmd | **fixed** | Safe for every character CmdSafeName admits. The CI dry run runs the same `:put` text on real cmd.exe (small gaps below). |
| A3 (my S2): TI `started_` | **fixed** | Claim reset per apply. START_PENDING is never claimed. Every return after Acquire releases. One narrow race left (minor R2-1). |
| A5 (my M7): data-folder trust | **fixed for the root** | The Win32 is correct. Parent ownership is not checked (minor R2-2). The two hooks can't be reached outside `--selftest`. |
| M2 TI left running on failure | fixed | An `OnExit` guard runs Release on every return after Acquire. The normal path clears it and releases at E. |
| M3 WaitState / M4 stop reason | fixed | Falls back to `ControlService` on ERROR_INVALID_PARAMETER. Logic checked below. |
| M5 UI-thread SCM | fixed | CanWrite runs on the load worker. Plan building (MarkTi, Undo/Resume re-reads, Dependents) runs on the new plan worker from copies. Paint never calls the SCM. |
| M6 load cancel | fixed | The load worker polls `Cancelled()` between FileCompany, CanWrite and the store steps. Shutdown joins all three workers. |
| M8 CM list retry | fixed | CR_BUFFER_SMALL is retried up to 5 times. The build notes say the guards now fail closed with a banner. |
| M11 `--data-dir` prefix | fixed | `Store::Plain` / `ToShell`. WriteRecovery strips `\\?\` and `\\?\UNC\`. |
| M12 ExpandEnvironmentStrings | not fixed | Still minor. Read-only, FileCompany only. |
| New serious | **none** | |

## A1: reasoning through cmd's parse phases
CmdSafeName is now an allow-list:
- characters: `[A-Za-z0-9]`, space, and `_ - . { } # $ @ + ~`;
- no leading or trailing space;
- no NUL.

The `:put` body (`PutSub`, shared by both scripts) has no `( )` blocks and only plain `if errorlevel 1 goto` lines. Every
use of the name is `"%N%"` or `"%~1"` inside double quotes. Phase by phase:
- **% expansion (phase 1):** no `%` in names. The `call :put "…"` line gets a second %-pass, and with no `%` it is a no-op.
- **Delayed expansion:** off (default; `setlocal EnableExtensions` only), and `!` is not admitted anyway.
- **Phase 2 special characters `^ & | < > ( )`:** none admitted. Even if `( )` were admitted, they would now be harmless, because no line puts the name inside a block.
- **Quotes:** `"` is not admitted, so no name can close a quoted argument early.
- **Spaces:** always quoted, and leading or trailing spaces are refused. `call` tokenising on space, `,` `;` `=` is
  neutralised by the quotes. `,` `;` `=` are not admitted anyway.
- **The remaining marks:**
  - `{ } # $ @ + ~ . -` have no meaning to cmd mid-token;
  - `@` matters only at the start of a command;
  - `~` matters only right after `%`.

Every Start and DelayedAutostart `reg add` is checked (`:bad` / `:baddelay`). The `reg unload` errorlevel is checked. The `:noload` text
now names the leftover-HXOFF case.

**Does the CI echo-only step test the same code path?** Yes, for the parse risk that mattered.
- `restore-dryrun.cmd` is `DryRunCmd(RestoreCmd(golden rows))`, which only rewrites lines starting with `reg ` to
  `call :dry reg …` and fakes `Select\Current`. The selftest asserts that the committed golden equals that transform byte for byte, so it can't drift from the generator.
- `call` adds a second %-expansion and caret-doubling pass. That makes the dry run stricter than the real line, not looser.
- The extra `Intel(R) Dry Run (x86)` row stresses `:put` with `( )` that the real generator would now route to `.reg`.
- The pwsh step checks for "was unexpected", for exit code 0, for the (R) row passing through, for the `Done: 5 restored` line and for the unload line.

Gaps (minor):
- **G1:** `:dry` always exits 0, so the `:miss`, `:bad` and `:baddelay` branches never run on real cmd. They are plain `echo "…%N%…"` / `set /a` /
  `exit /b` lines, so the risk is low. A second dry-run variant whose `:dry` returns 1 for one chosen row would cover them.
- **G2:** `restore-online.cmd` is not run in CI. It shares `PutSub`, and its own lines (the `fltmc` goto, `:noadmin`) are block-free.
  Low risk.

## A3: TrustedInstaller service state
`TiService` (ti.cpp):
- `Ensure` clears `started_` first. It calls Start only when the state is **STOPPED**, and claims the service only after `Control(Start)` succeeded.
- START_PENDING is polled until RUNNING within `waitMs` and never claimed.
- `StopIfStarted` stops only when the service was claimed, and clears the claim.
- `LiveTi::Acquire` closes any old token, and the provider is still reused across applies.

RunApply:
- `OnExit tiBack` is armed before `Acquire`, so a failed `T started` put, any later failed log write, or an Acquire that started TI
  and then failed (SeDebug, winlogon) all end in Release.
- `tiBack` is declared after `lock` and `log`, so it runs while both are still held.
- The normal path clears it, reads `StartedService()` before Release, and writes `T stopped` only when it was started and the stop succeeded.

A selftest on the fake covers two applies with TI already running, and the START_PENDING case.

- **R2-1 (minor) A false claim is still possible in a narrow window.** Between `Query` (STOPPED) and `StartServiceW`, Windows may start TI itself.
  `LiveBackend::Control` ignores ERROR_SERVICE_ALREADY_RUNNING and returns true, so `Ensure` claims it, and Release later stops
  a TI that Windows started. Fix: give Control (or a `Start` variant) an out-flag meaning "this call started it", and don't claim when
  StartServiceW returned ERROR_SERVICE_ALREADY_RUNNING.
- **R2-1b (minor UX)** STOP_PENDING (TI idling out) gives "TrustedInstaller is not running (Stop pending)" and the TI rows fail.
  It could wait for STOPPED, then start (and claim) it.

## A5: EnsureRoot
Correct Win32:
- The full path is normalised (`ToShell(ToExtended)`). It must sit below a drive or share root (`VolumeRootLen`); `\\?\Volume{}` and relative paths are refused.
- **Each component below the volume root**, top down:
  - `GetFileAttributesW` (it does not follow a final reparse point) must not show FILE_ATTRIBUTE_REPARSE_POINT and must be a directory;
  - a component is created only on FILE_NOT_FOUND/PATH_NOT_FOUND, and any other error refuses.
  - Missing parents get inherited ACLs. The root gets `O:BAD:PAI(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)` at create time. An elevated token can set owner BA.
- **The root itself:** opened with `READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES` and
  `FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT`.
  - The reparse and directory checks are repeated on the handle (`GetFileInformationByHandle`).
  - The owner comes from `GetSecurityInfo(OWNER_SECURITY_INFORMATION)`, compared with
    `IsWellKnownSid(WinBuiltinAdministratorsSid | WinLocalSystemSid)`.
  - Then `SetSecurityInfo(DACL|PROTECTED_DACL_SECURITY_INFORMATION)` puts back the protected DACL taken from the SDDL.
- **Handles and memory:** `h`, `osd`, `sd` and the token in `IsTokenUser` are freed on every path. No leak.
- **Failure:** the load worker sets `storeRefused`, reads no first.txt or logs, writes nothing, and turns writes off with a lasting
  banner. `CanApply` refuses. RunApply calls EnsureRoot(true) again before anything.

Hooks:
- `AcceptTokenUserAsOwner()` and `InjectedReparsePoint()` are function-local statics. The only writers are selftest.cpp:447
  (inside `if (wine)`) and :1363/:1376 (set, then cleared).
- `RunSelftest` is reached only from `ServicesModule::RunCli` when argv holds `--selftest`, and `Host::Run` returns straight after `RunCli`
  in the `cl.selftest` branch (host.cpp:365-399). No GUI, apply or `--live-test` path sets them.
- **Unreachable in a release run.**

Residuals (minor):
- **R2-2 Parent ownership is not checked.** Parents get only the reparse check. If a medium-IL process creates
  `C:\ProgramData\Hadron` before Hadron's first run (ProgramData lets Users create folders; CREATOR OWNER = them), it owns
  the parent. It can then grant itself FILE_DELETE_CHILD, rename Hadron's `services` aside, and put its own folder there holding
  a doctored `recovery\…\restore.cmd`.
  - Hadron's next EnsureRoot catches it (owner not BA/SY, red banner).
  - But a user who goes straight to WinRE would run the planted script as SYSTEM.
  - Fix: require the immediate parent `Hadron` to be owned by BA/SY/TrustedInstaller too, or create it with `O:BA` and a protected DACL like the root.
  - This needs malware already running as the user, so it stays minor.
- **R2-3 The DACL is re-applied on every EnsureRoot**, for each load and each apply. Handle-based `SetSecurityInfo` re-propagates inheritable ACEs to
  every existing child, so the cost grows with the number of logs and recovery folders.
  - WRITE_DAC is not a share-mode-checked right, so the open undo log can't make it fail.
  - Optional: skip when `GetSecurityInfo(DACL)` already shows SE_DACL_PROTECTED and the two ACEs.
- **R2-4 (Needs Windows)** The CI `--live-test` makes `%RUNNER_TEMP%\svc-live` through `EnsureRoot(false)` (default owner),
  then RunApply's `EnsureRoot(true)` checks the owner. On an elevated or UAC-off admin token the default owner is
  Administrators, so it should pass. If the hosted runner's token owner is the user SID, the live test will fail with "owned by …".
  Watch the first CI run.

## Other changes checked
- **WaitState:** `moved` and the 1 s grace for want=RUNNING.
  - A start that drops to STOPPED reports `dwWin32ExitCode` (or the service-specific code).
  - A stop that returns to RUNNING after leaving it reports.
  - A service that still reads RUNNING right after ControlService isn't flagged, because `moved` is still false. Correct.
- **SetStart order:** delayed flag first, then Start.
  - If ChangeServiceConfig2 refuses, nothing has changed.
  - If ChangeServiceConfigW then fails, the delayed flag stays written. The row has an `I` record and a `fail` D, so Undo restores it, and the message says so. Acceptable.
- **Plan worker:** builds from `PlanJob` copies, so it never reads `items_`/`logs_`.
  - A newer request while one is running is queued (`replan_`).
  - `CanApply` refuses while `planBusy_`.
  - OnPlanned joins before reading `planJob_.out`.
  - A plan build running alongside an apply only does reads (CanWrite opens for CHANGE_CONFIG and closes), and the backend's `armed_` is touched only by the UI thread before Start and after Join.
- **Load worker:** the `access` map survives TakeData's clear (`keep`). Cancel returns early with `ok=false`. Shutdown cancels and joins load, apply and plan.
- **`--ti on`:** now honoured in fixture mode only.

## Needs Windows
1. The CI step "Services restore.cmd on real cmd.exe (echo only)" passes. Optionally add the G1 variant for the failure branches.
2. The CI `--live-test` passes the new owner check (R2-4).
3. A real junction is refused by EnsureRoot on Windows CI. The selftest makes one there instead of injecting it.
