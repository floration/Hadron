# svcdrv Analyze: Win32 correctness (`win32`)

Scope: modules/services/* at 34ee85c (backend_live, ti, plan RunApply, store, recovery, hw, module threading),
tests/golden/services/restore.cmd, ci.yml. Method: read every SCM, registry, token and file path end to end. Also ran
clang `--target=x86_64-w64-mingw32 -fms-extensions -Wall -Wextra -fsyntax-only` on every TU (clean) and tested
restore.cmd fragments under Wine cmd. Wine's cmd turned out too lenient to stand in for real cmd (see S1).

## Findings

| # | Rating | Where | Finding |
|---|---|---|---|
| S1 | **serious** | recovery.cpp:72-79, 119-122, 163-166 | Service names with `(` or `)` pass CmdSafeName. Inside `if errorlevel 1 (...)`, the `)` in a name like `Intel(R) …` closes the block early, real cmd reports a syntax error and the whole restore.cmd stops with the offline hive still loaded. recovery\first holds every service, so it hits any Intel(R) service. |
| S2 | **serious** | ti.cpp:117-127, 180-190; module.cpp:787 | `LiveTi::started_` is never reset, and the provider lives for the whole session. On a later apply in which Windows is already running TrustedInstaller, Hadron logs `T started` and then **stops TrustedInstaller, which it did not start**, possibly during servicing. START_PENDING is also counted as "Hadron started it". |
| S3 | **serious** (conditional) | plan.cpp:794-800 (FirstRestoreRows) | The offline recovery\first scripts write Start for **every** driver, including the storage and boot-path drivers and the Boot/System values that the in-app "Restore first snapshot" deliberately blocks. If the disk or controller changed since first run (AHCI/RAID/VMD, a clone to NVMe), running it in WinRE resets the boot storage driver: 0x7B. |
| M1 | minor | recovery.cpp:113,123,93-94 | The DelayedAutostart `reg add` is not checked (known). `reg unload` is not checked either. When HXOFF is still loaded from an aborted run, the `reg load` failure is reported as "BitLocker-locked?". |
| M2 | minor | plan.cpp:612-616, 705 | Any `put()` failure after `Acquire` returns early without `ti->Release()`. TI is left running, no `T stopped` is written, and the token handle stays open. Needs an RAII release guard. |
| M3 | minor | backend_live.cpp:255-273 | `WaitState` doesn't stop waiting when the service falls back to STOPPED after StartService, or back to RUNNING after a stop. It burns the 30 s cap and reports "timed out" instead of `dwWin32ExitCode`. |
| M4 | minor | backend_live.cpp:413-417 | `ControlServiceExW` reason PLANNED\|MAJOR_NONE\|MINOR_NONE (0x40060017) is unverified. Fall back to `ControlService` on ERROR_INVALID_PARAMETER. Needs Windows. |
| M5 | minor | module.cpp:676-690 (WriteAccess), 706-731 (BuildFrom→MarkTi), plan.cpp BuildUndo/Resume/Control | SCM calls run on the UI thread: one OpenSCManager+OpenService per plan row for CanWrite, plus Query and Dependents. A Minimum plan (~270 rows) blocks the UI. CLAUDE.md: "The UI never freezes." |
| M6 | minor | module.cpp:444-466, 150-155 | The load worker never polls `Cancelled()` (FileCompany over ~700 binaries, EnsureFirstSnapshot). `Shutdown()` joins it on the UI thread, so closing during a load hangs. |
| M7 | minor (security) | store.cpp:265-273 | EnsureRoot accepts an existing root as is: no owner, DACL or reparse check. ProgramData lets Users create folders (CREATOR OWNER full control), so a medium-IL process can pre-create `Hadron\services` or a junction there. It can then edit restore.cmd (later run as SYSTEM in WinRE), first.txt or the undo logs. |
| M8 | minor | hw.cpp:256-262, 287-299; plan.cpp:113-117 | The hardware guards fail open. `CM_Get_Device_ID_ListW` has no CR_BUFFER_SMALL retry, so a device arriving between the size and list calls makes `hw.ok` false. `Ctx::Prepare` then leaves the filter and boot-chain guards empty. A boot-disk lookup failure (RST/VMD RAID, Storage Spaces) leaves `bootChain` empty with `ok=true` and no notice. |
| M9 | minor | ti.cpp:20,36 | `ThreadPrivilege` ImpersonateSelf/RevertToSelf throws away any token the thread already held instead of restoring it. Harmless today because Acquire runs unimpersonated. |
| M10 | minor | store.h:34; plan.cpp:665 | The `I` record is documented as "identity-verified" but holds the *planned* identity, written before Impersonate. The verified one is in D's text. Doc nit. |
| M11 | minor | module.cpp:426; recovery.cpp:319; store.cpp:275-279 | With `--data-dir` the root is `\\?\`-prefixed. README then says `D:\\?\C:\…\restore.cmd`, and EnsureRoot recurses up to `\\?` when parents are missing. CI live test only. |
| M12 | minor | backend_live.cpp:455-456 | The `ExpandEnvironmentStringsW` result is used without checking that it fit the fixed `MAX_PATH*2` buffer. |
| M13 | minor | backend_live.cpp:172-183 | Trigger type 7 (custom system state change) shows as "type 7". |
| M14 | minor | hw.cpp:138 | An unknown hw hint returns 3, so the row is included, while the text says "kept". genkb rejects bad hints, so this is unreachable, but it should fail closed (0). |
| M15 | minor | plan.cpp:775-777 | `TiLeftRunning` looks only at the newest log, so a later non-TI apply hides an earlier "TI left running". |
| M16 | minor | module.cpp:1195,1755 | `SelectObject(dc, f.caption)` is never undone. That matters only on the WM_PRINTCLIENT path (caller's DC). |

## Detail

### S1: `(` / `)` in names break restore.cmd and restore-online.cmd
`CmdSafeName` rejects `%!^&|<>"=;,`, control characters and non-ASCII, but allows `(` `)` (and space). `:put` contains:
```
if errorlevel 1 (echo skip %~1: no such key & set /a MISS+=1 & exit /b 0)
if errorlevel 1 (echo FAILED %~1 & set /a BAD+=1 & exit /b 0)
```
`%~1` expands before the line is parsed. Inside an open block, cmd treats any unquoted `)` as the end of the block,
while a `(` in an argument is a literal. With `Intel(R) TPM Provisioning Service` the block ends at `Intel(R`, and
` TPM … )` follows. That gives "… was unexpected at this time." This is the same failure as the classic
`%ProgramFiles(x86)%`-inside-a-block bug. A syntax error ends the whole batch file, including from inside a `call`.
The script stops at the first such name, after `reg load` and before `reg unload`. A re-run then fails at `reg load` with the
misleading BitLocker message (M1).

It is reached because:
- `EnsureFirstSnapshot` writes recovery\first with `FirstRestoreRows`, which is **every** service and driver.
- The fixture already has `Intel(R) …` display names. On an Intel Z790 system, Intel services whose *key* name contains
  `(R)` are common: Intel's installers often use the display string as the service name.
- Per-apply scripts are hit only when a planned row has such a name: possible through Single, Undo or Default-first,
  not through presets (they have no KB entry).

Wine's cmd tolerates it (I tested this: `wine cmd /c` ran the paren-name call to completion), so it can't be reproduced here.
Fix (both):
1. Add `(` and `)` to the CmdSafeName exclusions, so those names go to the .reg path.
2. Rewrite `:put` without parenthesised blocks:
   `if errorlevel 1 goto :miss` / `:miss` / `echo skip "%~1": no such key` / `set /a MISS+=1` / `exit /b 0`.
   Do the same for `:bad`, and quote `%~1` in every echo.

Add a selftest case `!CmdSafeName(L"Intel(R) X")`.

### S2: Hadron can stop a TrustedInstaller it did not start
`ServicesModule::StartApply` creates `ti_` once (`if (needTi && tiOn_ && !ti_)`) and reuses it for every later apply.
`LiveTi::Acquire` sets `started_ = true` only when it starts TI. `Release` closes the token but **never clears `started_`**.
Sequence:
1. First TI apply: TI is stopped, Hadron starts it (`started_ = true`), and Release stops it.
2. Windows Update or a servicing task starts TrustedInstaller.
3. Second TI apply: Acquire sees RUNNING and doesn't start it, but `started_` is still true. RunApply writes `T\tstarted`, and Release
   calls `Control(TrustedInstaller, Stop)`. Hadron stops the Windows Modules Installer in the middle of its work.

That breaks the "leave its prior state as found" rule (runas design) and makes the log lie. Separately,
`ti.state != SERVICE_RUNNING` also covers START_PENDING. There `StartServiceW` returns ERROR_SERVICE_ALREADY_RUNNING
(ignored), and `started_` is set although someone else started it.
Fix:
- set `started_ = false` at the top of Acquire (when `tok_` is null) and at the end of Release;
- only set it when StartServiceW itself succeeded. When the state is START_PENDING, wait for RUNNING without claiming it;
- optionally create the provider per apply.

### S3: the first-snapshot recovery script can undo a storage-mode change
`FirstRestoreRows` takes every non-instance item with its first-run cfg. The in-app `BuildDefault(false)` blocks:
- `neverTouch`, which covers the storage, disk and file-system Floor groups, filters and the boot chain;
- named never-preset items;
- launch-protected items;
- any target `start < 2` ("Hadron never sets Boot or System start; Windows changes these itself").

The offline script applies none of these. Windows sets `stornvme`, `storahci`, `iaStorVD`/`iaStorAVC` and similar to 0 or 3
per machine, depending on the boot device. Example: first run on SATA/AHCI, then the owner clones to NVMe or switches
VMD/RAID, then a later Hadron change breaks boot and he runs `recovery\first\restore.cmd`. The script sets the
now-in-use boot storage driver to 3, which gives INACCESSIBLE_BOOT_DEVICE on top of the original problem. He has no backups.
Fix: give `FirstRestoreRows` the same exclusions as BuildDefault:
- skip `IsStorageDriver`, the Floor storage/disk/FS groups, and any row whose recorded start is 0 or 1;
- list the skipped names in README section D as "not restored offline: Windows sets these per PC".

Per-apply scripts are fine: they hold only rows Hadron changed.

### M8: hardware guards fail open
- `CM_Get_Device_ID_ListW` can return CR_BUFFER_SMALL if a devnode appears between the size and list calls (a USB device plugged in during the load).
  ReadLiveHw then returns `ok=false`, and `Ctx::Prepare` skips `FilterServices`/`BootChainServices`. CheckGuard's
  "named in UpperFilters/LowerFilters" and "boot disk path" never-touch rules disappear. Rows with an `hw` hint are kept
  (HwEvidence returns 0), but a KB driver with `sysok` and no hint that is also a class filter, and is not in Floor, would be offered.
- `BootDiskId` failing (Intel RST/VMD RAID volumes, Storage Spaces, dynamic disks) sets `hw.error` but leaves `ok=true`,
  with no plan note.

Fix:
- loop on CR_BUFFER_SMALL;
- in `BuildPreset`/`BuildDefault`, block every driver row when `!hw.ok` or `bootDisk` is empty, and add a note.

### Positives (checked, no finding)
- **SCM access and handles**:
  - reads use `SC_MANAGER_CONNECT|ENUMERATE_SERVICE` and `SERVICE_QUERY_CONFIG|QUERY_STATUS`;
  - CanWrite opens `SERVICE_CHANGE_CONFIG` only, and SetStart `CHANGE_CONFIG|QUERY_CONFIG`;
  - every SC_HANDLE is RAII (`Sc`), and no handle leaks on error paths.
- **Config buffers**:
  - QueryServiceConfigW and QueryServiceConfig2W loop on ERROR_INSUFFICIENT_BUFFER with regrowth;
  - EnumServicesStatusExW uses the resume handle, caps the buffer at 256 KB, and falls back to an older type mask.
- **ChangeServiceConfigW**: SERVICE_NO_CHANGE for type and error control, null for everything else. The delayed flag goes through `ChangeServiceConfig2W`
  only when `op.delayed >= 0`.
- **Undo log**:
  - the snapshot and recovery files are written CREATE_NEW and flushed before `B`;
  - every record is WriteFile + FlushFileBuffers with a CRC;
  - `I` is flushed before the SCM call, then the value is re-read and `D` written;
  - torn last records are detected.
- **Locks**: the named mutex plus an in-process set (the mutex is recursive) plus share-mode-0 on the log; acquired and released on the same thread.
- **TI**:
  - SeDebug on an ImpersonateSelf thread token, checked for ERROR_NOT_ALL_ASSIGNED (rule 4);
  - a SYSTEM token from winlogon in the same session, swapped onto the thread and restored;
  - TI's token duplicated `SecurityImpersonation`/`TokenImpersonation`;
  - each Impersonate checks the level and `CheckTokenMembership(TI SID)`, and Revert runs on every failure path;
  - `ImpersonationFlag` refuses Hadron file writes while impersonating.
- **Threads**: no dialogs from workers; the worker writes `job_`/`loadOut_` and the UI reads them only after `Join()`; Arm/Disarm happen before thread
  creation and after the join. Shutdown joins both workers before members die (workers are declared before `job_`, but the host always calls Shutdown).
- **Elevation**: non-elevated means read-only; the toggle is session-only, off by default.

### MSVC / clang-cl
- The three flagged risks are fine:
  - `std::max<size_t>`/`std::min<DWORD>` have explicit template arguments, and NOMINMAX is set in win_compat.h;
  - `wcschr` is used only as a boolean;
  - the `Hx*` structs have their own names with guarded constants, and their layout matches SERVICE_TRIGGER_INFO / SERVICE_TRIGGER / SERVICE_TRIGGER_SPECIFIC_DATA_ITEM.
- Other checks:
  - `ui::DrawText` is rewritten to `DrawTextW` by the macro in both declaration and use, so it is consistent;
  - `__cpuid(int[4], int)` matches MSVC;
  - `kTiSid` has an extern declaration before its definition;
  - `_wtoi`/`wcstoul`/`swprintf` come in through MSVC `<string>`.
- clang syntax-only with `-fms-extensions -Wall -Wextra` on every TU: clean. I see no MSVC blocker. The first CI run is still the proof.

## Needs Windows
1. **S1 confirmation:** this changes nothing. In cmd.exe, run a .cmd containing
   `call :p "A(B) C"` / `exit /b` / `:p` / `ver>nul` / `if errorlevel 0 (echo x %~1 & exit /b 0)`.
   Expected: "C was unexpected at this time." On the owner's PC, also run
   `sc query state= all type= all | findstr /b "SERVICE_NAME" | findstr "("`.
2. Per-user templates (type 0x50/0x60) on 19045:
   - does `EnumServicesStatusExW(0x3FF)` list them;
   - does `ChangeServiceConfigW` on a template succeed as admin, or return 87 / 5? Test on a VM or the CI runner, not the owner's PC.
3. `ControlServiceExW(SERVICE_CONTROL_STOP, SERVICE_CONTROL_STATUS_REASON_INFO, 0x40060017)` is accepted (CI dummy service; the TI variant's Release exercises it).
4. `ChangeServiceConfig2W(DELAYED_AUTO_START_INFO, FALSE)` on a Manual/Disabled service succeeds. Undo and Default plans emit `op.delayed=0` with start 3/4.
5. A TI-impersonated `ChangeServiceConfigW` passes the SCM access check (CI informational step).
6. WinRE: `reg load` of `<drive>\Windows\System32\config\SYSTEM`, `Select\Current` parsing (`0x1` → 001), unload.
7. MSVC compile on windows-latest.
