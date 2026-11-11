# svcdrv kb-audit 2: the Finalize KB diff (rule 7)

Branch `finalize/svcdrv-kb` at fe827bc, diff `fdd5c31..finalize/svcdrv-kb -- tools/services modules/services/data
docs/design/services.md docs/ARCHITECTURE.md`. Read-only. Auditor: kb-auditor. I formed the verdict without
reading `finalize/kb.md`.

## What I ran and checked
- **Selftest.** `python3 -I tools/services/genkb.py --selftest` passes with 0 failures:
  - 38 must-fail cases, 15 ok texts and 1 warning case, as the README says;
  - the committed KB, 265 entries;
  - 26 fields per K line, and every @KEY is expanded.
- **Regeneration.** `genkb.py <scratch>/services.db` is byte-identical to `modules/services/data/services.db`:
  - 265 entries, so 166 are new (111 services and 55 drivers);
  - bloat 17, stripped 48, minimum 11: the same preset levels as before;
  - 0 warnings.
- **The new entries change no preset.** All 166 have `never=True` and no levels.
- **No unbasised numbers.** I listed every digit in every text field of `services.db`. All of them are identifiers:
  - Windows 10/11, Server 2016, 2004, 20H2, 1703 and 1903;
  - 0x7B and 0x80070422;
  - USB 2/3/3.0, PS/2, "class 1", "subclass 06, protocol 62" and 64-/32-bit;
  - "Start value must be 0".

  No RAM, CPU, FPS, latency, count or savings figure appears anywhere.
- **Ratings against the research files.** A script checked every new service's two ratings against
  `ws2016-services.tsv` and the builder's `finalize/iot-services.tsv`. All 111 match both.
- **Ratings against the live pages.** Because the IoT tsv is the builder's own copy, I checked it against the
  live page:
  - IoT page (live): the rating and description of 41 rows. 39 are new entries, plus UsoSvc and SysMain. All match,
    and so does the legend.
  - WS16 page (live): Appinfo, MSiSCSI, smphost, LanmanServer and SstpSvc, with their comments. All match the tsv.
    The rest of that page fell outside what the fetch returned, so for those rows I relied on the tsv, which
    kb-audit 1 had already checked.
- **Driver pages.** Every `published` driver entry, all 26, against its live page:
  - HIDKBM, USBHOST, USBCLASSES, STORPORT, STORNVME, STORAHCI, FLTMGR, BASICDISPLAY and USBAUDIO2;
  - STOP7B for its list of six services.
- **Gate probe.** I ran the number gate on 46 extra texts (scratchpad `kb2/probe.py`).
- **Sample.** I read all 166 new entries in `kb.py`, not a sample.

## Previous kb-audit findings
These categories are kb-audit 1's: category 2 means the text produces a false claim, category 6 means wording.

