# sniffer round 2: re-check at a8303b7

**Setup.**
- I made a fresh copy of a8303b7 in `.../scratchpad/an/sniffer/tree2/`. It contains my harness `modules/services/sniff.cpp`, which has the round-1 tests plus new round-2 tests (`--sniff r2`, `--sniff junction`).
- The repo build is `/home/user/Hadron/build`, used as is. Nothing tracked was edited.
- Under Wine the harness sets `Store::AcceptTokenUserAsOwner()`, as the selftest does, because Wine ignores owners in security descriptors. Without it, every apply is refused, as expected; pass `--strict` to see this.

Shell setup for every repro: `S=<scratch>/sniffer; export WINEPREFIX=$S/wine WINEDEBUG=-all`

## My serious items

| Round 1 | A-item | Status | Evidence |
|---|---|---|---|
| #1 parens break restore.cmd | A1 | **Fixed** | `CmdSafeName` is now an allow-list: letters, digits, space and `_-.{}#$@+~`, with no leading or trailing space. `:put` is gotos only and no longer has a `( )` block anywhere near `%N%`. My hostile-name set (`Intel(R)…`, `paren)x`, `paren(x`, `a*b?c`, `x'y\`z`, `back]br[`, `! % ^ &`, RTL, TAB, LF, trailing `\`, `]é`) leaves only `sp ace` in restore.cmd; the rest go to the .reg files and README C. Repro: `wine $S/tree2/build/HadronServices.exe --sniff recovery --dir "Z:$S/rec2"`. Real cmd.exe is still CI-only (dry-run step). |
| #2 first recovery writes every driver | A2 | **Fixed** | `FirstRestoreRows` drops the following: never-touch items; anything with start < 2; launch-protected items; storage drivers; never-preset items; System-start drivers without sysok; every driver when hw is unavailable. The golden `first-restore.cmd` has 96 rows, with no ACPI/pci/stornvme/disk/volsnap/FltMgr/WdFilter/CNG/tcpip/mssmbios and no Boot/System values. A residual is listed as m1 below. |
| #3 Undo has no guard | A4 | **Fixed** | The GUI repro from round 1 (`shots2/Lforged.png`) now shows disk, RpcSs and stornvme as `[-] Protected: never touched`, "Apply 0 changes". Harness variants (`--sniff r2 --fixture 'Z:\home\user\Hadron\tests\fixtures\services'`, output in `out/r2-forged.txt`): a wrong machine GUID, a missing machine GUID or a wrong build is refused outright for both Undo and Resume. With the right machine, 12 of 20 forged rows are blocked: stornvme, `STORNVME`, RpcSs, disk, volsnap, mpssvc, WdFilter, a Boot or System target on a service, a Boot target on a third-party driver, and the per-user instance. With no hardware info, a driver row is blocked (fails closed). Rows still offered are m2. |
| #4 data dir trusted as is | A5 | **Fixed** (reparse point verified under Wine; owner check needs Windows) | A real link (a unix symlink, which Wine reports as a reparse point) is refused at the root and below it, with `secure` both on and off, and nothing is created through it (`--sniff junction --dir "Z:$S/jt/link"`). The owner check works: under Wine with the hook off, my temp dirs are refused. The DACL is re-applied by handle (`FILE_FLAG_OPEN_REPARSE_POINT`). A residual is listed as m3. |
| #5 delayed linger | A7 | **Fixed** | All variants are correct now: Default-first in all 81 (current, first) combinations; Undo for before `2/-` or `2/0` against live `3/1` or `4/1` (writes `2/0`); Resume for planned `2/-` or `2/0` against live `3/1` or `4/1` (writes `2/0`); stock (with a test KB entry) for stock `2/-`, `2/1` or `3/-` against live `3/1`, `4/1`, `2/1` or `2/-`. The CLI golden is `Spooler 3/1 -> 2/0`. |

