# svcdrv-visibility: every Services key, the TrustedInstaller read pass, and the protected-items override

Branch `fix/svcdrv-visibility`. Small fixes thread. Design: docs/design/services.md, section "What the page lists,
and the protected-items override". Decisions: docs/DECISIONS.md, 2026-11-11 rows tagged `proxy (Small fixes)`
(registry-only kind, TI read pass) and `owner` (the override).

## The reports
1. "clearly this is not all the services, like boot services are all invisible; and turning on trustedinstaller
   doesnt gain you view to any extra services or drivers, which it should"
2. "preset protected should have a manual override something like 'checking this box means you understand editing
   protected services/drivers will likely destroy your windows install'"

Evidence from the owner's PC (read-only, Windows 10 22H2 19045): 694 Services subkeys; `sc query type= all state=
all` as plain admin lists 693; 95 keys have Start=0 and the SCM lists 94 of them (Fs_Rec missing).

## Root cause: not a loss. StartOverride, plus 51 registry-only keys
Confirmed on the owner's PC with this branch's `--dump-list-live` (8136334, read only), as administrator and with
`--ti on`:
- **Nothing was lost.** `#scm_raw` 693 (the same as `sc query`), 744 Services keys, `#enum_missed_read_by_name` 0, and
  all 95 keys with registry Start=0 present as rows.
- **Why "boot services are all invisible".** 49 of those drivers (3ware, atapi, vmbus, vpci, pciide, storvsc,
  LSI_SAS*, megasas*, iaStorV, nvraid, Ramdisk, pmem, ...) have registry Start=0 and a `StartOverride` subkey whose
  values 0, 1 and 2 are 3. The SCM reports the overridden value, DEMAND_START, so Hadron showed them as Manual, and
  the Boot-start list looked short. Windows uses StartOverride to demote unused inbox boot drivers (reasoning, from how
  Windows handles these keys; not checked beyond this PC's values). Before this branch the page also marked such a
  row "(pending)", because its registry Start differed from the SCM's.
- **51 registry-only keys** (744 keys, 693 SCM entries) were never shown at all; they are now, read-only. Fs_Rec, the
  one Start=0 key `sc query` didn't list, is one of them.
- **694 vs 744 Services keys.** The two counts come from different methods: 694 is the earlier count of
  `HKLM\SYSTEM\CurrentControlSet\Services` subkeys made with `reg query` before this branch; 744 is
  `ListServiceKeys`' count in the dump (every subkey of `ControlSet<Select\Current>\Services`, links included). This
  write-up has no data on which keys make the 50 between them or whether keys were added between the two runs; the
  dump's 744 is the count the page now uses, and all 744 are on the page (693 from the SCM, 51 registry-only).
- **TrustedInstaller added nothing** (items +0, configs +0): TrustedInstaller saw nothing an administrator didn't, and
  the status line now says that with the number.
- Two weak spots in `LiveBackend::Enumerate` were hardened anyway: a page that returned no entry while asking for no
  more than the buffer would have looped without progress until the guard (now it grows, then fails), and a name
  returned twice across pages would have been listed twice (now dropped and counted).

## What changed
| Area | Where | What the user sees |
|---|---|---|
| Registry cross-check | `collect.*` (`CollectItems`), `backend_live.cpp` (`ListServiceKeys`), `backend_fake.cpp` (`regkeys.txt`) | Every Services key is on the page. A key the SCM's list missed is read from the SCM by name (detail pane: "The SCM's list did not return this…"). A key the SCM doesn't have is "Registry only (not loaded by the SCM)", read-only, with the reason and the key's values in the detail pane; Drivers tab by its Type, Services tab without one |
| StartOverride | `backend_live.cpp` (`ListServiceKeys`), `collect.cpp`, `model.h` (`StartOverridden`, `PendingRestart`), `module.cpp` | "Manual (registry: Boot, StartOverride)" in the Start type cell instead of "(pending)"; a sortable "Registry Start" column on the Drivers tab (dropped first when narrow); the raw Start, each StartOverride value and the reasoning line in the detail pane. Plans use the SCM's value; nothing writes StartOverride |
| Enumerate counts | `backend_live.cpp` (`lastEnum`) | Mask accepted and tried, raw and driver counts, pages, duplicates, unreadable configs: in the dump |
| TI read pass | `collect.cpp`, `module.cpp` (`StartLoad`, `ID_TI`) | The TrustedInstaller toggle reloads the list; with it on the list is read again as TrustedInstaller and the status line says "TrustedInstaller showed N more items and M more configs than administrator" (0 included) or why it didn't run. Rows TI added are marked in the detail pane |
| `--dump-list-live` | `selftest.cpp` (`RunDumpListLive`), `collect.cpp` (`DumpListTsv`), `standalone.cpp` | The TSV above, with `raw_start` and `start_override` columns and `#start_overridden`. Release and test builds |
| Protected-items override | `plan.*` (`OverrideRefusal`, `OverrideOffered`, `BuildSingle`, `BuildCustom`, `BuildUndo`, `BuildResume`, `TargetGuard`, `RunApply`), `store.*` (`prot`), `regfallback.cpp`, `module.cpp` | A checkbox on Single and custom-preset plans (Console F4) with the owner's warning. Protected rows become tickable, unticked, red PROTECTED with their guard; a red second confirmation lists each before anything is applied. The apply marks them `prot` in the log; only those rows can be undone with the box (Undo, Resume of an undo) |

## Safety (hard rules 1, 2, 4, 7)
- The cross-check reads only: `RegOpenKeyExW` with `REG_OPTION_OPEN_LINK` and `KEY_QUERY_VALUE` on each subkey;
  a link is a leaf. Registry-only rows carry `cfgUnknown` and `CheckGuard` calls them never-touch, so no plan, preset,
  stock pass, first.txt, recovery script or undo can use them (selftest).
- The TI pass: off by default, never persisted (rule 4). Impersonation is reverted in a scope guard on every path,
  Hadron's file writes refuse while it is on, and the TrustedInstaller service is started only if it was stopped and
  stopped again if Hadron started it (the same `TiService` rules as an apply). The only new control permission is
  `AllowTiControl`: Control of the service named `TrustedInstaller`, only during the load's Acquire/Release.
  `--dump-list-live` never gets it: a stopped TrustedInstaller is skipped with the reason. A load starts
  TrustedInstaller only after a flushed `ti-read` claim (B, `T starting`; then `T started <pid> <creation time>`,
  `T stopped`/`T stopfailed`, E), so a stop that fails shows as the TrustedInstaller-left-running banner (review R2).
- The override changes nothing until Apply, then goes through the same snapshot, recovery zip and flushed undo log.
  It never offers launch-protected items (Windows refuses them), registry-only or unreadable items, per-user
  instances, Boot/System targets (checked before never-touch), or registry-fallback writes. Undo and Resume lift only
  rows the override apply marked, only back to their logged before value (review R1). The second confirmation is a click only.
  Built-in presets, Default and Stock are byte-identical with it on (selftest).
- Rule 7: the "why a key can sit outside the SCM" text is labelled reasoning, not checked on the PC.

## Spec ambiguities resolved
- *Undo of an override apply* (review R1). The box is offered on Single and custom-preset plans, and on Undo or a
  Resume of an undo only when the plan holds a row its log marked `prot`; it lifts only those rows, only back to the
  logged before value. A forward resume of a protected change is refused (apply it again, or undo it).
- *StartOverride display.* "Optional/hidden by default" read as: the Registry Start column is on the Drivers tab only
  and is the first column dropped when the list is narrow; the Start type cell carries the registry value anyway.
- *Review A1-A3.* Undo lifts a marked row only when its apply finished it (D ok) and its target was 2 or more. Finished
  `ti-read` logs whose TrustedInstaller was not started or was stopped again stay on disk but leave the page's history
  (`DropClosedTiReads`; an open claim stays for the banner; nothing is deleted). A driver whose registry Start is 0 is
  kept as Boot-start even when a StartOverride makes the SCM report Manual, and every plan row for an item with a
  StartOverride says "Windows applies StartOverride over Start, so this change may not take effect (reasoning, not
  tested)". On the owner's PC that keeps the 49 overridden inbox boot drivers out of every plan unless the
  protected-items override offers them, like any Boot-start driver.