| # | Finding | Status | Evidence |
|---|---|---|---|
| F1 | TI banner and Stop-TI "did not finish / nothing stopped it / Manual" | **Open** (code, outside this diff) | `module.cpp:1031,1514` are unchanged on `finalize/svcdrv-kb`, `-logic` and `-ui`. `services.md` still says "`T started` with no `T stopped` marks a crash". This is category 2: it blocks the module merge, not this KB diff |
| F2 | Hadron-column legend | **Partly closed** | `services.md` "List mode" now gives the full, correct list. The UI string at `module.cpp:2959` is unchanged on all three branches |
| F3 | Stripped notes claim use is detected; IoT misquoted | **Closed** | `_STRIP_HOW` says Hadron does not detect use, and the IoT quote is now "enterprises" (checked against the live legend) |
| F4 | Contested Minimum pbasis published | **Closed** | 6 entries use `pbasis='reasoning'` and the F4 wording |
| F5 | SysMain "can be negative" and "memory compression" | **Closed** | `if_off` is labelled reasoning and says how to test. The compression claim is gone. The IoT quote matches the live page |
| F6 | intelppm labelled folklore as a whole | **Closed** | `basis='reasoning'`, and only the tweak is labelled folklore |
| F7 | The gate misses counts, sizes and fractions | **Closed** (required parts). Bypasses remain: S12 | All 6 listed texts fail the gate. Warnings are passed and printed. 16 new must-fail cases |
| F8 | "Default: Windows stock" chip and F8 | **Open** (code) | `module.cpp:2103` is unchanged. Proxy Q2 gives the label |
| F9 | "expected to be near-empty" Drivers note | **Open** (code) | Present at `plan.cpp:269` (`:281` on `-logic`) and in golden `plan-drivers-bloat.txt` |
| F10 | XboxGipSvc class hint unsourced | **Partly closed** | Now `svc:xboxgip`, and the note says why. The service name `xboxgip` is itself unsourced (S15) |
| F11 | Company column | **Partly closed** | The `-logic` and `-ui` branches add "what the file says" in the detail pane. The column header is still "Company" |
| F12 | "Every key changes the view or opens a plan" | **Open** (code) | `module.cpp:2941` |
| F13 | "TI toggle does not help" | **Partly closed** | The WinDefend KB text is fixed. `module.cpp:1818` still says "the TrustedInstaller toggle does not help" without "(reasoning, not tested)" |
| F14 | README F "Windows sets those per PC" | **Open** (code) | `recovery.cpp:240` |
| F15 | `:nowin` hides the BitLocker hint | **Open** (code) | `recovery.cpp:126` is unchanged. The hint is still only under the `reg load` failure |
| F16 | Smaller wording items | **8 of 9 closed** | Closed: RetailDemo, MessagingService, dot3svc, CaptureService (test-how, in the `keep` text), SSDPSRV (`test`), RasAuto (`test`), UsoSvc, and LicenseManager/NgcSvc. Open: README A "WinRE can't open .zip files" (`recovery.cpp:195`). UsoSvc: the live IoT row does say "Windows Update (incl. WSUS) depends on this service", so kb-audit 1's paraphrase concern was mistaken and the new text is right |

KB and text findings: 5 closed, 1 closed with residual bypasses (F7) and 1 partly closed (F10).
Code findings outside this diff: F1, F8, F9, F12, F14, F15 and README A are open, and F2, F11 and F13 are partly
open. They still need the logic and UI branches before the module merges.

## Findings

### Must-fix (blocks merge)

**M1. The plan goldens still contain the KB text from before this diff, so CI fails and the goldens keep the
overstated wording.**
- **Where:**
  - `tests/golden/services/plan-services-stripped.txt`;
  - `tests/golden/services/plan-services-minimum.txt` (lines 19, 26, 30, 34, 60, 64);
  - any other golden that prints a basis line.
- **Text:** "basis: published -- ... rates it OK to disable on Windows 10/11 IoT Enterprise, meaning useful to some
  devices but not all; Hadron adds it only where the hardware or use is absent". The contested rows say
  "basis: published -- ... not needed to boot, network or play", with the raw.githubusercontent VDOT URL.
- **Problem:**
  - `--dump-plan` prints the pbasis, and this diff changed P_STRIP_*, P_CONTESTED, the pbasis kind and the VDOT URL.
  - `git diff --stat fdd5c31..finalize/svcdrv-kb -- tests/` is empty, so `services_check.sh` step 2 fails.
  - The committed goldens show the F3/F4 claims this diff exists to remove.
- **Fix:** run the regeneration in `services.md`:
  ```
  python3 -I tools/services/genkb.py
  wine build/HadronServices.exe --selftest --fixture tests/fixtures/services --update-golden
  ```
  Review the diff, which should change only basis lines and VDOT URLs, and commit it on this branch.

### Should-fix

**S1. storport.**
- **Text:** expl "The system-supplied storage port driver that most modern storage controller drivers (miniports)
  plug into."
- **Problem:** the basis is `published` (@STORPORT), but the page does not say "most". It says vendors "are
  encouraged to use Storport where possible".
- **Rewording:** "The system-supplied storage port driver that storage miniports plug into; Microsoft's AHCI driver
  (StorAHCI) is one, and Microsoft encourages vendors to use it."

**S2. USBXHCI.**
- **Text:** expl "... which also run the USB 2 ports on modern boards."
- **Problem:** @USBHOST does not say this, yet it sits under a `published` basis with no label. It is reasoning.
  It is right for the owner's Z790, which has no EHCI.
- **Rewording:** "Drives USB 3 (xHCI) host controllers. On boards with no separate USB 2 controller it runs the
  USB 2 ports too (reasoning)."

