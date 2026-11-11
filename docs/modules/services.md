# Services (`services`)

`HadronServices.exe` on its own, or the **Services** page in `Hadron.exe`. Idea `svcdrv`, built through the pipeline
on 2026-11-11 (full run). The detailed design note is `docs/design/services.md`; the spec is
`docs/pipeline/svcdrv/2-research.md`. This page is the overview: what it does, how it works, where the code is, and how
it was designed.

## What it does
- **Two lists, Services and Drivers.** Every Win32 service and every kernel and file-system driver on the PC, with
  state, start type (including Auto-delayed and the trigger-start flag), type, account, image path, Company and
  Hadron's preset verdict. Every column sorts; F3 filters; the lists are keyboard-navigable. Per-user instances
  such as `CDPUserSvc_4f2a1` are folded onto their template.
- **Detail pane** for the selected item:
  - Hadron's explanation, for stock Windows items only, with its **basis** (rule 7) and, where the effect depends on
    workload, how to test it yourself;
  - why it is or isn't in each preset, with the basis for that (`pbasis`);
  - hardware evidence (is the device it drives present, absent, or seen before);
  - current, first-seen and Windows-stock configuration; triggers, dependencies, dependents and failure actions;
  - protection (PPL, the security-descriptor check, Tamper Protection);
  - the service's own registered description, labelled as Windows' text, not Hadron's.
  Third-party items get facts only: path, Company (the file's own claim) and start type. A few OEM services on
  the owner's board (MSI Center, Mystic Light, Realtek audio, Intel LMS and JHI) have explanation-only entries that
  say what is and isn't documented; they are never in a preset.
- **Presets, the same levels on both tabs:**
  - **Default: as first seen**: back to how Hadron first saw this PC (dated). That is not Windows' default, and the
    label says so.
  - **Default: Windows stock**: values come first from this PC's own WinSxS component manifests (read-only,
    decompressed with Windows' msdelta against the servicing stack's wcp.dll, cached per build, UBR and edition),
    cross-checked with the KB: five Microsoft troubleshooting pages and two third-party tables that must agree
    (wintips.org, TweakHound 20H2), each labelled with its source and "names no build". Where the manifest, the
    tables or a runtime-set service disagree, both values are shown and the row is never applied. No driver has a
    value. Details: `docs/pipeline/fixes/svcdrv-kb.md` §7.
  - **Pure bloat removal**, **Stripped** and **Minimum**, cumulative (an item in a level is in every stricter
    level unless that level names another target). Stripped removes only hardware that was never seen; Minimum
    ticks rows whose use can't be detected, Stripped leaves them unticked; hot-plug classes are never ticked. Membership is stock
    Microsoft items with a basis only; third-party and OEM items are never in a preset.
- **Single-item changes:** start type for both tabs; Start, Stop and Restart for services. Drivers are never loaded
  or unloaded live.
- **TrustedInstaller toggle** (rule 4): off at every launch and never saved. When on, the writes admin can't make run
  on a thread impersonating TrustedInstaller, and TI is put back the way it was found.
- **Undo** of any apply, and **Resume or Undo** of an apply that was interrupted (crash, power loss).
- **Custom presets.** Any plan's ticks can be saved under a name, then reopened, exported or imported as a plain-text
  `HXPRESET 1` file (Hadron's own strict parser; an export never overwrites, an import refuses cloud placeholders and
  offline files unread). Opening one only builds a plan, which goes through the same guards as the built-in presets
  (never-touch, never-preset, no KB entry, drivers without `sysok`, the hardware check) and the same confirm, snapshot
  and undo. Remove writes a tombstone; nothing is deleted.
- **Measure.** "Measure before" on a plan, then "Measure after" once the PC has restarted. Each records RAM in use, commit,
  the process and `svchost.exe` counts, minutes after boot, build and machine. The compare table is labelled
  `owner-measured`, warns when the conditions differ, names any Hadron applies between the two, and says how to test fairly.

