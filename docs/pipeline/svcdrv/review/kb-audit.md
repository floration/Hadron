# svcdrv kb-audit: services KB and user-facing text (rule 7)

Branch `module/services-svcdrv` at b7cc215, read-only. Auditor: kb-auditor.

## What I ran and checked
- `python3 -I tools/services/genkb.py --selftest`: PASS, 32 checks, 0 failed (22 must-fail cases, 6 ok texts, the committed KB with 99 entries, 26 fields per K line, every @KEY expanded).
- I regenerated the KB into the scratchpad (`genkb.py <scratch>/services.db`). It is byte-identical to `modules/services/data/services.db`: 99 entries; direct levels bloat 17, stripped 48, minimum 11; 23 explanation-only.
- I probed the gate with extra texts using `scratchpad/svcdrv-review/probe.py` (finding 9).
- I cross-checked every WS16 rating quoted in the KB against `docs/pipeline/svcdrv/research/ws2016-services.tsv`, using `scratchpad/svcdrv-review/ws.py`.
- I checked the sources live:
  - IoT page: the ratings for 46 services, and its definition of "OK to disable".
  - VDI 2004 guide: the rows for WdiSystemHost, DPS, SysMain, DiagTrack, WSearch, OneSyncSvc, CaptureService, CDPSvc and WerSvc.
  - VDOT Services.json: exactly 20 Disabled, plus InstallService, VSS and defragsvc Unchanged. This matches r3 and every @VDOT claim.
  - RTLF: wlidsvc, Windows Update, LicenseManager.
  - DEFCOMPAT: the service list and the wscsvc warning.
  - STARTMENU: BrokerInfrastructure and PDC.
- Not fetched, so I relied on r3's quotes: PERUSER, TAMPER, PROTSVC, WUERR, REGBACK, LOADORDER and VDOTREADME. STOP7B, ADDSERVICE and TRIGGERS are in SOURCES but no KB text uses them.
- Measured before/after RAM and process count is deferred (DECISIONS 2026-11-11; 2-research.md §1.6). No RAM, CPU, FPS, process-count or savings number appears in kb.py, module.cpp or plan.cpp. Nothing is predicted.

## Findings (most severe first)

### F1. Category 2 (produces a false claim). The TrustedInstaller left-running banner and the Stop-TI Why text
- **Location:**
  - `modules/services/plan.cpp:1048-1050` (`TiLeftRunning`);
  - `module.cpp:703-707` and `1513-1515` (the banner);
  - `module.cpp:1028-1032` (the Why text).
- **Text:**
  - Banner: "Hadron started TrustedInstaller during an apply that did not finish, and it is still running."
  - Why: "Hadron started it for an apply that did not finish, and nothing stopped it. It is a Manual (on-demand) service: Windows starts it again when it next installs updates or features."
- **What's wrong:** the claim is true only by coincidence. The test is "any log ever written has `T started` and no `T stopped`", plus "TrustedInstaller is running now". Three cases make the text false:
  1. **The apply did finish.** `ApplyPlan` writes `E` (done), then `Release()` fails to stop TI (plan.cpp:924-931). The log then has `T started` and no `T stopped`, but the apply completed. "did not finish" is false.
  2. **The claim never expires.**
     - Logs are never pruned.
     - The banner's own "Stop it" action runs a Control plan, and that plan never writes `T stopped` to the old log.
     - The banner also says "Windows stops it on its own when idle".
     - So every later time Windows starts TrustedInstaller for its own servicing (Windows Update, optional features), the banner reappears and says Hadron started it. The comment "any log: a later apply doesn't hide it" makes this permanent.
  3. **"nothing stopped it"** is not known. Hadron has only "no record of stopping it". Windows may have stopped it and started it again since.