**S3. BasicDisplay.**
- **Text:** note "... and during a graphics driver reinstall."
- **Problem:** the page says "during the transition" while a new WDDM driver is installed.
- **Rewording:** "... and while a new graphics driver is being installed (from removing the old one to installing
  the new one)."

**S4. hvservice. The description is wrong.**
- **Text:**
  - expl "Starts and manages the Windows hypervisor (Hyper-V, WSL2, Memory Integrity)."
  - if_off "Features that need the hypervisor stop: Hyper-V, WSL2, Windows Sandbox and Memory Integrity (reasoning)."
- **Problem:**
  - The boot loader launches the hypervisor (`hypervisorlaunchtype` in the BCD). hvservice.sys is the
    Hypervisor/Virtual Machine Support driver, not what starts it.
  - The if_off is a definite claim with no test behind it.
- **Rewording:**
  - expl: "Kernel support driver for the Windows hypervisor and the features built on it (Hyper-V, WSL2, Windows
    Sandbox, Memory Integrity). The boot loader starts the hypervisor itself (reasoning)."
  - if_off: "Features that use the hypervisor may fail (reasoning, not tested). Hadron never changes it while the
    hypervisor is running."
  - The second sentence matches `HypervisorDrivers()` in `plan.cpp`.

**S5. HDAudBus. Hardware fit.**
- **Text:**
  - expl "onboard audio and GPU HDMI/DisplayPort audio sit on it";
  - if_off "Onboard audio and HDMI/DisplayPort audio stop (reasoning)."
- **Problem:** on many Intel 600/700-series boards with Intel's Smart Sound Technology driver installed, onboard
  audio sits on Intel's own audio bus driver, not HDAudBus. The owner's PRO Z790-P may be one of them. As written,
  it says the owner's onboard audio dies, which may be false on his PC.
- **Rewording:**
  - expl: "The bus driver for High Definition Audio controllers, such as a GPU's HDMI/DisplayPort audio and
    onboard audio that uses Microsoft's HD Audio bus."
  - if_off: "Audio devices on that bus stop (reasoning). Some Intel boards run onboard audio on Intel's own bus
    driver instead; the device's Driver details show which one loads."

**S6. The default note on all 29 reasoning drivers.**
- **Text:** "From the driver's role in Windows; Hadron found no Microsoft page that describes it."
- **Problem:** this claims a search that found nothing. Microsoft has pages for NDIS, DXGKrnl, NTFS, KMDF, the TPM
  and others. It also sits oddly next to WdBoot and WdNisDrv, which cite @DEFCOMPAT.
- **Rewording:** "Reasoning from the driver's role in Windows; Hadron cites no Microsoft page for it."

**S7. iphlpsvc.**
- **Text:** "Some Xbox Live multiplayer features use Teredo (reasoning)."
- **Problem:** this is not something you can derive from how the hardware works, so it is not reasoning in the
  rule-7 sense. It is a fact about Xbox networking that needs a source.
- **Rewording:** either cite a Microsoft or Xbox support page that says it, after reading it, or write: "Xbox
  party chat and some multiplayer games are reported to need Teredo (not stated on either page). Test it: compare
  Settings > Gaming > Xbox Networking before and after."

**S8. TPM.**
- **Text:** "... and anti-cheats that check the TPM can fail (reasoning)."
- **Problem:** this is unsourced and not derivable. On the owner's Windows 10 it is also speculative.
- **Rewording:** "BitLocker with a TPM protector and Windows Hello keys fail (reasoning). Some anti-cheats are
  reported to check the TPM (not sourced)."

**S9. Appinfo.**
- **Text:** "Hadron itself always runs as administrator, so it needs this to start (reasoning)."
- **Problem:** this is overstated. Appinfo is needed for the UAC elevation prompt, not to run as administrator.
  An already-elevated parent, or UAC off, does not use it.
- **Rewording:** "Hadron asks for administrator rights, so starting it from a normal desktop goes through this
  service's UAC prompt (reasoning)."

**S10. TermService.**
- **Text:** if_off "@IOT says that to prevent remote use, clear the boxes on the Remote tab of System Properties
  instead."
- **Problem:** the quote is accurate, but the "If it's off" field holds the source's advice, not the effect.
- **Rewording:** "Remote Desktop connections to this PC stop (reasoning). @IOT says that to prevent remote use,
  clear the checkboxes on the Remote tab of System Properties rather than disable it."

