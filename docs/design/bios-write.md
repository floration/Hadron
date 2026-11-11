# bios: guarded writes ("change settings like SCEWIN") — build spec

Owner's call 2026-11-11 (decision card "Yes, with all safeguards"); hard rule 2 was rewritten for it. Read
rule 2 in `CLAUDE.md` first: every bullet there is an acceptance check for this spec. Base: `main` at `ed726f7`.
Module spec for everything that already exists: `docs/design/bios.md`.

## Goal
In BIOS Settings, the owner can pick a new value for any mapped setting (from its option list, or within its
numeric range/step), stage several, review one plan, confirm, and have Hadron write the bytes into the existing
UEFI variable (`Setup` etc.) the way SCEWIN would, but with: a drift check, a whole-variable backup before each
write, read-back verify with automatic restore on mismatch, and a one-click restore of any earlier backup.
Changes take effect after a reboot. If a write ever leaves the board unbootable, Clear CMOS restores defaults
(board manual jumper JBAT1); the UI says so.

## Never
- Never call `SetFirmwareEnvironmentVariable*` outside `modules/bios/fwwrite.cpp`; never `NtSetSystemEnvironmentValue*`.
- Never write size 0 (that deletes the variable), never a size different from the current size, never a
  variable that is not in the map's (var, guid) list, never create one.
- Never write in `--selftest`, `--screenshot`, snapshot mode, unverified mode, or with the switch off.
- Never persist the switch (not in settings.ini, not anywhere). Every start = off.
- Never accept a value that isn't an option `v` (oneof), 0/1 (checkbox), or `mn <= v <= mx` with
  `(v - mn) % step == 0` when `step > 0` (numeric). No free hex, no raw byte editor.
- Never open a dialog from the worker.

## Files
- **New `modules/bios/fwwrite.h/.cpp`**: the only file with the write API. Contents:
  - `struct FwBackend { virtual void Read(FwVar*) = 0; virtual DWORD Write(const std::wstring& name, const std::wstring& guid, const std::vector<uint8_t>& data, DWORD attr) = 0; virtual bool Smbios(SmbiosInfo*, DWORD*) = 0; };`
    `RealBackend` (uses `ReadVariable`, `SetFirmwareEnvironmentVariableExW`, `ReadSmbios`) refuses every
    `Write` with `ERROR_ACCESS_DENIED` when the process-wide "no firmware writes" latch is set; the host's
    selftest and screenshot paths set it (see "Test mode latch"). Selftests use a `MockBackend` (in-memory vars).
  - Pure plan logic (no Win32): `ValidateValue(const Setting&, long long, std::wstring* why)`,
    `EncodeValue(long long v, int width, uint8_t* out)` (little-endian; fails if v doesn't fit in width bytes
    unsigned, or if `DecodeValue` of the result != v), `BuildPlan(const Db&, staged, live vars, &plan, &err)`.
  - `Execute(FwBackend&, const Plan&, const std::wstring& backupRoot, cancelled, progress, &result)`: runs on
    the worker, holds `ThreadPrivilege(SE_SYSTEM_ENVIRONMENT_NAME)` itself (refuse if !ok()).
  - `Restore(FwBackend&, backupDir, &plan/&result)`: plan and execute a restore from a backup folder.
- `modules/bios/module.cpp`: switch, edit UI, staging, plan overlay, restore UI, job wiring, selftests.
- `modules/bios/firmware.h`: change the header comment (reads here; the one write path is `fwwrite.cpp`).
- CMake (add the file to the bios target), `docs/design/bios.md` (one line pointing here), MODULES.md if it lists files.
- `core/host` only for the test-mode latch (see below). **Do not touch** `core/fs`, `core/ui` API, other modules,
  `tools/bios`, `settings.db`, `CLAUDE.md`, `.github/workflows/ci.yml` (the lead already changed the guard).

## Plan (data)
Staged change = (setting index, new value). `BuildPlan`:
1. Requires: live source, guard actively matched (`!unverified_` and `guardUnknown_` empty), switch on. The
   module checks these before calling and `BuildPlan` takes a `PlanGate{live, guardMatched, switchOn}` and
   re-checks them (defence in depth).
