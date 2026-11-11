# sniffer: adversarial run of `services` (branch module/services-svcdrv @ 34ee85c)

Everything ran under Wine 9 against fixtures or the fake backend only. Scratch files are all under
`<scratch>/sniffer/`:
- `scripts/mkfx.py` makes the hostile snapshot fixtures in `fx/`;
- `scripts/mklogs.py` makes the hostile undo-log fixtures in `fxl/`;
- `tree/` is a copy of the repo with a harness, `tree/modules/services/sniff.cpp`, run as `HadronServices.exe --sniff`;
- `out/` holds the plans and harness output, and `shots/` the screenshots.

The repo was not modified.

Shell setup for every repro: `S=<scratch>/sniffer; export WINEPREFIX=$S/wine WINEDEBUG=-all`

## Findings

| # | Rating | Where | Finding |
|---|--------|-------|---------|
| 1 | **serious** | recovery.cpp:85-90 and 120-126 (`:put`) | A service name that contains `)` or `(` breaks restore.cmd and restore-online.cmd. `CmdSafeName` lets parentheses through, and `%~1` is expanded unquoted inside `if errorlevel 1 ( ... )`. On real cmd this is a syntax error, which kills the whole script mid-restore with the offline hive still loaded. The first-run recovery includes every service, including third-party ones such as `Intel(R) ... Service`. |
| 2 | **serious** | plan.cpp:793-800 (`FirstRestoreRows`), recovery/first | The first-run recovery script writes Start for **every** item, including Boot- and System-start drivers and the curated floor (stornvme, disk, ...). Hadron otherwise never touches these. It bypasses `CheckGuard`, and if Windows changed a storage driver's Start after first run (for example AHCI/RAID mode), it can turn a recoverable PC into a 0x7B. |
| 3 | **serious** | plan.cpp:358-394 (`BuildUndo`) | Undo applies no guard at all. An undo log that is forged or corrupted but has a valid CRC (CRC is integrity, not authenticity) produces a ticked plan of `disk Boot->Disabled`, `stornvme Boot->Disabled`, `RpcSs Auto->Disabled`. Reproduced in the GUI. It also ignores the log's `machine` and `build`. |
| 4 | **serious** (suspected, needs Windows) | store.cpp:254-259 (`EnsureRoot`) | When the data folder already exists, it is used as is. The DACL is never checked or set, despite the header comment saying otherwise, and a reparse point is not detected. On stock Windows, `BUILTIN\Users` can create folders under `C:\ProgramData` (inherited CI), so a non-admin process can pre-create `ProgramData\Hadron\services` and own it. It can then plant undo logs (feeding #3) or first.txt. |
| 5 | **serious** | plan.cpp:271-276 (`BuildDefault` first), 403-405 (`BuildResume`), 287 (`stock`) | "Restore first snapshot" writes a different value from the recorded one when the live item carries a lingering `DelayedAutostart=1` on a non-Auto start. Example: current `3/1` or `4/1`, first `2/-` or `2/0`: the plan shows and writes `2/1` (Auto delayed). Hadron's own UI creates that state, because BuildSingle to Manual/Disabled leaves the 1 in place. The plan never converges. |
| 6 | minor | store.cpp:136,147; module.cpp:1129-1135 | An undo record with a bad CRC is skipped silently. If a P record is lost, its I/D records are orphaned and Undo leaves that item changed. `badRecords` and B-count vs rows mismatch are never shown. Reproduced: 1 of 4 items left changed. |
| 7 | minor | model.cpp:240 (`Esc`) | TAB in a service name becomes a space. The undo log and first.txt then name a different service, and if "A B" also exists, first.txt never parses: it is renamed aside and rewritten on every launch. |
| 8 | minor | module.cpp:420; recovery.cpp:270 | `--data-dir` is stored `\\?\`-prefixed, so README says `D:\\?\C:\...\restore.cmd D:`. `EnsureRoot` recursion fails on UNC roots (it tries to create `\\localhost`) and on a prefixed root whose parent is missing (it climbs to `\\?`). |
| 9 | minor | plan.cpp:715-731 (`LoadLogs`/`ResolvedBy`), store.cpp:385 | Log order relies on local-time stamps. After a DST fall-back or a clock change, "Undo last apply" picks the wrong log, and an undo with an earlier stamp never resolves its interrupted apply. Safety holds, because rows changed since are unticked. Suspected; derived from the code. |
| 10 | minor | plan.cpp:740-745 | An interrupted undo hides the original interrupted apply. Undo on the banner then means "undo the undo", which re-applies the preset. After Resume finishes the undo, the original log is still unresolved (only `undo:<stamp>` resolves it), so the banner returns. Reproduced with `fxl/multi`. |
| 11 | minor | plan.cpp:676-684 | If the undo log's D flush fails, RunApply returns before `ti->Release`, so a TrustedInstaller that Hadron started is left running. The banner covers it. |
| 12 | minor | plan.cpp:229-231 | A Win32 service with registry Start=0 or 1 is not guarded (only drivers are) and is offered `Boot -> Disabled`, ticked. Undo would then try to write 0 to a service and fail. This is only possible with a hand-edited registry. |
| 13 | minor | KB | No KB entry has a stock value, so "Default: Windows stock" is always an empty plan (`0 of 110 items`). |

What held up (no crash, hang or wrong plan):
- empty snapshot, empty file, 6,000 extra items with a 6,000-long dependency ring, 300-char names, 40K-char display/description/image (detail pane renders), RTL and emoji names, duplicates, missing fields, bad cfg, missing hw.txt, dependency cycles, missing deps, LUID instances (orphan, `_abc`, `X_`, third-party `Foo_1a2b`);
- Boot-start KB drivers are blocked in every preset; a System-start KB "svc" item that is a driver type is blocked; ErrorControl Critical and PPL are blocked;
- garbage, only-B, vanished-item and >16 MB undo logs;
- crash after every seq, each with no tail, a torn D tail or a NUL tail: Undo restored all 12 items every time and cleared the interrupted state (36 runs);
- flush failure at every record index: no change was ever made without a flushed I record;
- cancel after k = 1..7: the log is finished, the apply is not flagged interrupted, and Undo restores everything;
- BuildSingle plus undo over all 3x3x3x2 combinations: exact;
- apply lock: same process is refused, a second process is refused while a holder runs and allowed afterwards;
- restore.cmd excludes `% ! ^ & | < > " = ; ,`, non-ASCII and control characters into the .reg path. The title/stamp `rem` lines are sanitised. No command injection was found with the remaining characters, because a separator is needed and all are excluded.

## Detail and repros

### 1. Parentheses break restore.cmd
Generated (harness `--sniff recovery`):
```
call :put "Intel(R) TPM Provisioning Service" 2 1   & rem Auto (delayed)
...
:put
reg query "%K%\%~1" >nul 2>&1
if errorlevel 1 (echo skip %~1: no such key & set /a MISS+=1 & exit /b 0)
```
After expansion, the `)` of `(R)` closes the IF block. Real cmd then reports "`TPM was unexpected at this time.`", the same mechanism as the classic `Program Files (x86)` inside a block. A syntax error ends batch processing at all call levels, so:
- the remaining rows are not restored;
- `reg unload HKLM\HXOFF` never runs;
- the totals are not printed.

The same applies to `echo FAILED %~1 &` and to restore-online.cmd. Wine's cmd tolerates it (verified), so CI cannot catch this.

Repro: `wine $S/tree/build/HadronServices.exe --sniff recovery --dir "Z:$S/rec"`, then look at `$S/rec/recovery/20261111-101500/restore.cmd` lines 22-24.

Fix: exclude `(` and `)` in `CmdSafeName`, or rather never expand `%~1` inside a block:
- `set "N=%~1"` with delayed expansion off, then `if errorlevel 1 goto :miss`;
- use plain lines, not `( ... )` blocks.

Add a golden with a `(R)` name.

Needs Windows: run the generated restore.cmd from `rec/` against a test hive in a VM (WinRE cmd), or just `cmd /c` the 12-line `cmdt/t.cmd` on Windows 10 and expect "TPM was unexpected at this time".

### 2. The first-run recovery writes every driver
`FirstRestoreRows` (plan.cpp:793) takes every non-instance item. With the committed fixture the first recovery contains `stornvme`, `disk`, `pci` and others. Fix: emit only rows that pass `CheckGuard` with no neverTouch, and only start >= 2, or limit them to items Hadron's presets can change. The plan builders already refuse to touch the rest, so the recovery should not either.

### 3. Undo has no guard (GUI repro)
Run:
```
python3 -I $S/scripts/mklogs.py $S/fxl
xvfb-run -a wine /home/user/Hadron/build/HadronServices.exe --fixture "Z:$S/fxl/forgedI" --plan undo --screenshot "Z:$S/shots/Lforged.png"
```
The screenshot shows "3 ticked: disk Boot->Disabled, RpcSs Auto->Disabled, stornvme Boot->Disabled" and an "Apply 3 changes" button. The log only needs valid CRC-32s.

Fix: in BuildUndo and BuildResume, run `CheckGuard(live, c)`. Then:
- block neverTouch rows;
- block any target the original plan could never have produced: a service to start < 2, or a driver raised to Boot;
- refuse logs whose `machine` differs from the PC.

### 4. Existing data folder trusted as is
`EnsureRoot`: `if (a != INVALID_FILE_ATTRIBUTES) { if (!(a & DIRECTORY)) ...; return true; }`. The DACL is never checked, although the header comment says it is "only when this call created the folder or it has no protected DACL yet". There is no `FILE_ATTRIBUTE_REPARSE_POINT` check on the root or its parents.

Needs Windows: as a standard user, `mkdir C:\ProgramData\Hadron\services` before the first elevated run, then check `icacls` after Hadron starts. Expect the user to remain owner with full control.

Fix:
- open the root with `FILE_FLAG_OPEN_REPARSE_POINT|BACKUP_SEMANTICS` and refuse a reparse point;
- check that the owner is BA or SY and that the DACL is protected;
- otherwise refuse writes (writesOff) with a notice, or create a fresh folder and rename the old one aside.

### 5. Default restore writes Auto (delayed) for plain Auto
Run:
```
wine /home/user/Hadron/build/HadronServices.exe --dump-plan default --tab services --fixture "Z:$S/fx/delayedlinger"
```
Output: `[x] Spooler  Manual -> Auto (delayed)  (3/1 -> 2/1)`, while first.txt has `2/-`.

The harness (`--sniff all`) shows 4 of 81 (current, first) combinations wrong: `3/1->2/-`, `3/1->2/0`, `4/1->2/-` and `4/1->2/0`, all of which write `2/1`.

Cause: `op.delayed` is only set when `s.cfg.Delayed() != f->cfg.Delayed()`. `Delayed()` is false for a non-Auto start even when the value is 1. The same happens in BuildResume and in the stock branch.

Fix: as BuildSingle does, compare the raw value (`s.cfg.delayed == 1`) when the target is Auto, so that op.delayed = 0 is written.

### 6. A lost P record makes Undo incomplete
Repro: the harness's `LostP` gives "log count 4, rows 3, badRecords 1 ... 1 item(s) silently left changed after Undo".

Fix:
- show `badRecords` and `count != rows.size()` on the banner and the Undo plan;
- for I/D records with no P, fall back to the I record's observed value as "before" (I carries `observed`).

### 8. Long and UNC data dirs
Run:
```
wine $S/tree/build/HadronServices.exe --sniff long --dir 'C:\sniffld'
wine $S/tree/build/HadronServices.exe --sniff long --dir '\\localhost\C$\sniffunc'
```
- The prefixed root produces README `D:\\?\C:\...\restore.cmd D:`.
- UNC fails with "Could not create \\localhost: Path not found."

Fix: strip `\\?\` before computing `folder`. Stop the parent recursion at the volume or share root.

## Needs Windows
- #1: real cmd.exe behaviour with `(R)` names (expected: syntax error aborts the script).
- #4: ProgramData ACL inheritance lets a standard user pre-create and own `ProgramData\Hadron\services`.
- Whether real service key names on the owner's PC contain `(`/`)`: `reg query HKLM\SYSTEM\CurrentControlSet\Services | findstr "("`. Intel services such as `Intel(R) TPM Provisioning Service` are expected.