**S11. WpnUserService.**
- **Text:** note "@IOT's rating is the client-side one."
- **Problem:** the IoT page is for IoT Enterprise, not the client.
- **Rewording:** "@IOT's rating is for Windows 10/11 IoT Enterprise, the closer of the two to a desktop."

**S12. Gate bypasses.**
- **Problem:** the README says the gate rejects counts and sizes. These pass:
  - "Frees about 1.5 gigabytes." This one is a bug: a decimal before a word unit is skipped, because
    `(?![.\d])` and the `.` lookbehind exclude it.
  - "40 fewer running processes." Only one modifier is allowed.
  - "Removes 30 Windows services."
  - "Removes 30 of the services."
  - "Removes 30+ services."
  - "Cuts the process count by 40."
  - "Process count drops from 120 to 80."
  - "40 apps stop."
  - "Frees one gigabyte." and "Saves a gigabyte."
  - "One second faster."
  - "Lowers CPU use by 3 points."
- **Fix:** the KB today holds only identifier digits (see the scan above), so add the allow-list check kb-audit 1
  suggested. Fail any digit outside:
  - a `[[...]]` marker;
  - `Windows 10(/11)?`, `Server 2016`, `20H2`, a 4-digit release (`1607|1703|1903|2004`), `0x[0-9A-F]+`;
  - `USB [23](\.0)?`, `PS/2`, `class 1`, `subclass \d+, protocol \d+`, `(32|64)-bit`, `Start value must be 0`.

  Add the texts above as must-fail cases.

**S13. KtmRm.**
- **Text:** "... while it is disabled their transactions with a kernel resource manager fail."
- **Problem:** the page says "any MSDTC transaction interacting with a Kernel Resource Manager fails". It does not
  cover KTM's own transactions.
- **Rewording:** "@IOT says MSDTC and KTM start it when needed, and that while it is disabled any MSDTC
  transaction that involves a kernel resource manager fails."

**S14. usbaudio and USBSTOR.**
- **Problem:** two small unsourced details sit under a `published` basis:
  - usbaudio: "class 1" is supported by @USBAUDIO2 ("usbaudio.sys (USB Audio 1.0 driver)"), not by @USBCLASSES.
  - USBSTOR: "(Bulk-Only)" is not on @USBCLASSES.
- **Fix:**
  - usbaudio note: "@USBCLASSES lists Usbaudio.sys for the USB Audio class; @USBAUDIO2 calls it the USB Audio 1.0
    driver."
  - USBSTOR expl: "The inbox driver for USB flash drives and USB disks that don't use UAS."

**S15. XboxGipSvc.**
- **Text:** hw `svc:xboxgip`, and the note "a device whose driver is xboxgip (the Xbox accessory driver)".
- **Problem:** the service name and role are unsourced. This is the same gap as F10, moved from the device class
  to the service name. A Bluetooth-connected Xbox controller does not use xboxgip, so it never keeps the row.
- **Fix:** record a measurement: the Driver details of a USB-connected Xbox controller on a real PC. Or add to the
  note: "(reasoning: the driver name is not sourced; a Bluetooth-connected controller uses a different driver, so
  it does not keep this row)".

