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

## Root cause: what is known and what is not
- **Known.** Before this branch the page showed exactly one thing: the rows of one `EnumServicesStatusExW` pass
  (`backend_live.cpp`, `LiveBackend::Enumerate`, lines 211-254 on main 96d250c). Nothing compared that list with
  anything, so a row the pass lost was simply absent, with no count, no trace and no way to tell from the UI.
  Nothing after the enumeration drops rows: `FoldInstances` removes only per-user instances whose template is listed,
  `OnTab`/`IsDriverType`/`KindOf` put 0x1/0x2/0x4/0x8 on the Drivers tab, `Refilter` filters only by tab and search
  text, and the company, signer, event, sxs and hardware passes update rows in place by name. The fixture, which has
  21 Boot-start drivers, shows all of them on the Drivers tab.
- **Not confirmed.** Which step loses the Boot-start rows on the owner's PC. Reading the code against the Win32
  documentation, the loop is correct for the documented contract (partial pages on `ERROR_MORE_DATA`, `pcbBytesNeeded`
  for the remaining entries, the resume handle carried across calls, a 256 KB cap), and the type mask 0x3FF includes
  both driver bits. Wine's SCM returns its 24 services and 7 drivers with 0x3FF on the first call. So the code holds no
  bug that explains "every Boot-start driver missing", and this write-up does not claim one. Two weak spots were
  hardened anyway: a page that returned no entry while asking for no more than the buffer would have looped without
  progress until the guard (now it grows, then fails), and a name returned twice across pages would have been listed
  twice (now dropped and counted).
- **How to confirm.** `HadronServices.exe --dump-list-live --out <file>` (and again with `--ti on`, with
  TrustedInstaller running) on the owner's PC. Its header gives the mask the SCM accepted, the raw entry count and
  driver count before any Hadron processing, pages, duplicates, the number of Services keys, how many rows the
  enumeration missed but the SCM opened by name (`#enum_missed_read_by_name`), registry-only keys and counts per
  tab, kind and start. If the Boot-start drivers come back as `scm-direct`, the enumeration loses them on that PC; if
  `#scm_raw` already has them and the page didn't, the loss is in the UI; if the dump shows all 94 as `scm`, the
  report came from something other than missing rows (for example sorting or scrolling), and the next step is the
  owner's screenshot.

## What changed
| Area | Where | What the user sees |
|---|---|---|
| Registry cross-check | `collect.*` (`CollectItems`), `backend_live.cpp` (`ListServiceKeys`), `backend_fake.cpp` (`regkeys.txt`) | Every Services key is on the page. A key the SCM's list missed is read from the SCM by name (detail pane: "The SCM's list did not return this…"). A key the SCM doesn't have is "Registry only (not loaded by the SCM)", read-only, with the reason and the key's values in the detail pane; Drivers tab by its Type, Services tab without one |
| Enumerate counts | `backend_live.cpp` (`lastEnum`) | Mask accepted and tried, raw and driver counts, pages, duplicates, unreadable configs: in the dump |
| TI read pass | `collect.cpp`, `module.cpp` (`StartLoad`, `ID_TI`) | The TrustedInstaller toggle reloads the list; with it on the list is read again as TrustedInstaller and the status line says "TrustedInstaller showed N more items and M more configs than administrator" (0 included) or why it didn't run. Rows TI added are marked in the detail pane |
| `--dump-list-live` | `selftest.cpp` (`RunDumpListLive`), `collect.cpp` (`DumpListTsv`), `standalone.cpp` | The TSV above. Release and test builds |
| Protected-items override | `plan.*` (`OverrideRefusal`, `OverrideOffered`, `BuildSingle`, `BuildCustom`, `BuildUndo`, `BuildResume`, `TargetGuard`, `RunApply`), `regfallback.cpp`, `module.cpp` | A checkbox on Single, custom-preset, Undo and Resume plans (Console F4) with the owner's warning. Protected rows become tickable, unticked, red PROTECTED with their guard; a red second confirmation lists each before anything is applied |

## Safety (hard rules 1, 2, 4, 7)
- The cross-check reads only: `RegOpenKeyExW` with `REG_OPTION_OPEN_LINK` and `KEY_QUERY_VALUE` on each subkey;
  a link is a leaf. Registry-only rows carry `cfgUnknown` and `CheckGuard` calls them never-touch, so no plan, preset,
  stock pass, first.txt, recovery script or undo can use them (selftest).
- The TI pass: off by default, never persisted (rule 4). Impersonation is reverted in a scope guard on every path,
  Hadron's file writes refuse while it is on, and the TrustedInstaller service is started only if it was stopped and
  stopped again if Hadron started it (the same `TiService` rules as an apply). The only new control permission is
  `AllowTiControl`: Control of the service named `TrustedInstaller`, only during the load's Acquire/Release.
  `--dump-list-live` never gets it: a stopped TrustedInstaller is skipped with the reason.
- The override changes nothing until Apply, then goes through the same snapshot, recovery zip and flushed undo log.
  It never offers launch-protected items (Windows refuses them), registry-only or unreadable items, per-user
  instances, Boot/System targets, or registry-fallback writes. The second confirmation is a click only.
  Built-in presets, Default and Stock are byte-identical with it on (selftest).
- Rule 7: the "why a key can sit outside the SCM" text is labelled reasoning, not checked on the PC.

## Spec ambiguities resolved
- *Undo of an override apply.* The override is offered on Undo and Resume plans too, so a protected change can be
  put back (a Boot-start driver goes back to Boot through the SCM); without the box ticked on that undo plan, the
  row stays refused as before. Reason: the spec asks that a protected row "can be applied and undone with it on".
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
- `--selftest`: 468 checks (was 439), standalone and in the suite, under Wine. New (`selftest_visibility.cpp`, and
  five UI checks in `module.cpp`):
  - the fixture's `regkeys.txt`: a typed key on the Drivers tab, a key without Type on Services, a link as a leaf;
  - the reported shape reproduced on the fake (every Boot-start driver left out of the enumeration): the bare
    enumeration shows 0 of 21, the cross-check shows 21 of 21 with their real start type;
  - registry-only rows in no preset, single or custom plan, no stock value, not in first.txt;
  - the TI pass on the fake: +1 item, +1 config, token reverted, write flag and control permission cleared, 0 said
    as 0, a refused impersonation keeps the admin list, the dump never starts TrustedInstaller;
  - the dump's header and rows;
  - the override: off by default, rows freed and unticked when on, built-in presets/Default/Stock identical,
    launch-protected / registry-only / unreadable / Boot-System targets still refused, custom presets, no registry
    fallback, a Boot-start driver applied to Disabled and undone back to Boot on the fake with snapshot, log and zip;
    UI: off at start, F4, the second confirmation (click only, withdrawn by a tick change), reset on close and on the
    next plan, never in the settings.
- `tests/tools/services_check.sh`: goldens unchanged (10 plans and delayedlinger); new screens
  `services-regonly-*`, `services-protected-on-*`, `services-protected-ask-*` (baseline and console).
