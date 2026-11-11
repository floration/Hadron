# svcdrv: Services and drivers page

Stage 1 (Ideas), 2026-11-11. Branch `module/services-svcdrv`. Full run (owner did not ask for a light run).

## Owner's words (verbatim)
> i would like a services/drivers page (run as trustedinstaller toggle)
> shows all services and drivers and shows their state, explains what they do (default windows ones), start settings, etc
> presets for default, pure bloat removal, stripped, minimum
> seperate for services and drivers but same preset levels

## What it is
A new Hadron module, `services`, that lists every Win32 service and every kernel/file-system driver on the PC,
shows its live state and start settings, explains what the stock Windows ones do, and applies tuning presets.
Services and drivers are two separate lists (tabs), each with the same four preset levels.

## Why
The owner tunes his PC aggressively and wants one tool that shows what is running, what it's for, and sets
it to a known level with one click, including protected services that admin alone can't change.

## Features
1. **Two lists, Services and Drivers.** Columns: name, display name, state, start type (Boot/System/Auto/Auto-delayed/
   Manual/Disabled, plus trigger-start flag), service type, account, image path, company/signer, preset target, basis.
   Sort by any column, filter box, keyboard navigation (CLAUDE.md UI rules).
2. **Detail pane:** description from the KB (stock Windows items only), dependencies and dependents, failure actions,
   current vs. stock vs. snapshot start type.
3. **Live control (services only):** start, stop, restart, change start type. Drivers: start type and state only; no
   live load/unload (unloading a kernel driver can bugcheck). `proxy`
4. **Presets, same four levels for both lists** (`proxy` for the meaning of each):
   - **Default:** two options. "Restore snapshot" (the state captured the first time Hadron's page ran on this PC) and
     "Windows stock" (a per-build reference table, Win10 22H2 first, each entry with a basis).
   - **Pure bloat removal:** telemetry, Xbox, consumer and OEM extras, things that are plainly unused.
   - **Stripped:** bloat plus printing, Bluetooth, search, sync, remote access and Hyper-V/VM pieces, only where the
     hardware or use is absent (detected, shown in the plan).
   - **Minimum:** only what is needed to boot, network, play games and run Hadron. Never touches Boot-start drivers;
     System-start drivers only from a curated list.
5. **Custom presets and per-item overrides**, export/import as a plain-text format Hadron writes and parses itself (rule 3). `proxy`
6. **TrustedInstaller toggle**, off by default (rule 4): when on, config writes that admin can't make (protected services
   such as WinDefend, WaaSMedicSvc) run on a thread impersonating the TrustedInstaller token; the TrustedInstaller
   service is left in the state it was found. Reuse the TI technique from `modules/runas` (docs/design/runas.md). `proxy`
7. **Safety:** every change (single item or preset) shows a plan (item, current → target, why, basis), changes nothing
   until confirmed, and writes a snapshot/undo log first (flushed). Undo restores the exact prior config. A boot-recovery
   .zip (rule 6) is written with the snapshot: a WinRE/offline-registry restore script for when Windows won't start. `proxy`
8. **Explanations:** written only for stock Windows services and drivers, each a tier-1 KB entry with a `basis` (rule 7)
   and a "test it, and how" note where the effect depends on workload. Third-party items show image path, company,
   signer and start type only. `proxy`
9. **Measured before/after:** RAM in use and process/service count, measured live on the PC before and after a preset
   and labelled as measured. No predicted savings. `proxy`

## Module, admin, files, firmware
- **Module:** new, id `services` (name "Services"), standalone `HadronServices.exe` and inside `Hadron.exe`.
- **Admin:** Required (every exe). TrustedInstaller only behind the off-by-default per-feature toggle (rule 4).
- **Files:** does not move, rename or delete user files, so not `core/fs`. It writes service configuration
  (SCM API / `HKLM\SYSTEM\CurrentControlSet\Services`) and its own snapshot, undo and recovery files under
  `%LOCALAPPDATA%\Hadron\`. Treat it with `module-builder-careful` anyway: a bad write can stop Windows booting.
- **Firmware:** none.

## Risks
- **Unbootable system.** Disabling the wrong driver or service can stop boot or networking. The owner has no backups.
  Mitigations: Boot-start drivers are never touched by presets, a curated list gates System-start drivers, dependency
  check in the plan, flushed snapshot before every write, offline recovery .zip.
- **Never apply presets to the owner's real PC during the pipeline.** Test on Wine/fixtures and a throwaway VM or
  CI runner only. Changing his services is outside the project folder and needs his OK for that action (rule 5).
- **Honesty (rule 7).** "Bloat" and preset membership are claims: every item's inclusion carries a basis; folklore is
  labelled as such; any number shown is measured, never estimated. Every KB diff goes to `kb-auditor`.
- **Build differences.** Service sets differ across Win10/11 builds; the stock table must be keyed by build, and
  unknown items fall back to "no explanation, not in any preset".
- **Windows self-repair.** WaaSMedicSvc and Windows Update can revert start types; detect and show drift.
- **TI impersonation** is new to `core/`-adjacent code; if it moves out of `modules/runas` into `core/sys` it is a
  `core/` API change.
- **Wine** can't exercise SCM writes or TI realistically; real checks need a Windows CI runner or VM.

## Proxy answers (logged in docs/DECISIONS.md, tag `proxy`)
1. Services get live start/stop/restart; drivers get start type and state only, no live unload.
2. TI toggle: in-process impersonation for writes only, off by default, TI service restored to its prior state.
3. Default preset: both first-run snapshot (primary) and a stock per-build table.
4. Preset meanings as above; Minimum never touches Boot-start drivers, System-start only from a curated list.
5. Plan + confirm + flushed snapshot/undo, plus a boot-recovery .zip.
6. Custom presets and per-item overrides, with plain-text export/import.
7. Explanations for stock Windows items only, with basis; third-party shows path, company, signer.
8. Before/after RAM and process count, measured live and labelled; no predictions.
9. Full run.
