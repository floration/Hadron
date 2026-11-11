# svcdrv Review, Intention group: documentation writer

Branch `module/services-svcdrv` at b7cc215, read-only. Checked `docs/design/services.md`, `tools/services/README.md`, the
`docs/ARCHITECTURE.md` and `docs/DECISIONS.md` diffs, `docs/pipeline/svcdrv/*.md` and `design/*.md` against
`modules/services/`, `core/host/`, `tests/` and `.github/workflows/ci.yml`. I ran `genkb.py --selftest` (PASS: 22 must-fail
cases, 99 entries, 26 fields per K line, as the README says). I did not run Wine.

Most severe first. "spec" = docs/design/services.md, unless 2-research.md is named.

## R-D1 (cat 6, could be argued 2): first-run README section F says "Hadron never changes these", which is false
- Docs: tests/golden/services/first-README.txt:139-142 (from recovery.cpp:269-273); spec services.md:184-186 ("`recovery\first` holds only what Hadron may change").
- Code: `FirstRestoreRows` (plan.cpp:1073-1075) leaves out the named never-preset items (TrustedInstaller, VSS, swprv, WinDefend ...) and every Boot/System-start item. But:
  - `BuildSingle` (plan.cpp:383-386) blocks only never-touch, Boot/System *targets*, TI-while-toggle-on and delayed-in-group. A single change can therefore disable VSS, swprv or TrustedInstaller (the README's own list, lines 143-145).
  - Presets can lower a System-start driver that has `sysok` (spec :121-124; plan.cpp:246-249), and `s.cfg.start < 2` drops every one of those from `recovery\first`.
- Why it matters: the README tells a user who can't boot that these items don't need restoring, and the "first" folder is the one that sounds like "put everything back". After a single change to VSS or a sysok preset, `recovery\first` silently can't undo it. Only that apply's own folder can. Fix one of two ways: reword F ("not in this script; if Hadron changed one, use that apply's recovery folder") and fix the spec line, or include rows Hadron can change.

## R-D2 (cat 6/5): the per-apply README and restore.cmd say they restore the plan they undo
- Docs/text: recovery.cpp:197 `"What this restores: " + title`, and restore.cmd's first rem line (recovery.cpp:97; golden restore.cmd:2 reads `...20261111-101500: Stripped Services`). RunApply passes `p.title` (plan.cpp:813).
- Result: the README line reads "What this restores: Stripped (Services)", but the folder restores the start types from *before* Stripped. For an Undo apply's folder it reads "What this restores: Undo <stamp>", and running it re-applies the original change.
- No golden or selftest covers a per-apply README.txt. Only `first-README.txt` is golden (selftest.cpp:939 checks a README with title "t").
- Why it matters: in WinRE the title is the only thing that tells the user which folder to run. Say "start types as they were before <title>, applied <stamp> UTC", and add a per-apply README golden.

## R-D3 (doc, recovery usability): someone whose Windows won't boot is not told how to find or choose a folder, or what to do without WinRE
- README (first-README.txt:5-21) covers one folder. Nothing says that `recovery\` can hold `first` plus one folder per apply, that stamps are UTC, or that the newest apply's folder undoes only that apply. To undo several applies, run the folders newest first, or run `first`. Nothing says to list them with `dir D:\ProgramData\Hadron\services\recovery` or how to read the README there (`notepad` or `type ... | more`).
- There is no fallback for a PC whose WinRE doesn't open (disabled or broken recovery partition): boot Windows install media > Repair your computer > Troubleshoot > Command Prompt. Step 1 assumes Automatic Repair appears.
- The result banner, the only place the UI names the recovery files, shows only the .zip: module.cpp:1532 `" Offline recovery: " + r.zipPath`. The README itself (line 21) and DECISIONS row 3 both say WinRE can't open .zip files and that the plain folder exists for that reason. The banner should name `r.recoveryDir` and its README.txt, and suggest copying the folder to a USB stick before restarting.

## R-D4 (doc): README says the .zip is "beside" the folder; it isn't
- README line 21 (recovery.cpp:227): "The .zip beside it is the same files."
- Code: the folder is `<root>\recovery\<stamp>\` (recovery.cpp:334) and the zip is `<root>\recovery-<stamp>.zip` (recovery.cpp:357), one level up from `recovery\`. Say "`..\..\recovery-<stamp>.zip`" or "in the Hadron services folder".

## R-D5 (doc/2, edge): README paths are wrong when `--data-dir` is set
- Spec services.md:152 says `--data-dir` is kept as a full path "(README shows it)".
- Code: WriteRecovery strips the drive letter (recovery.cpp:337-341), and README prints `D:<folder>\restore.cmd D:` and `C:<folder>\restore-online.cmd` (recovery.cpp:208, 225). With a data dir on another volume (`E:\hx`), the README tells the user to run the script from the Windows drive, where it isn't. A UNC data dir (`\\srv\share\...`, which the code supports) prints `D:\\srv\share\...`. restore.cmd's `%~d0` default also assumes the script sits on the Windows volume (recovery.h:4-5).
- Low reach (`--data-dir` is a power-user flag), but the spec claims the README handles it.

## R-D6 (cat 5): the Console help legend leaves out the Hadron column's "kept" label, and the spec description of that column is stale
- UI: module.cpp:2959 reads "Hadron: protected, no preset, or the lowest preset that changes it".
- Code: `KbVerdict` (module.cpp:792-807) also returns "kept" (hardware present or seen before), and it hides the level whenever hardware evidence keeps the item.
- Spec services.md:232-233 still describes the pre-Design column: "the item's lowest preset level, or protected / no preset / —". design/baseline.md §4 has the correct table. Update the spec and the legend.

## R-D7 (cat 5): plan notes name a control that doesn't exist
- plan.cpp:328 says "... Use Restore first snapshot instead." plan.cpp:1094 says "Restore first snapshot is off."
- The control is labelled "Default: first snapshot" in Baseline (module.cpp:2101) and "F7 Snapshot" in Console (module.cpp:2445). The research spec's term is not the UI's.

## R-D8 (cat 5/doc): Console key documentation is out of date with the code
- The Console help says "Tab: List, filter, detail" (module.cpp:2952), and console.md:112 says the same. Since the lead's merge (5-design.md:297-299), Tab walks every click target (`BuildTabStops`, module.cpp:2030-2039): banner actions, chips, item buttons and every F-key bar cell (`AddClick`, module.cpp:2487).
- Ctrl+F (focus the filter, module.cpp:1306) is in neither layout's help or design doc.
- Ctrl+Z is ignored while the filter has focus (module.cpp:3247). That is reasonable, but nothing says so.

## R-D9 (doc/4): DECISIONS row 2 is stricter than the code on hot-plug classes
- DECISIONS.md row 2 (svcdrv research) says "Stripped removes only never-seen hardware, never hot-plug classes."
- Code: a never-seen hot-plug class gets h==2 and `r.include = false` (plan.cpp:256), so it is offered unticked and the user can tick it. This applies to Stripped and Minimum alike. Spec services.md:190 documents "offered unticked", which matches 2-research §5.5 ("never auto-removed"). The DECISIONS row should say "never ticked by default", or the code should block the row.

## R-D10 (doc): format section errors (spec "Formats", store.h)
- Snapshot `deps` are `;`-joined, not `/`-joined: services.md:54 says `/`; the code uses `Join(i.deps, L";")` (store.cpp:37); the fixture snapshot.txt reads `http;WSearch`. `interfaces` is also `;`.
- Meta keys written: `kind`, `created`, `build`, `machine`, and `source` on pre-apply (plan.cpp:802-806, 1111-1114). The spec lists three of them. store.h:21 lists a `hadron` key that is never written.
- `created` is local time `yyyy-MM-dd HH:mm` in first.txt (plan.cpp:1108) but the UTC stamp in `snap-*.txt` (plan.cpp:803). Neither doc says so.
- Undo `P`: the 4th field is the literal `start`, and `identity` is `ti|admin`. The `I` record's last field is the same `ti|admin`, not a verified identity (store.h:34 says "identity-verified"). On `ok`, the `D` text field holds the identity (plan.cpp:902). store.h:36 leaves out `done|cancelled` on `E`, which the spec has.
- Write path (services.md:87-88) names `ControlServiceExW`. backend_live.cpp:351 also falls back to `ControlService`.

## R-D11 (doc): undocumented CLI flags and modes
- Module and render hooks, all fixture-only in effect: `--tab services|drivers` (live, prefs override it, module.cpp:547, 563), `--select`, `--plan <bloat|stripped|minimum|default|stock|undo|undo-last|resume|ti-stop>` (module.cpp:765-775), `--focus list|search|detail|<n>`, `--sort <key>[:desc]` and `--keys F1,F2,text:...` (module.cpp:3357-3368). The design docs mention them in passing; services.md does not.
- Selftest: `--update-golden` (rewrites every golden, selftest.cpp:402, 772-933) and `--golden <dir>` (selftest.cpp:542). No doc says how to regenerate goldens.
- `--dump-plan stock` works (selftest.cpp:1731) but isn't in the Acceptance list. That's fine, but say so.
- `--readonly` without `--data-dir` never sets a store root (module.cpp:571-575). The page then shows no first-run snapshot ("vs first" is blank), no undo logs and no interrupted banner. The banner (module.cpp:1482) and the spec (services.md:153) say only that nothing is written or armed.

## R-D12 (cat 5, minor): stamps are UTC but the UI shows them unlabelled
- Spec services.md:70 says stamps are UTC. The interrupted banner (module.cpp:1502) and the recovery folder names show `20261110-213012` with no "UTC". The owner will read it as local time when choosing which apply to undo or which folder to run (see R-D3).

## R-D13 (doc, minor)
- Spec Files table (services.md:33, 42): `backend_fake.*` is only `backend_fake.cpp`. "8 plan goldens and a `restore.cmd` golden" leaves out `plan-services-default-delayedlinger.txt`, `restore-dryrun.cmd`, `first-restore.cmd` and `first-README.txt`. Acceptance at :256-258 lists them.
- 3-build.md:9 says the list's Start type shows "pending restart". The code shows it only in the detail pane (module.cpp:1743-1745). 2-research §5.1 doesn't say where.
- docs/ARCHITECTURE.md:134-135 lists the data locations (settings, logs) but not the new `%ProgramData%\Hadron\services\` with its own DACL rules. :20 still says tools/ holds the "BIOS pipeline" only. docs/MODULES.md, PROGRESS.md and README don't mention services yet, which is Push's job per PIPELINE §8. Noted only.
- First-README section E (recovery.cpp:249-250) says "Hadron changed those while impersonating TrustedInstaller". In the `first` folder nothing was changed yet; the rows are marked because their key refuses admins.

## Checked and consistent
- "No screen-reader support in v1": in the status line after every load in both layouts (module.cpp:710-713) and in the Console help (module.cpp:2965). This meets CLAUDE.md.
- Console F-key map (F1-F11, item bar F1-F7 and Esc, F12 unused, F10 via WM_SYSKEYDOWN) matches console.md and the help page. No key applies: Enter refuses ID_APPLY (module.cpp:1298, 1334), and there is no Console Apply key.
- Baseline and Console column sets and drop orders match baseline.md §3 and console.md "Column set".
- Recovery folder contents (restore.cmd, restore-online.cmd, README.txt, restore-unicode-ControlSet00{1,2,3}.reg) and zip match the spec and DECISIONS row 3. `:put` matches spec :170-182. fltmc is used in the Safe Mode script.
- Undo-log `B` sources match spec :61 (plan.cpp:221, 290, 371, 447, 493). Control plans write no undo log file (`control` is never logged).
- `tools/services/README.md`: 22 must-fail cases, 26 fields, `hw` hint grammar (genkb.py:49 = hw.cpp:124-136), cumulative levels and the softer-level rejection (genkb.py:103-104) are all accurate.
- ARCHITECTURE `--accent`: screenshot only (host.cpp Screenshot) with the achromatic fallback, as documented. DECISIONS rows 3-5 match the code (WdiServiceHost `never`, dmwappushservice Stripped-only, DiagTrack in bloat).
- CI matches spec Acceptance: Wine job, MSVC selftests, plans, restore-dryrun on real cmd.exe, live test, and the TI variant with continue-on-error.