- *A TrustedInstaller read with no data folder.* No claim can be written, so a stopped TrustedInstaller is not started
  (the status says why); a running one is used without the control permission.
- *Launch-protected single changes.* Off, the single-change plan is as before (offered; Windows refuses at apply).
  With the override on, the row is blocked with "Windows itself refuses this change", as the spec asks.
- *Registry fallback.* The spec refuses Boot/System writes through it; this build refuses every protected row
  through it (SCM path only), which is stricter.
- *Baseline red marker.* Core's `ListView` has no per-row colour, so the Baseline list shows "PROTECTED: <reason>" as
  text and the detail pane in red; Console's grid draws it red. A per-row ink in `core/ui` is a lead follow-up.
- *TI starting the service for a read.* The existing TI rules start TrustedInstaller only for an apply. A read pass
  needs the token too, so the toggle's reload may start it, and stops it again when Hadron started it.
- *Registry-only items without a Type.* Shown on the Services tab with type "No Type value", per the spec.

## Tests
- `--selftest`: 484 checks (was 439), standalone and in the suite, under Wine. New (`selftest_visibility.cpp`, and
  five UI checks in `module.cpp`):
  - the fixture's `regkeys.txt`: a typed key on the Drivers tab, a key without Type on Services, a link as a leaf;
  - the reported shape reproduced on the fake (every Boot-start driver left out of the enumeration): the bare
    enumeration shows 0 of 21, the cross-check shows 21 of 21 with their real start type;
  - registry-only rows in no preset, single or custom plan, no stock value, not in first.txt;
  - the TI pass on the fake: +1 item, +1 config, token reverted, write flag and control permission cleared, 0 said
    as 0, a refused impersonation keeps the admin list, the dump never starts TrustedInstaller;
  - the dump's header and rows;
  - StartOverride: the fixture's hyperkbd (registry Start=0, StartOverride 0/1/2 = 3, SCM Manual) is overridden, not
    pending, and kept as a Boot-start driver (A3); a synthetic registry-Manual override row plans from the SCM's value
    with the may-not-take-effect note; the dump's raw_start/start_override columns and count; the list cell text;
  - a partial Services key list in the status text and the dump header;
  - the TI read claim: started, recorded and stopped; a failed stop persists as the banner; nothing recorded when
    TrustedInstaller already runs; not started when the claim can't be written; closed reads leave the history (A2);
  - the override: off by default, rows freed and unticked when on, built-in presets/Default/Stock identical,
    launch-protected / registry-only / unreadable / Boot-System targets still refused, custom presets, no registry
    fallback, a Boot-start driver applied to Disabled and undone back to Boot on the fake with snapshot, log and zip,
    the `prot` mark, an unmarked undo row and a protected forward resume refused, a System target refused, a marked
    row without D ok or with a Boot/System target not lifted (A1);
    UI: off at start, F4, the second confirmation (click only, withdrawn by a tick change), a rebuild brings a
    protected row back unticked, reset on close and on the next plan, never in the settings.
- `tests/tools/services_check.sh`: goldens unchanged (10 plans and delayedlinger); new screens
  `services-regonly-*`, `services-protected-on-*`, `services-protected-ask-*` (baseline and console).
