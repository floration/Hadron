# svcdrv mini Finalize: docs vs code

Read-only pass on `module/services-svcdrv` (asked at e3cfd92; HEAD is 94cafef, which adds only two DECISIONS rows).
All three finalize branches (kb, logic, ui) are merged. Nothing was built. "Right" means the side the fix keeps.
Every doc fix below is text only. No code fix in the table is a hard-rule break.

## Mismatches
| # | Doc says | Code does | Right | Fix |
|---|---|---|---|---|
| 1 | design/services.md:148-152: `EnsureRoot` "the protected DACL is applied again" on every load | store.cpp:490-520: sets the DACL only on a root it creates (kRootSddl :308). Refuses an existing folder unless it is already admin-only (R1) | code (R1, rule 1) | Rewrite the bullet; also ARCHITECTURE.md:138 "a protected DACL" -> "created admin-only; an existing one must already be" |
| 2 | services.md:65-80: undo log has B/P/T started\|stopped/I/D/E only; :79 says E lacks done\|cancelled in store.h | plan.cpp:962-983 writes `T starting`, `T started pid pstart`, `T notstarted`, `T stopfailed`; :1320 writes the `N` record and `dismiss:`/`ti-resolved:` sources; :1060 writes E with done\|cancelled. store.h:37,40 still say "identity-verified" and leave out done\|cancelled | code | Add the records and sources to the format block. Fix the store.h comments |
| 3 | services.md:85-86: an interrupted log offers Undo or Resume | Dismiss writes a `dismiss:<stamp>` log (plan.cpp:1432; R9) | code | Add Dismiss |
| 4 | services.md:81: control plans write no log file | Stop it on TI writes a `ti-resolved:` log (plan.cpp:1135; R4). R13 restarts what it stopped | code | Say so |
| 5 | services.md:245-246: `T started` with no `T stopped` marks a crash | Claim only while TI runs with the logged pid and creation time. Stale claims are closed at load (plan.cpp:1364, 1408) | code (R4) | Rewrite "TI marker" |
| 6 | services.md:134-137: a single change may set a System-start driver where Windows allows | BuildSingle blocks lowering one without `sysok` (plan.cpp:444; R6) | code (R6) | Rewrite; the 6-review Docs bullet asked for this |
| 7 | services.md:208: use-based Stripped rows are offered unticked | At Minimum they are ticked (plan.cpp:268-280) | code (proxy Q1, DECISIONS:27) | Add the Minimum rule |
| 8 | services.md:210-213: the stock plan "says how many items have a stock value (0 today)" | No data: `emptyText` "Hadron has no Windows-stock data for build N yet" (plan.cpp:371-376); the chip says "(no data for this build yet)" (module.cpp:2252) | code (proxy Q2, DECISIONS:28) | Update |
| 9 | services.md:17 and :214-217: "Default (first snapshot)"; the KB guards don't apply | Title is "Default: as first seen" (plan.cpp:313). It refuses another build or no machine GUID (:319-322; R11), reports a `baseline` first.txt (R12) and skips unreadable items (R2) | code (Q3, R2, R11, R12) | Update the label and list the new refusals |
| 10 | services.md:106-121: apply order | Also refuses every non-Undo apply with no first.txt (plan.cpp:901; R12), flushes `T starting` before Acquire (R22), and uses unreadable-config guards (CheckGuard; R2) | code | Add the steps |
| 11 | services.md:197-199: recovery\first holds "only what Hadron may change" | It also leaves out unreadable items; first-2..first-9 retries (plan.cpp:1473-1495) | code | Add both |
| 12 | services.md:247-252: Hadron column with `—`, `kept` | The header is "Preset"; it shows `not in KB` and `kept (hw)` (module.cpp:903-915) | code (ui.md cat 5) | Rename the labels |
| 13 | services.md:253-254 and :284 (`--live-test` in HadronServices.exe); 2-research:329 | Only in a `-DHADRON_SERVICES_TEST_HOOKS=ON` build; release prints "not in this build" (selftest.cpp:2290-2294, CMakeLists.txt:89-92; R25) | code (R25) | Note it in both rows. Log R25 in DECISIONS (no row yet) |
| 14 | services.md:42, 291, 300 and 2-research:324: "8 plan goldens" | 10 plan goldens (stock added) + delayedlinger (services_check.sh:55; the selftest loop) | code (R20) | Change to 10; list plan-*-stock.txt |
| 15 | Recovery README F (recovery.cpp:300; golden first-README.txt:153): "Hadron never changes these" | A single change may change the named never-preset items (VSS, swprv, TrustedInstaller with the toggle off) (plan.cpp:438-445 blocks only never-touch) | neither: README overclaims (rule 7) | "Presets and Default never change these; a single change to one is undone by Undo or that apply's own recovery folder". Regenerate first-README.txt |
| 16 | README "Which folder to use" (recovery.cpp:232-236) names only `first` | It may write `first-2`..`first-9` (plan.cpp:1474) | code | Mention first-N. Low priority |
| 17 | 2-research:139 (Company "second Worker pass") | Company is read in the load worker before the list shows (module.cpp:705; Analyze F4) | spec (design note: the spec wins) | Move it to a second pass, or log a deviation. The UI doesn't freeze, so this is not a rule issue |
| 18 | 2-research:140 "Preset target for the selected level" | KbVerdict shows "first preset that lists it" (module.cpp:898-916) | code (Design) | Log it in DECISIONS. 6-review asked for Design's unasked decisions to be logged; none is |
| 19 | 2-research:238-240: SeDebug, impersonate SYSTEM, then start TI | TI is started through the Backend as admin, before the token work (ti.cpp; services.md:171) | code (reasoning: SCM start needs no SYSTEM) | One line in services.md as a deviation |
| 20 | DECISIONS:23 "Stripped ... never hot-plug classes" | Hot-plug classes are offered unticked at every level (services.md:203-205; DECISIONS:27) | code | Add a clarifying DECISIONS row |
| 21 | 1-idea:54 data in `%LOCALAPPDATA%\Hadron\`; :21, :36, :46 signer, custom presets, before/after; :31 OEM in bloat; :38 TI for WinDefend | ProgramData; deferred; OEM never in presets; PPL is not unlockable | code (DECISIONS:23, 30; research §0.1) | Nothing; 1-idea is history. Push should say so in the PR |
| 22 | tools/services/README.md:10 "50 must-fail cases" | genkb.py:321-372 has 48 | code | 48; genkb.py:390 comment "seed range" -> "at least 60" |
| 23 | ARCHITECTURE.md:10-12: core has `Confirm()` and a zip writer | Neither exists (core/ui, core/sys). Services has its own (recovery.cpp:332) | code | Fix the tree comment (lead/core doc) |
| 24 | kb.py:41, 50, 51 use en-au/en-in URLs | Analyze D10 asked for canonical en-us | Analyze | Swap the URLs; kb-auditor to reopen them |

## Open questions
1. After a Windows feature update, R11 makes "Default: as first seen" refuse permanently. recovery\first's README still points at it. Should Hadron take a new baseline per build, or say so in README B/E? (proxy)
2. Logic and UI merged user-facing text after kb-audit-2 ran: banners, README, plan notes, legend, help. Rule 7 needs a kb-auditor pass on the merged diff. kb-audit-2's F1, F8, F9, F12-F15 look closed in code; the auditor should confirm.
3. Analyze leftovers still open: M8 (RunApply re-reads but doesn't re-run CheckGuard on the live item, plan.cpp:1003-1022), M12 (VM hint from SMBIOS only, hw.cpp:124), F3/F5 (load cost, detail rebuilt per paint), UNC data dir, `:dry` miss/fail branches and `restore-online.cmd` not run in CI.
4. logic.md "left for others": the banner line-break character drop ("whe / idle") isn't listed as fixed in ui.md. Drift for `cfgUnknown` items.
5. Should DECISIONS log the behaviours added in Finalize: Dismiss (R9), refusing applies without first.txt (R12), `--readonly` reading the default folder?

## Follow-up ideas (for Push to file)
- Windows-stock start-type table for 19045 (then other builds), with a published or owner-measured basis (proxy Q2).
- Custom presets with plain-text export/import (2-research §2).
- Measured before/after RAM and process count, reboot to reboot (§2, §0.6).
- Authenticode signer column (§2).
- Event 7040 account in drift (§5.7).
- Hardware evidence from device-interface triggers and the devnode last-arrival date (services.md "Deviations").
- Sourced OEM/third-party KB entries (proxy Q4).
- Rest of the KB: Fax; drivers not confirmed by name (HdAudAddService, HidClass, usbport, usbehci, volmgrx, iaStorAVC/vendor storage, CimFS); drivers left out (usbhub, usbvideo, usbprint, usbscan, usbser, wcifs, FileCrypt, rdbss, mrxsmb, mrxsmb20, bowser, NdisVirtualBus, WFPLWFS, rspndr, lltdio, vmbus and the Hyper-V drivers) (kb.md §3b).
- Registry-write fallback for SCM-refused items (SeRestorePrivilege + REG_OPTION_BACKUP_RESTORE) (§5.6).
- Lead/core: TI helper into `core/sys/token.h`; runas TI-state restore; `--size`/`--dpi` for fixture renders; ListView SetSort/Reveal/per-cell colour/row height, text-safe and 3:1 theme tokens, ScrollBar high contrast, a shared F-key bar (5-design "Not done"); an opt-in per-list font only if needed (proxy Q7).

## Needs Windows (windows-latest CI, a VM, or read-only on James's PC)
1. The first windows-latest run: MSVC compile, selftest, goldens, screenshots.
2. `--live-test HadronTestSvc` (hooks build), the TI variant, and the owner check on the runner's data folder.
3. `restore-dryrun.cmd` on real cmd.exe (`Intel(R)` row, Start 0 skip, `Done: 5 restored`). Not yet run: the `:dry` miss/fail branches and `restore-online.cmd`.
4. Whether a TI-impersonated `ChangeServiceConfigW` passes WaaSMedicSvc's descriptor (VM), and whether the TI token can be queried as admin.
5. Per-user templates from `EnumServicesStatusExW` on 19045; `ChangeServiceConfigW` on PimIndexMaintenanceSvc, UnistoreSvc and UserDataSvc.
6. The stop reason code accepted; Auto (delayed) on a grouped dummy; `DelayedAutostart` surviving `sc config start= disabled`; whether an absent value reads FALSE.
7. WinRE: `reg load` of `config\SYSTEM`, `Select\Current`, `reg add` on WaaSMedicSvc's key as SYSTEM.
8. ProgramData: can a standard user pre-create `Hadron\services`? R1: an Everyone-write ACE is refused (that test is skipped under Wine).
9. James's PC, read-only (`--readonly --data-dir <project folder>`): class filters, boot storage path (NVMe vs VMD/RST), LAN/Wi-Fi/BT names, VBS state, load time and plan-open freeze, high contrast, DPI above 96, and a hands-on keyboard traversal. Never apply a preset there.