- **Failure scenario:**
  1. An apply has a failed TI stop, so its log keeps `T started` and no `T stopped`.
  2. Weeks later Windows Update is installing a cumulative update, and TrustedInstaller is running.
  3. Hadron shows the WARN banner with a "Stop it" button, telling the owner that Hadron left TI running.
  4. The owner stops TrustedInstaller mid-servicing.
- **Second problem, "It is a Manual (on-demand) service":** this is hard-coded. The live start type is in `items_`, and the user (or Hadron's own single change) may have set TrustedInstaller to Auto. So the sentence is an assumption stated as a fact.
- **Fix:**
  1. Record TrustedInstaller's pid and process creation time in the `T started` record. Claim "Hadron started it" only while the running TrustedInstaller has that same pid and creation time (`OpenProcess` + `GetProcessTimes`).
  2. Write a resolving record (`T stopped` or `T gone`) when TI is seen stopped or a different instance is running, and after the Stop-it plan succeeds.
  3. Word the Why text from data: "Hadron started TrustedInstaller for the apply of <stamp> and its log has no record of stopping it. Its start type is <live StartLabel>; Windows starts it when it installs updates or features (reasoning)."
  4. Say "an apply that did not finish" only when the log has no `E` record.

### F2. Category 6. The help legend for the Hadron column (Console)
- **Location:** `module.cpp:2959`.
- **Text:** "protected, no preset, or the lowest preset that changes it".
- **What's wrong:**
  - `KbVerdict` (module.cpp:793-807) also returns "kept" (hardware present or seen before) and "—" (not in the KB). The legend names neither.
  - It also shows a level for rows that this preset would not change:
    - rows already at their target (the comment at :791 says current start types are ignored);
    - hot-plug rows offered unticked;
    - Stripped rows whose use can't be detected, which plan.cpp:256-261 offers unticked.
  - "changes it" overstates what the preset does.
- **Fix:** "protected; kept (its hardware was seen); no preset; — (not in Hadron's KB); otherwise the first preset that lists it (Stripped rows may be offered unticked)".

### F3. Category 6. The Stripped membership notes claim use is detected
- **Location:** `tools/services/kb.py` `P_STRIP_IOT` and `P_STRIP_IOT_WS16` (lines 82-85). They are shown as "Preset basis" and in the plan's basis line.
- **Text:** "Hadron adds it only where the hardware or use is absent." `P_STRIP_IOT` adds: "meaning useful to some devices but not all".
- **What's wrong:**
  - **Use is not detected.** About 20 Stripped entries have no `hw` hint and get "use cannot be detected: tick if you don't use it" (plan.cpp:256-261), for example WSearch, CDPSvc, OneSyncSvc, PcaSvc, SSDPSRV, ShellHWDetection and RasAuto. Hadron never establishes that use is absent; the user decides.
  - **The IoT page is misquoted.** It says "useful to some but not all **enterprises**, and security-focused enterprises that don't use it can safely disable it". It does not say "devices".
- **Fix:** "@IOT rates it OK to disable on Windows 10/11 IoT Enterprise ('useful to some but not all enterprises'). Hadron includes it only when its hardware was never seen; where use can't be detected the row is offered unticked for you to decide."

### F4. Category 6. Contested Minimum entries carry a `published` membership basis
- **Location:** `kb.py:88-89` (`P_CONTESTED`), used by SysMain, DPS, WdiSystemHost, WerSvc, DiagSvc and DusmSvc.
- **Text:** pbasis='published', "@VDOT disables it for virtual desktops; other Microsoft sources disagree (see Contested). Minimum includes it because it is not needed to boot, network or play."
- **What's wrong:** the published sources split, and for WerSvc two of them (WS16 and IoT) say Do not disable. Including the item is Hadron's call. The deciding sentence ("not needed to boot, network or play") is reasoning. A `published` label tells the user Microsoft backs the membership, which overstates it. For SysMain, "not needed to play" also sits awkwardly next to the entry's own "Effect is workload-dependent and can be negative: test it".
- **Fix:** set pbasis='reasoning' and write: "Microsoft sources disagree (see Contested): @VDOT disables it for virtual desktops. Hadron puts it in Minimum because Windows boots, networks and runs games without it (reasoning); whether that helps is not known: test it."

### F5. Category 6. SysMain's two unsourced claims
- **Location:** `kb.py:495-507`.
- **Claim 1:**
  - **Text:** if_off "Effect is workload-dependent and can be negative".
  - **What's wrong:** the entry's basis is iot(). The IoT page says nothing about performance. The only source for "can be negative" is r3 [43] (Windows Latest, a 4 GB VM, single runs), and it is not in SOURCES.
  - **Fix:** either add that source as a weak `published` one with its conditions ("4 GB Win11 VM, single runs: one app launch slower"), or write "can be negative (reasoning: it preloads for your usage pattern): test it".
- **Claim 2:**
  - **Text:** expl "It also maintains memory compression and placement (reasoning: from how Windows memory management uses it)".
  - **What's wrong:** this is not derivable reasoning. It is community or Windows-internals knowledge with no source, and "placement" is vague.
  - **Fix:** "It is also reported to manage memory compression (not stated in Microsoft's service pages)", or cite a source.

### F6. Category 6. intelppm: the folklore label covers the whole entry
- **Location:** `kb.py:720-724`.
- **What's wrong:** basis='folklore' applies to the explanation. So the Basis column shows "folklore" in the warn colour next to "protected". It reads as if Hadron's description, or its reason to keep the driver, were folklore. In fact:
  - what the driver does is reasoning, and the floor text in plan.cpp:37 agrees;
  - only "disabling it is a latency tweak" is folklore.
- **Fix:** set basis='reasoning' (note: "Processor power driver; the floor keeps it"), and label the tweak inline: "Disabling it is repeated online as a latency tweak (folklore: no published measurement); Hadron keeps it because it controls idle states and frequency."

### F7. Category 6. The gate's number check misses the quantities this module is about
- **Location:** `tools/services/genkb.py`. It uses `tools/bios/genkb.py` UNIT, and calls `B.render(..., warnings=None)`.
- **What's wrong:** rule 7 says the generator fails when a number lacks an owner-measured or published basis. These all pass the gate, in expl, why, contested and pbasis_note alike:
  - "Starts 40 processes fewer."
  - "Removes 30 services."
  - "Uses 12 threads."
  - "Frees 200 megabytes."
  - "Saves half the RAM."
  - "Two seconds faster."

  Process and service counts are exactly what the deferred before/after measurement will display. The bios SOFT_WARN check ("half", "twice", "tens of") is never reported, because the services gate passes no warnings list.
- **Fix:**
  - In the services gate, add a second pattern for `\d+\s*(processes|services|threads|handles|megabytes|kilobytes|gigabytes)`.
  - Pass a warnings list and print it.
  - Add must-fail selftest cases for each.
  - Optionally fail any digit outside an allow-list (5-digit builds, 0x error codes, "Windows 10/11", "Server 2016", "1709", "2004", "20H2", "1903").

### F8. Category 6. The "Default: Windows stock" chip and F8
- **Location:** `module.cpp:2103` (chip) and `:2948` (help "F8 Plan: Default, Windows stock").
- **What's wrong:** no KB entry has a `stock` value. The plan's note says "0 of N items have a Windows stock value", but the chip and key label promise a return to Windows defaults. This is the Analyze owner gap, still open (5-design.md:97).
- **Fix:** until the 19045 table exists, label it "Default: Windows stock (no data yet)" or disable it with that reason.

### F9. Category 6. The Drivers note on Pure bloat removal
- **Location:** `plan.cpp:269-270`.
- **Text:** "Pure bloat removal on the Drivers tab is expected to be near-empty".
- **What's wrong:** no driver entry has a bloat level, so the plan is always empty.
- **Fix:** "Pure bloat removal has no drivers: Hadron found no Microsoft source that names a stock driver as safe to disable."

### F10. Category 6. XboxGipSvc's hardware hint
- **Location:** `kb.py:291-297`.
- **Text:** why "Xbox accessories; kept when one was ever connected", with hw='class:XnaComposite'.
- **What's wrong:** "kept when one was ever connected" depends on Xbox One/Series (GIP) accessories enumerating in class XnaComposite.
  - No source in r3 or r4 says which class they use. The fixture simply asserts XnaComposite for PID 0B12.
  - hw.cpp's hot-plug list names both `xnacomposite` and `xboxcomposite`.
  - If GIP devices use the other class, a controller that was connected is reported as "never seen, hot-plug" and the row is not kept.
- **Fix:** use hw='svc:xboxgip'. It matches by function driver and is class-independent. Or verify the class on a real PC and record that as a measurement.

### F11. Category 6. A file claim in the Company column
- **Location:** `module.cpp:809-819` (`CompanyShort`) and the column header "Company" (:1888, :1892).
- **What's wrong:** the list shows the version-resource string as the company, with no hint that it is unverified. The detail pane says "Company (file claim)", and the Console design left the column out for that reason. Stripping the legal form also makes "Microsoft Corporation" and a look-alike "Microsoft Corp." identical in the list. The stripping itself is fine: the full text stays in the detail pane and in the filter.
- **Fix:** header "Company (claimed)", or add one legend line: "Company: what the file says about itself, not a verified signature".

### F12. Category 6. Console help: "Every key changes the view or opens a plan"
- **Location:** `module.cpp:2941`.
- **What's wrong:** F6 flips the TrustedInstaller toggle, a session state, and Esc during an apply requests a cancel. Neither is "the view" or "a plan". The safety half, "no key applies one", is true.
- **Fix:** "No key applies a plan or changes a service: Apply is a click. Keys change the view, open a plan, flip the TrustedInstaller toggle, or (Esc during an apply) stop after the current item."

### F13. Category 6. "The TrustedInstaller toggle does not help" for protected services
- **Location:**
  - `module.cpp:1817-1818`: "only the protected service itself can change it; the TrustedInstaller toggle does not help".
  - The TI caption at :2083 and :2780: "Not Defender, PPL or Tamper Protection".
  - The WinDefend `keep` text in kb.py: "@PROTSVC: a protected service can only be reconfigured by itself".
- **What's wrong:**
  - PROTSVC says the ChangeServiceConfig2 call "must be made by the protected service itself", and it lists allowed commands, including `sc config start=Auto`.
  - That TI impersonation fails is r3's reasoning, marked untested (r3 §0.1).
  - Both are stated as tested facts.
- **Fix:** "Microsoft's protected-services page: only the service itself can change its configuration (setting Auto is allowed). Hadron's TrustedInstaller toggle is not expected to help (reasoning, not tested)."

### F14. Category 6. Recovery README section F
- **Location:** `recovery.cpp`, golden `tests/golden/services/first-README.txt`, section F.
- **Text:** "Hadron never changes these ... Boot- and System-start values, the storage and boot path, never-touch items and launch-protected services. Windows sets those per PC, and writing an old value back after a disk or controller change can stop Windows from starting."
- **What's wrong:** the list includes TrustedInstaller, VSS, WinDefend, Audiosrv, BFE and others. Windows does not set those per PC, and the boot risk applies only to the boot path. Two reasons are merged into one.
- **Fix:** "Hadron never changes these, so the scripts never write them. Boot- and System-start values and the storage and boot path are set by Windows per PC: writing an old value back after a disk or controller change can stop Windows from starting. The rest are services Hadron protects (Defender, servicing, System Restore, core services)."

### F15. Category 6 (borderline 2). The offline restore script hides the BitLocker hint
- **Location:** `recovery.cpp` (:nowin and :noload), golden `first-restore.cmd:10,162-167`.
- **What's wrong:** the BitLocker hint is only in `:noload`. A BitLocker-locked volume fails the earlier `if not exist "%T%\Windows\System32\config\SYSTEM"` and lands in `:nowin`. That says "No Windows installation at D:", which sends the user looking for another drive.
- **Fix:** add to `:nowin`: "If that drive uses BitLocker it shows no files until unlocked: see README.txt section A, step 2."

### F16. Category 6. Smaller wording items
| Entry / place | Text | Problem | Fix |
|---|---|---|---|
| README A | "WinRE can't open .zip files" | Absolute and unsourced | "WinRE has no Explorer to open a .zip: use the plain folder." |
| RetailDemo if_off | "Nothing else uses it." | Unsourced absolute | "Hadron knows of nothing else that uses it (reasoning)." |
| MessagingService if_off | "A desktop without a modem has nothing that uses it." | Reasoning stated as fact | Append "(reasoning)" |
| dot3svc why | "home networks do not use it" | Unsourced, though the IoT page supports a version of it | "@IOT: wired networks that don't enforce 802.1X are unaffected; home routers normally don't (reasoning)" |
| CaptureService if_off | "...can lose optional capture features; test it." | "Test it" with no how (rule 7) | test="Disable, restart, then record with OBS's 'Windows 10 (1903 and up)' capture and take a Snipping Tool shot; undo if either fails." |
| SSDPSRV if_off | "test online play" | No how, and no `test` field | test="Note the game's NAT type (or your router's UPnP table) before and after; undo if it gets stricter." |
| RasAuto pbasis | "Test it if you use the built-in Windows VPN client." | A test hidden in the basis note | Move it to `test`: "Connect your built-in Windows VPN after a restart." |
| UsoSvc | "@WS16 and @IOT say Windows Update depends on it." | IoT says "Stopping UsoSvc service prevents download and installing of latest updates", not "depends on" | "@WS16 says Windows Update depends on it; @IOT says stopping it prevents downloading and installing updates." |
| LicenseManager, NgcSvc keep | "(reasoning)" | The IoT page supports both: "content acquired through the Microsoft Store doesn't function properly" and "Reconfiguring the NgcSvc service isn't recommended" | Optional: cite @IOT and make them published |

## Audit table
| # | Entry / string | Claim | Basis given | Verdict | Suggested rewording |
|---|---|---|---|---|---|
| 1 | TI banner + Stop-TI Why | Hadron started TI for an unfinished apply; nothing stopped it | none (logic) | wrong | F1 |
| 2 | Stop-TI Why | "It is a Manual (on-demand) service" | hard-coded | overstated | Use the live start type (F1) |
| 3 | Console legend, Hadron column | "protected, no preset, or the lowest preset that changes it" | n/a | overstated | F2 |
| 4 | P_STRIP_IOT / _WS16 | "only where the hardware or use is absent"; IoT "devices" | published IoT | overstated | F3 |
| 5 | P_CONTESTED (6 entries) | Membership is published | published | overstated | pbasis reasoning (F4) |
| 6 | SysMain if_off | "can be negative" | published (IoT) | unsourced | F5 |
| 7 | SysMain expl | Memory compression and "placement" | "reasoning" | unsourced | F5 |
| 8 | intelppm | Whole entry labelled folklore | folklore | wrong (label) | F6 |
| 9 | genkb gate | Fails on any number without a measured or published basis | the selftest | overstated | F7 |
| 10 | "Default: Windows stock" chip, F8 | Restores Windows defaults | stock table (empty) | overstated | F8 |
| 11 | Drivers bloat note | "expected to be near-empty" | KB | wrong | F9 |
| 12 | XboxGipSvc | Kept when an Xbox accessory was ever connected | class:XnaComposite | unsourced | F10 |
| 13 | Company column | The version-resource string is the company | file claim, unlabelled in the list | overstated | F11 |
| 14 | Filter placeholder | "Filter by name, company or group" | code | supported (it also matches display name and account; optional: name them) | "Filter: name, display name, company, group or account" |
| 15 | Console help header | "Every key changes the view or opens a plan" | code | overstated | F12 |
| 16 | Status line | "Tab and Shift+Tab move, Enter or Space activates"; Console "F1 lists the keys" | code (module.cpp:1291-1299, MoveFocus) | supported | — |
| 17 | Status line + help note | "No screen-reader support in v1" / "English only" | CLAUDE.md | supported | — |
| 18 | Protection line + TI caption | The TI toggle doesn't help with PPL | reasoning, not marked | overstated | F13 |
| 19 | WinDefend keep (PROTSVC) | "can only be reconfigured by itself" | published | overstated | F13 |
| 20 | README F | "Windows sets those per PC" applied to all 55 items | reasoning | wrong | F14 |
| 21 | restore.cmd :nowin | "No Windows installation" on a BitLocker-locked drive | script | wrong | F15 |
| 22 | README A | "WinRE can't open .zip files" | none | unsourced | F16 |
| 23 | RetailDemo, MessagingService | "Nothing else uses it" / "nothing that uses it" | published IoT (the description only) | unsourced | F16 |
| 24 | dot3svc why | "home networks do not use it" | published IoT | unsourced (supportable) | F16 |
| 25 | CaptureService, SSDPSRV, RasAuto | "test it" without how | mixed | unsourced (rule 7 form) | F16 |
| 26 | LicenseManager, NgcSvc keep | Keep, reasoning | reasoning | supported (could be published) | F16 |
| 27 | UsoSvc keep | "@IOT says Windows Update depends on it" | published | overstated (paraphrase) | F16 |
| 28 | 76 preset memberships | IoT, WS16, VDOT and VDI ratings as quoted. Checked live or against the tsv: all IoT ratings for the bloat, Xbox, contested, Bluetooth and Hyper-V entries; all WS16 ratings; VDOT's 20 Disabled; VDI's "Test before disabling", "evaluate in each environment", "disconnected network" and Mail/OneSyncSvc. No bloat entry is contested, no bloat entry has an IoT No guidance, and every WS16 No guidance is named in its pbasis | published | supported | — |
| 29 | Every `published` basis | Has an https URL | gate: render_basis + SOURCES | supported | — |
| 30 | Measured before/after | No RAM or process number shown; none predicted | deferred by decision | supported | — |
| 31 | README A/B | WinRE opens after failed starts; BitLocker key first; Windows is often not on C:; `bcdedit` osdevice; fltmc for the admin check | r3 [37][38]; CLAUDE.md rule 4 | supported | — |
| 32 | restore.cmd comments | Writes only Start/DelayedAutostart for existing keys; never Boot/System; never searches other drives | script (:put checks `reg query` first and skips 0x0/0x1; T is the argument or %~d0) | supported | — |
| 33 | "kept" / "no preset" labels | Hardware seen → kept; never, unknown build, launch-protected or no sysok → no preset | code (KbVerdict = BuildPreset's tests) | supported | — |
| 34 | genkb selftest | Fails on a tier-1 entry without basis, a missing pbasis, published without a URL, unit numbers, owner text, etc. | `--selftest` run: PASS | supported (for unit-bearing numbers; see #9) | — |

**Verdict counts: supported 11, overstated 11, unsourced 7, wrong 5 (34 rows).**

Hardware fit with the owner's machine (14700KF, no iGPU, Windows 10 22H2, PRO Z790-P WIFI):
- cphs, igfxn and IntcDAud fall out as hardware-absent, which is correct with no iGPU.
- intelppm stays on the floor.
- The Bluetooth and Wi-Fi items are kept by evidence, since the board has Wi-Fi and Bluetooth.
- HvHost follows the CPUID hypervisor bit.
- No KB text names the owner's hardware; OWNER_DENY passes.