2. Validates each value (`ValidateValue`); drops no-ops (new == cur) with a note.
3. Groups by (var, guid). For each variable: current bytes and attributes from the last live read; refuse when
   the variable read failed, `attr` lacks NV(0x1)/BS(0x2)/RT(0x4), or has any of 0x8 (HW error record),
   0x10 (authenticated), 0x20 (time-based authenticated), 0x40 (append). Refuse if two staged settings overlap
   bytes, or `offset + width > size`.
4. Produces per variable: name, guid, attr, `oldData` (whole), `newData` (oldData with only the planned bytes
   changed), and per setting: prompt, menu path, old text → new text, offset, width, old bytes → new bytes,
   flags (`hidden`, `cond` → "Hidden in the BIOS menu" / "Shown only when another setting allows it; may have
   no effect"). Assert `newData` differs from `oldData` only inside the planned ranges.

## Execute (worker), per variable, in plan order, stop at the first failure
0. **Write lock.** Hold the named mutex `Global\Hadron.bios.fwwrite` for the whole run (RAII). Global, because
   firmware is machine-wide: two Hadron windows in any session (suite and standalone, two copies, two users)
   never write at once. Refuse, with nothing written, when the mutex can't be created or opened (the error text,
   e.g. access denied), when another holder has it ("Another Hadron window is writing BIOS settings. Nothing was
   written."), or when it comes back `WAIT_ABANDONED` ("Another Hadron window stopped in the middle of a BIOS
   write. Nothing was written. Check its backup."; the refusing run releases it, and the module re-runs the
   unfinished-session scan so that session's banner shows). `ExecOptions::lockName` overrides the name; the
   selftest uses its own, so it never touches the real lock.
1. Re-check SMBIOS against the map's `B` line (`BoardMatches`/`BiosMatches`); abort all on mismatch or error.
2. Re-read the variable. Abort if size, attributes or **any byte** differs from `oldData` (drift: "changed since
   it was read; re-read and plan again"). Planned bytes and unplanned bytes both count.
3. Backup, **for every variable of the plan before the first write** (steps 1–3 run for all variables, then
   4–6 per variable): session folder `%LOCALAPPDATA%\Hadron\bios-backups\<yyyyMMdd-HHmmss>[-N]\` (never reused:
   `CreateDirectoryW`, add `-2`, `-3` on `ERROR_ALREADY_EXISTS`). In it, each file `CREATE_NEW` + `FlushFileBuffers`,
   in the **same layout as Save snapshot** so Load snapshot reads it: `vars\<NNNN>_<name>__<guid>.bin` (the
   re-read bytes), `index.csv`, `system.txt`, plus `plan.txt` (the human plan text). Reuse snapshot.cpp's
   writers where possible; the selftest proves the loader accepts the folder. After the backup reads back equal,
   `manifest.txt` (`CREATE_NEW` + flushed, before the journal opens), tab-separated, version 2:
   ```
   Hadron BIOS backup manifest 2
   board	<board line from the SMBIOS read at backup time, as system.txt's "Board:">
   bios	<BIOS line from the same read, as system.txt's "BIOS:">
   <name>	<guid>	<size>	<attributes, 8 hex digits>	<SHA-256 of the bytes, BCrypt, lowercase hex>
   ```
   **Admin-only folders (DACL policy).** The backup root is created with, and when it already exists gets again
   (`SetNamedSecurityInfoW`, `PROTECTED_DACL_SECURITY_INFORMATION`), owner BUILTIN\Administrators and a protected
   DACL: SYSTEM and Administrators full control, Authenticated Users read/execute, inherited by children
   (`O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;AU)`). Every session folder and file is created
   with that same descriptor. Without this, an unkeyed manifest in a user-writable folder would let a
   non-elevated process plant a "backup" (with a recomputed hash and a fake unfinished journal) and have the
   elevated Hadron write its bytes: free hex across the UAC boundary. A path is **trusted** when its owner is
   Administrators or SYSTEM, it has a DACL that isn't NULL, and no access-allowed ACE (any allowed type,
   inherit-only included) grants a write, append, EA/attribute write, delete, delete-child, `WRITE_DAC`,
   `WRITE_OWNER`, `GENERIC_WRITE`, `GENERIC_ALL` or `MAXIMUM_ALLOWED` right to any SID other than SYSTEM and
   Administrators (`SdTrusted` / `PathTrusted`, read with `GetNamedSecurityInfoW`).
4. Journal (`journal.txt`, opened once with `CREATE_NEW`, each line flushed): `BEGIN <var> <guid>` before the
   write, `OK <var>` after a verified read-back, `RESTORED <var>` / `FAILED <var> <err>` otherwise, `END` at the end.
5. `Write(name, guid, newData, attr)` with the attributes exactly as read. Size never 0 (assert).
6. Read back. If size/attr/bytes != `newData`: write `oldData` back, read back again, journal `RESTORED` or
   `RESTORE-FAILED`, abort the rest. A write error (GLE) aborts the rest, surfaced with `VarErrorText`/FormatMessage
   (5 = locked by firmware, 19 = write-protected, 1314 = privilege).
Result: per variable (written+verified / restored / failed / not attempted), the backup folder path, and text.

**Incomplete session on start:** on module start (UI thread, cheap) scan the newest backup folders' `journal.txt`
for a `BEGIN` with no matching `OK`/`RESTORED`; show a banner: "A BIOS write on <stamp> didn't finish. Restore
that backup?" with a Restore button (goes through the normal restore plan + confirm). "Newest" is the session
folder's creation time (the stamp in the name follows a clock that can jump). No banner at all when the root
isn't trusted; a session whose folder or journal isn't trusted is skipped (and logged).

## Restore
"Restore backup…" button: folder picker (`IFileDialog` `FOS_PICKFOLDERS`, owned by `IHost::Frame()`, default
folder = the bios-backups root) or the newest session from a small list. **Only Hadron's own sessions under the
protected root restore** (`LoadBackupSession`, on the worker): the chosen folder must be a real folder (not a
link) directly inside the backup root (both resolved with `GetFinalPathNameByHandleW`); the root, the session
folder and every folder and file in it must be trusted (checked before anything is read; refused with "This
backup folder can be changed without admin rights, so it isn't trusted"); it must have `journal.txt` and a
version-2 `manifest.txt`; its variables must be exactly the manifest's, each with the manifest's size, NV|BS|RT
attributes and SHA-256; and `system.txt`'s board/BIOS must equal the manifest's. The board/BIOS guard then reads
the manifest's lines, never the unhashed `system.txt`. A plain Save snapshot folder or anything else is refused,
with the reason. Plan: for each backed-up variable that
the map knows, current size/attr vs backup; refuse a size mismatch; list byte ranges that differ and the
settings they decode to (old → backup value). Same gate as writes (switch on, live, guard matched), same
confirm overlay, same execute path (drift check against the *current* bytes read at plan time, own new backup
session of the current bytes first, write, read back). Restoring is never silent.

## UI (all custom-drawn, core/ui, both layouts, both themes)
- Header: a switch **"Allow BIOS writes"** (off). Turning it on shows a warning banner (tone bad): "Writes change
  your firmware settings. Hadron saves a backup first and verifies every write. They take effect after a
  reboot. If the PC won't boot, Clear CMOS (JBAT1 jumper, see the board manual) to restore defaults." When on but
  writes are blocked, the banner says exactly why (snapshot mode / not live / guard unverified or mismatched /
  not elevated / privilege missing).
- Detail pane of the selected setting, only when writes are possible: "New value": for oneof and checkbox, the
  options as chips (current one marked); for numeric, a `TextField` with the range/step shown and inline
  validation text. "Stage" button. A staged setting shows "Staged: old → new" with "Unstage".
- "Stage recommended" button for the selected goal: stages only settings where `RecVal` gives an exact value
  and it differs from current (never advice text). Shows how many.
- A "Staged (N)" filter chip, and a "Review and write…" primary button (enabled when N > 0 and writes are possible).
- Plan overlay (modal inside the view, drawn over the list; Esc or Cancel closes; **Enter does not confirm**):
  title "Write N settings to M variables", the warning line, each variable with name/guid/attr/size, each setting
  row (prompt, path, old → new text, offset, old bytes → new bytes, flags), the backup folder that will be used,
  and buttons Cancel / **Write N settings**. The plan list scrolls (wheel + draggable scrollbar).
- While executing: progress in status, other buttons disabled, Cancel stops before the next variable only.
- After: a result banner (written+verified count, any restore/failure with the error text), "Open backup folder"
  link (ShellExecute on the folder, prefix stripped), then a fresh live read so Current shows the new values.
- Log every planned write, write, verify and restore through `core/sys/log`.

## Test mode latch
`core/host`: when the process runs `--selftest`, `--screenshot`, or `--fixture/--snapshot`, set a process-wide
latch (e.g. `hx::sys::SetNoFirmwareWrites()` / `NoFirmwareWrites()` in `core/sys`; one bool, set before any
module code runs, never cleared). `RealBackend::Write` refuses when set; the switch can't be turned on when set.
This is a `core/` API addition: keep it to those two functions; the lead reviews it.

## Selftests (add to the bios selftest; all with MockBackend; no real firmware)
- ValidateValue: oneof accepts only option values; checkbox 0/1; numeric range and step; rejects out-of-range,
  off-step, negative where mn >= 0.
- EncodeValue: widths 1/2/4/8, little-endian, overflow rejected, round-trip with DecodeValue.
- BuildPlan: gate false → refused (each of the three); bad attrs (each bit) refused; missing var refused;
  overlap refused; out-of-bounds refused; no-op dropped; `newData` differs only in planned bytes; two settings
  in one variable → one variable entry.
- Execute: happy path (journal BEGIN/OK/END, backup bins equal old data, mock var equals newData); drift (one
  unplanned byte changed in the mock between plan and execute) → nothing written; SMBIOS mismatch → nothing
  written; write error on var 2 of 3 → var 1 written, 2 failed, 3 not attempted; read-back mismatch (mock
  corrupts) → restored to oldData and journal RESTORED; backup folder never reused (two sessions in the same
  second → `-2`); Load snapshot reads the backup folder and decodes the old values.
- Restore: from a backup, plan lists the differing settings; size mismatch refused; execute restores bytes.
  Manifest v2 content (board/BIOS lines, SHA-256 per variable); a hand-edited `.bin`, a session outside the root,
  the root itself, a plain Save snapshot folder, a `system.txt` edited to another BIOS, and a session with a file
  a non-admin can change are each refused; a genuine session restores.
- Trust policy on descriptors built in memory: admin-only passes; Users:write, Everyone:full, a NULL DACL, no
  DACL, a user owner, `GENERIC_ALL` (inherit-only), `DELETE` and `WRITE_DAC` to others are each refused. On disk
  the selftest probes whether the OS keeps file ACLs (Windows does; Wine reports the Unix user as owner): if it
  does, the restore and banner tests use the real check; if not, they inject one (`TrustFn`) that refuses only
  the paths a test marks writable.
- Write lock: refused while another thread holds it; released on return; an abandoned lock refuses that write
  and the next one goes ahead.
- Incomplete journal detected; complete one not; newest by creation time; an untrusted journal or root shows
  no banner.
- Latch: with the latch set, `RealBackend::Write` returns ERROR_ACCESS_DENIED without calling the API
  (call it with a bogus variable name to prove the path; the latch is always set in selftest anyway).
- The switch is off on start and is not in settings.ini after toggling.
Backups go to a new folder under `%TEMP%` that the run removes at the end (never `%LOCALAPPDATA%`:
`Execute`/`Restore` take the backup root as a parameter; only the module passes the real one).

## Screenshots
Add to `tests/tools/bios_check.sh` and the Windows CI step: `bios-write-plan` (fixture + a scripted staged plan
over the mock, overlay open) in baseline dark. The fixture path must drive a MockBackend; nothing real.

## Acceptance
- MinGW build of HadronBios.exe and Hadron.exe; `HadronBios.exe --selftest --fixture <snap>` and the suite
  selftest pass under Wine (`bash tests/tools/bios_check.sh build shots`), plus existing checks unchanged.
- CI guard passes: `grep -rnE 'SetFirmwareEnvironmentVariable' core modules app` shows only `modules/bios/fwwrite.cpp`;
  no `NtSetSystemEnvironmentValue`.
- Wine click test: BIOS buttons and list respond to left clicks (`scratchpad` driver is the lead's; just make sure
  the window-proc order rule holds: no returning case inside the `WM_LBUTTONDOWN` fall-through chain).
- Commits on your branch say why; never merge.

## Return
Branch name and head SHA; files changed; selftest counts; anything in this spec you couldn't do or changed and why;
the exact list of places that can reach `RealBackend::Write`.