### The two static test hooks (`Store::AcceptTokenUserAsOwner`, `Store::InjectedReparsePoint`)
They cannot be set outside `--selftest`:
- The only writers are `selftest.cpp:447` and `selftest.cpp:1363`; the clear is at 1376. Nothing in `module.cpp`, `standalone.cpp`, core or app references them, and there is no CLI flag, environment variable or registry value that sets them.
- `RunSelftest` is reached only through `ServicesModule::RunCli`, which runs only on `--selftest`. Both the standalone exe and `Hadron.exe --solo services --selftest` run it through `host.cpp:386` and then exit with its return code. The GUI never starts in that process, so a set hook cannot reach a real apply.
- `AcceptTokenUserAsOwner` is set only when `ntdll!wine_get_version` exists, so on real Windows it stays false even during the selftest.
- `InjectedReparsePoint` holds one exact temp path, and the selftest clears it.

Nit: both hooks are plain static state in release code. Wrapping them in `#ifdef HADRON_SELFTEST`, or moving them behind a test-only translation unit, would make this structural rather than reviewed. I don't count it as a finding.

## Regressions: none found
- Hostile snapshot fixtures × 5 levels × 2 tabs: no crash. The same 4 malformed cases are refused with clear errors. No ticked row starts from Boot or System. MapsBroker with ErrorControl Critical is still blocked. A service with Start 0 is now blocked (my round-1 m12 is fixed).
- GUI screenshots (`shots2/`) on the empty, 6,000-item, 40K-character, weird-name, no-hw, cycle, only-B, garbage, vanished, >16 MB, multi and forged fixtures: all render. Multi now explains the undo-of-undo case (my m10).
- Harness re-run (`out/r2-sniff-all.txt`):
  - crash after every row × torn tail: 0 problems;
  - injected flush failure at every record: no change made without its I record;
  - cancel at every point: undo is complete;
  - single change + undo: 0 bad.

  The "flush 20" and "cancel 8" lines are harness artefacts, as in round 1.
- A lost P record is now reported on the Undo plan ("planned 4 row(s) but only 3 could be read…"). It is still not recoverable automatically, but it is no longer silent (m6 fixed as minor).
- Apply lock: same as round 1.

## New or residual (all minor, no new serious)
- **m1. The first recovery still writes Start for drivers that were demand- or auto-start at first run but are Boot-start now.** Example: a third-party NVMe or RAID driver not on the storage list (Samsung `secnvme`, an `amd*` RAID driver) after a storage-mode change. Hadron's never-touch guard reads the live start, but the offline script cannot. Fix: in `:put`, read the current `Start` from the offline hive and skip when it is 0 or 1. Low probability.
- **m2. Undo and Resume still offer never-preset items (VSS, WinDefend, TrustedInstaller) and third-party items from a log that matches machine and build, ticked.** This is consistent with single changes being allowed on them. With A5 in place, forging a log needs admin, and the machine GUID is not a secret (readable from HKLM by any user). The log also accepts duplicate rows for the same service; BTHUSB appeared twice, both ticked, so the last write wins. Fix: drop or refuse logs with duplicate names, since a legit plan never has them.
- **m3 (suspected, needs Windows). Parent ownership is not checked.** `C:\ProgramData` lets Users create subfolders (inherited CI), so a standard user can pre-create `C:\ProgramData\Hadron` and own it. From there they can rename Hadron's admin-owned `services\` away, which loses the undo logs and recovery folder. Because writes go by path after `EnsureRoot`, they could also swap a parent during an apply so that undo, snapshot and recovery files land elsewhere. CREATE_NEW means nothing is overwritten, but the undo ability is lost. Check on Windows: as a standard user `mkdir C:\ProgramData\Hadron`, run Hadron elevated, then as the user `ren C:\ProgramData\Hadron\services x`. Fix: require Administrators or SYSTEM ownership of `ProgramData\Hadron` too (or apply the same protected DACL to it), or keep a handle to the root open and create files relative to it.
- **m4. Names with `( ) ' ! & %` now always go to the manual regedit-import path.** On the owner's PC that likely includes the `Intel(R) …` services, which is harder to do in WinRE than running a script. It is safe, and section C says so; this is a UX note only.
- Already in Build's "not fixed" list, re-confirmed: the snapshot round trip with a TAB in a name still collides ("duplicate tab name"); UNC data dir is untested.