## Audit table
| # | Entry | Claim | Basis given | Verdict | Suggested rewording |
|---|---|---|---|---|---|
| 1 | storport | "most modern storage controller drivers plug into" | published @STORPORT | overstated | S1 |
| 2 | stornvme, storahci | System-supplied NVMe miniport; Storport miniport for SATA AHCI that replaced MSAHCI | published | supported | — |
| 3 | ACPI, disk, partmgr, volmgr, volsnap, volume | @STOP7B: their Start value must be 0 (Boot) | published @STOP7B | supported (exactly the page's six) | — |
| 4 | USBSTOR, UASPStor | Usbstor.sys for USB mass storage; Uaspstor.sys for UAS, subclass 06 protocol 62 | published @USBCLASSES | supported (minor: S14) | — |
| 5 | FltMgr | System-supplied; installed with Windows; active when a minifilter loads | published @FLTMGR | supported | — |
| 6 | USBXHCI | "also run the USB 2 ports on modern boards" | published @USBHOST | overstated (unlabelled reasoning) | S2 |
| 7 | UCX01000, USBHUB3, usbccgp, HidUsb | UCX sits before xHCI; the hub manages and enumerates; usbccgp is the composite parent; Hidclass/Hidusb | published | supported | — |
| 8 | kbdhid, mouhid, kbdclass, mouclass, i8042prt | Mapper drivers; "all keyboards and keypads", "all mice and touchpads"; i8042prt is the PS/2 function driver | published @HIDKBM | supported | — |
| 9 | usbaudio, usbaudio2 | Usbaudio.sys for USB Audio; the USB Audio 2.0 driver ships from 1703 | published | supported | S14 (cite) |
| 10 | BasicDisplay, BasicRender | Fallback driver; always used with BasicRender (WARP) | published @BASICDISPLAY | supported (minor: S3) | — |
| 11 | hvservice | "Starts and manages the Windows hypervisor" | reasoning | wrong | S4 |
| 12 | HDAudBus | Onboard audio stops | reasoning | overstated (owner's hardware) | S5 |
| 13 | TPM | Anti-cheats that check the TPM can fail | reasoning | unsourced | S8 |
| 14 | 29 reasoning drivers | "Hadron found no Microsoft page that describes it" | reasoning | overstated | S6 |
| 15 | storport, stornvme, storahci (`_BOOT_PATH`) | Hadron never changes drivers on the boot disk's path | reasoning | supported (`CheckGuard` bootChain plus the floor) | — |
| 16 | 111 new services | @IOT and @WS16 ratings and paraphrases | published | supported (all 111 match the tsvs; 39 IoT and 5 WS16 rows checked live) | — |
| 17 | UsoSvc | @IOT says Windows Update depends on it | published | supported (live row) | — |
| 18 | iphlpsvc | Xbox Live multiplayer uses Teredo | reasoning | unsourced | S7 |
| 19 | Appinfo | Hadron "needs this to start" | reasoning | overstated | S9 |
| 20 | TermService | @IOT: clear the Remote tab instead | published | supported (wrong field) | S10 |
| 21 | WpnUserService | "@IOT's rating is the client-side one" | published | overstated | S11 |
| 22 | KtmRm | "their transactions ... fail" | published | overstated (slight) | S13 |
| 23 | Audiosrv, AudioEndpointBuilder | Hadron's floor keeps it | reasoning | supported (`plan.cpp` Floor) | — |
| 24 | Dhcp, Netlogon, EapHost, NlaSvc, tzautoupdate, RasMan, WbioSrvc, msiserver, VaultSvc, DeviceAssociationService | Effects inferred from the service's role | reasoning, labelled inline | supported | — |
| 25 | XboxGipSvc | Kept when an xboxgip device was seen | published (membership) | unsourced (hint) | S15 |
| 26 | genkb gate | Fails the 6 kb-audit F7 texts; warnings printed | selftest | supported | — |
| 27 | genkb gate and README | "rejects counts ... sizes and durations in words" | selftest | overstated (11 bypasses) | S12 |
| 28 | All KB text | No unbasised quantity | digit scan of `services.db` | supported | — |
| 29 | 166 names | Exist on 19045 client | KB comment; Server-only names left out | supported (none is Server-only; all are present on 22H2 Pro) | — |
| 30 | plan goldens | Match the KB | CI `services_check.sh` | wrong (stale) | M1 |
| 31 | README | 38 must-fail, 15 ok, 1 warning case | selftest output | supported | — |
| 32 | 166 entries | Explanation only, no recommendation, no preset change | `never=True`, no levels; text read in full | supported (the only advice in a text field is TermService's quote, S10) | — |

**Verdict counts: supported 19, overstated 8, unsourced 3, wrong 2 (32 rows).**
**Findings: must-fix 1 (M1), should-fix 15 (S1-S15).**

Hardware fit (14700KF, no iGPU, Windows 10 22H2, PRO Z790-P WIFI):
- The new entries change no preset, so nothing new can be disabled on the owner's PC.
- HDAudBus (S5) is the one text that may be false on his board.
- USBXHCI's USB 2 claim (S2) is right for Z790 but unlabelled.
- TPM's anti-cheat clause (S8) does not apply on Windows 10.
- No text names the owner's hardware; OWNER_DENY passes.