### What it never does
- It never restarts the PC. Presets change only `Start` and `DelayedAutostart`, effective at the next restart.
- It never touches Boot-start drivers, filters, the boot storage chain, ErrorControl-Critical drivers, the floor
  (RPC, DCOM, LSA, event log, audio, networking, Hadron's own dependencies), or Hyper-V drivers while the hypervisor
  runs. A System-start driver is lowered only with a curated `sysok` entry.
- Protected (PPL) services, the Defender/Security stack, TrustedInstaller, VSS and swprv are never in a preset.
- It never creates or deletes a service, and never deletes a registry key (CI greps for those APIs).
- It never shows a predicted saving. The only numbers on the Measure page are values it recorded on this PC.

## How it works
1. **Load** (on a `Worker`): enumerate through the SCM, query each item, read hardware evidence through CfgMgr32
   (every devnode, present or not), match each item to the embedded KB, then fill in Company on a second Worker pass.
2. **Plan.** Every change, single or preset, first becomes a plan in the page: item, current → target, why, basis,
   and any guard or dependency that blocked a row. Nothing is written until **Apply** is pressed with the mouse.
   Enter never applies, Apply is never a tab stop, and fixture mode never applies.
3. **Apply** (`RunApply`):
   - take the cross-process lock and re-check the data folder;
   - refuse unless `first.txt` exists and belongs to this PC (Undo excepted);
   - write the snapshot, then the recovery folder and its `.zip`, then the undo-log header and every planned row;
   - per row: re-read the item (skip it if it changed), flush an intent line, write through the SCM, re-read and
     verify, flush a done line;
   - close the log, and stop TI if this apply started it.
   If anything before the first write fails, nothing changes.
4. **Recovery.** Each apply writes `recovery\<stamp>\restore.cmd`, `restore-online.cmd`, `README.txt` and `.reg`
   files, as a plain folder (runnable from WinRE, which can't unzip) plus a `.zip` of it. `restore.cmd` loads the
   offline SYSTEM hive and puts back every start type the apply changed.
5. **Data folder:** `%ProgramData%\Hadron\services\`, owner Administrators, protected admin and SYSTEM-only DACL.
   Every folder above it must be admin-owned and not renamable by others; a folder under a user profile is refused.

The safety mechanics, file formats and every resolved ambiguity are in `docs/design/services.md`.

## Code layout
| Path | Role |
|---|---|
| `modules/services/module.cpp` | The UI (`hx::Module`): both lists, the detail pane, plans, banners, both layouts |
| `modules/services/model.*` | Pure data: `SvcItem`, `StartCfg`, names, image paths, per-user folding |
| `modules/services/kb.*` | Loads the embedded `services.db` (RCDATA `SERVICES_KB`) |
| `modules/services/hw.*` | Hardware evidence (a pure part that reads a dump, and the live CfgMgr32 part) |
| `modules/services/backend.h`, `backend_live.cpp`, `backend_fake.cpp` | The SCM interface. `backend_live.cpp` is the **only** file that writes, and only after `Arm()`; the fake backend drives the selftest, goldens and screenshots |
| `modules/services/plan.*` | Guards, presets, single, Default, Undo and Resume plans, dependency checks, `RunApply` |
| `modules/services/store.*` | The data folder checks, snapshot and undo-log formats, `CREATE_NEW` writers, the apply lock |
| `modules/services/recovery.*` | The restore scripts, `.reg` files and a stored-only zip writer (rule 3) |
| `modules/services/sxsstock.*` | Windows-stock start types from this PC's WinSxS manifests (read-only), the cache and the cross-check |
| `modules/services/custom.*` | Custom presets: the `HXPRESET 1` format, the preset store, export and import |
| `modules/services/measure.*` | Before/after measurements: taking, storing and pairing records |
| `modules/services/ti.*` | TrustedInstaller impersonation (module-local; moving it to core is a lead follow-up) |
| `modules/services/selftest.cpp`, `cli.h`, `standalone.cpp` | `--selftest` (282 checks), `--dump-plan`, and `--live-test` (test-hooks build only) |
| `tools/services/kb.py`, `genkb.py` | KB source (289 entries) and the rule-7 gate (`tools/services/README.md`) |
| `tests/fixtures/services/` | The fixture machine plus `delayedlinger`, `forged`, `interrupted` and `multi` regression cases |
| `tests/golden/services/` | 11 plan goldens and the generated restore scripts |
| `tests/tools/services_check.sh` | The Wine acceptance run CI uses |

## Design process
It went through all eight pipeline stages; each wrote `docs/pipeline/svcdrv/<n>-<stage>.md`.
- **Ideas:** the owner's four lines became nine features; the proxy filled the gaps (logged in `docs/DECISIONS.md`).
- **Research:** five researchers (Windows APIs, architecture, documentation, kernel and hardware, logic and tests)
  produced the builder spec. It moved the stock table, custom presets, measurements and the signer column to
  follow-ups, and kept the Defender stack out of presets because PPL blocks even TrustedInstaller.
- **Build:** `module-builder-careful`, because a bad write can stop Windows booting.
- **Analyze:** five analysts; serious findings went back to Build before Design.
- **Design:** a Baseline designer and a Console designer, each in all four themes (`docs/pipeline/svcdrv/design/`).
- **Review:** nine reviewers in three groups plus `kb-auditor`; 25 findings (R1-R25).
- **Finalize:** fixed R1-R25, then six mini-stage passes and a second round; `kb-auditor` passed twice more.
- **Push:** these docs and the PR.

## Not yet checked on real Windows
Wine can't run the SCM writes, folder ownership or TrustedInstaller. The first `windows-latest` CI run is the first
MSVC compile, the first `--live-test` on a dummy service, the first `restore-dryrun.cmd` on real cmd.exe and the first
data-folder owner check. Read-only checks still wanted on the owner's PC (class filters, boot path, VBS, DPI, high
contrast, keyboard, the minimum-size clamp at 1366x768 and 125%) are listed in `docs/pipeline/svcdrv/7-finalize.md`.
Never apply a preset on the owner's PC as part of testing.
