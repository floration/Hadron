# svcdrv-kb audit: fix/svcdrv-kb (24 KB entries, STOCK table, stock UI)

Diff `git diff origin/main...origin/fix/svcdrv-kb` (read-only; I worked on an export in scratchpad `br/`).

**What I ran**
- `genkb.py --selftest`: PASS, 0 failed, 289 entries.
- Regeneration: 289 entries; bloat 17, stripped 48, minimum 11; 213 explanation-only; 0 warnings. The regenerated db is **byte-identical** to `modules/services/data/services.db`.
- 24 new entries confirmed (265 to 289). No preset changed.
- Fixture: it has only WSearch and BrokerInfrastructure of the five STOCK services, which is where the golden's "2 of 109" comes from. The bloat, stripped and minimum goldens go from 3 to 2 "no KB entry" items because Fax now has an entry.

**Pages fetched live (WebFetch; curl is blocked by the proxy):** @WUERRCLIENT, @W32TWG, @TASKSCHED, @SEARCHPERF, @STARTMENU, @UVC, @USBPRINT, @RDBSS, @LLTDCSP, @HVNET, @USBCLASSES, @IOT (Fax row), @MSIBOARD, @INTELLMS, @INTELDAL, @FNMSICENTER, @FNMYSTIC, @FNRTKAUD. Every quoted "@X says" text matches its page, except the two flagged below: mrxsmb's paraphrase and the storvsc note.

## Verdict table

| # | Entry | Claim | Basis | Verdict | Note |
|---|---|---|---|---|---|
| 1 | Fax | @IOT: "OK to disable", no description; not on @WS16 | reasoning + published | supported | The live row reads "(Fax) Manual 🟢 OK to disable" with an empty description. ws2016-services.tsv has no Fax row. |
| 2 | usbhub | @USBCLASSES Usbhub.sys quote; xHCI hubs use USBHUB3 (reasoning) | published | supported | Quote is verbatim. The page says Usbhub3 loads "when a SuperSpeed hub is attached to an xHCI controller"; the wider claim is labelled reasoning. |
| 3 | usbvideo | @UVC quote | published | supported | Verbatim. |
| 4 | usbprint | @USBPRINT, both quotes | published | supported | Verbatim. |
| 5 | usbscan | "manages USB digital cameras and scanners", WIA | published | supported | Verbatim. The page expands WIA as "Windows Imaging Architecture"; the explanation uses the usual name, "Windows Image Acquisition". Harmless. |
| 6 | usbser | "In Windows 10, Usbser.inf loads Usbser.sys…" | published | supported | Verbatim. |
| 7 | wcifs | "(Windows Sandbox, for example) may fail" | reasoning | unsourced | Vendor fact used as an example, with no source: s1. |
| 8 | FileCrypt | Hadron doesn't know what it does | reasoning | supported | Honest. |
| 9 | rdbss | "The file system library network redirectors are built on" | published | overstated | @RDBSS: only the SMB redirector uses rdbss.sys ("currently the only driver that can be built as a non-monolithic…"). All others "link with the rdbsslib.lib static library": s2. |
| 10 | mrxsmb | "@RDBSS names the … SMB redirector as a non-monolithic network mini-redirector built on RDBSS" | published | supported | Paraphrase. The page says it "is currently the only driver that can be built as" one, and "built on RDBSS" is implied. Quote it instead: s2. |
| 11 | mrxsmb20 | SMB2/3 connections fail | reasoning | supported | — |
| 12 | bowser | "Workstation service lists it as a dependency, so the SMB client may fail to start (reasoning)" | reasoning | unsourced | A registry fact labelled reasoning; the owner can check it in one command. "May" undersells it: the SCM won't start a service whose dependency is disabled: s3. |
| 13 | NdisVirtualBus | Creates virtual adapters | reasoning | supported | Says how to check (Device Manager). |
| 14 | WFPLWFS | May break filtering/VPN | reasoning, not tested | supported | — |
| 15 | lltdio | "Discovers the layout of the local network **for the Network Map**"; "can no longer map the local network" | published (quote) + unlabelled | **wrong** | Neither @LLTDCSP quote mentions a Network Map. Network and Sharing Center lost the Network Map in Windows 8 (Wikipedia, "Features removed from Windows 8": "Network Map is removed from Network and Sharing Center"). So Windows 10 has no inbox map: M3. |
| 16 | rspndr | "shows up on their network maps" | published + reasoning | overstated | Same issue: only older Windows or third-party LLTD tools draw maps: M3. |
| 17 | vmbus | @HVNET VMBus quote | reasoning (note cites) | supported | Verbatim. The page names no driver files, and the note says so. |
| 18 | netvsc | @HVNET NetVSC quote | reasoning | supported | Verbatim. |
| 19 | storvsc | Note: @HVNET "describes VMBus and the guest-side virtual service clients" | reasoning | overstated | @HVNET is a networking page: it has no storage content and never mentions StorVSC. The shared note implies support it doesn't give: s4. |
| 20 | MSI_Center_Service | Service name from @FNMSICENTER; "Windows does not use it" | reasoning | supported (label) | The page does say "a service (MSI_Center_Service)". Its "typically located" folder is `…\MSI\One Dragon Center`; it lists `…\MSI\MSI Center\` too. `_OEM`'s "The vendor does not document this service" is an absolute claim; what was checked is "Hadron found no page": s5. |
| 21 | Mystic_Light_Service | Name and folder from @FNMYSTIC | reasoning | supported (label) | "It is the service "Mystic_Light_Service"" is verbatim. Test given. |
| 22 | RtkAudioUniversalService | "Realtek does not say what it does"; "Sound itself comes from the audio driver, not from this service" | reasoning | overstated | First clause: absence of evidence stated as fact. Second: stated flatly, while the test that follows shows it is a hypothesis. The name is verbatim on @FNRTKAUD. The ALC897 codec is on @MSIBOARD, so it fits the owner's board: s5. |
| 23 | LMS | @INTELLMS quote; "AMT is a business (vPro) feature" | published | supported | Quote verified. The vPro clause has no label. It fits the owner's hardware: Z790 plus a 14700KF has no AMT. s6. |
| 24 | jhi_service | @INTELDAL, both quotes; installed by the ME installer | published | supported | Verbatim. The page names no Windows service, and the note labels the name link as reasoning. |
| 25 | STOCK msiserver 3/- | "The default Startup type of the Windows Installer service is Manual." (Windows 10/11) | published | supported | Verbatim. Applies to: Windows 10, Windows 11. |
| 26 | STOCK W32Time 3/- | Workgroup: "The startup value for the Windows Time service is Manual"; trigger-start by default | published | supported | Verbatim. Applies to all supported client and Server versions. Scope is honest. |
| 27 | STOCK TimeBrokerSvc 3/- | "is set to 3 by default" | published | **wrong (render)** | The quote is right, but services.db ships the raw marker: `"is set to [[3\|published\|Microsoft's Task Scheduler startup failure page (https://…)]] by default"`. Nothing in `modules/services` renders `[[ ]]`. Services `genkb.expand()` builds the K line from the raw text; `render()` validates it and its stripped output is thrown away. This is the only marker in the db, and the fixture has no TimeBrokerSvc row, so no golden catches it: M1. |
| 28 | STOCK WSearch 2/1 | "should have the following configuration", "Automatic (Delayed Start)" | published | supported | Verbatim. The note says it is an expected setting, not a build statement. |
| 29 | STOCK BrokerInfrastructure 2/0 | "BrokerInfrastructure is an automatic service."; delayed start not mentioned (reasoning) | published + reasoning | supported | Verbatim. The symptom heading is "Windows 2012 R2, Windows 10, or Windows 2016". This is the only stock note without "The page names no build": s7. |
| 30 | plan.cpp `r.reason` | "Windows stock start type for build 19045" | (fixed text) | **overstated** | None of the five pages names 19045 or 22H2; I checked each. The basis line in the same row says "The page names no build". The pages scope themselves as "Supported versions of Windows Client", a live list. From memory, not fetched: Windows 10 22H2 Home/Pro left mainstream support on 2025-10-14. Shipping an inference as a sourced build fact breaks rule 7: M2. |
| 31 | Chip / F8 / F1 help / detail (Services tab) | "Default: Windows stock", "Stock", "Plan: Default, Windows stock" | — | overstated | `StockKnown()>0` drops "(no data…)" when only 5 KB services have a value (2 of 109 on the fixture). The label reads as a full reset to stock. The plan's first note does give the count, so this is should-fix, not must-fix. The detail line "Auto (delayed) (published)" gives no scope: s8. |
| 32 | file.net label | "third-party, user-contributed process database" (kb.py comment, README) | — | overstated (mild) | The fetched pages' body text is written by file.net ("Since 2005, file.net has helped users…"); user ratings and comments are a separate section ("There are no user opinions yet"). "Third-party" is the accurate and useful part: s9. |
| 33 | Counts (fix doc, README, design) | 289 entries, 213 explanation-only, 5 stock, presets 17/48/11, goldens 3 to 2 | genkb | supported | Re-run and matched. |
| 34 | selftest.cpp stock check | Every stock value is published (with a URL) or owner-measured, never on a storage driver | — | supported | Does not check that notes are free of markers (M1). |

**Verdict counts: supported 24, overstated 7, unsourced 2, wrong 2 (34 rows).** Overstated: rows 9, 16, 19, 22, 30, 31, 32. Unsourced: 7, 12. Wrong: 15, 27.

**Is file.net acceptable?** Yes, as a pointer for a service's name and folder only. That is how it is used: basis `reasoning`, the label says "(third-party)", and it is never cited for behaviour. The risk is low because the KB is looked up by service name, so a wrong name is inert (never shown). It still isn't `published` in rule 7's sense, so the basis must stay `reasoning`, and it does. Better: replace it with an `owner-measured` `sc qc <name>` capture from the owner's PC where MSI Center or Realtek UAD is installed.

**Hardware:** no preset change, so nothing new is disabled on the 14700KF / Z790 / Windows 10 PC. Hardware references are correct: ALC897 is on @MSIBOARD, the board has no AMT, and none of the new text depends on an iGPU.

## Must-fix

**M1. TimeBrokerSvc's stock note ships a literal `[[…]]` marker** (row 27). Two parts:
- Fix the pipeline in `tools/services/genkb.py`. In `expand()`, first line, add `text = B.MARK_RE.sub(lambda m: m.group(1), text)`. In `check_entry`, add `if '[[' in line or ']]' in line: errors.append('%s: marker shipped literally' % where)`. Add a selftest case that a stock or basis note with a marker renders without `[[`, as bios genkb already does at line ~397. Add TimeBrokerSvc at Disabled or Auto to the fixture so a golden shows the stock note.
- Or, with no genkb change, reword the stock_note. Current text: `'@TASKSCHED (supported Windows client versions) says to check that its Start value "is set to [[3|published|@TASKSCHED]] by default" (Manual). The page names no build; Hadron uses it as the Windows 10 stock value.'`
  Replacement: `'@TASKSCHED (supported Windows client versions) tells you to check that its Start value is the Manual value "by default" and, if not, to set it back. The page names no build; Hadron uses it as the Windows 10 stock value.'`
  I recommend the genkb fix: the README promises `[[shown|kind|@KEY]]` works "as in the bios KB", and today it doesn't.

**M2. Plan row reason** (row 30), `modules/services/plan.cpp:396`. Current text: `r.reason = L"Windows stock start type for build 19045";`
Replace with:
```cpp
r.reason = e->stockBasis == L"owner-measured"
    ? L"Windows stock start type, measured on build " + c.build
    : L"Windows stock start type from a Microsoft page (it names no build)";
```
Regenerate `plan-services-stock.txt`. The golden's "why:" line then agrees with its basis line.

**M3. lltdio / rspndr: Network Map** (rows 15–16).
- lltdio expl. Current: `'Discovers the layout of the local network for the Network Map, and runs quality-of-service checks such as bandwidth estimation.'` Replace: `'Lets this PC discover the layout of the local network and start quality-of-service checks such as bandwidth estimation. Windows 10 has no built-in network map that shows the result (reasoning).'`
- lltdio if_off. Current: `'This PC can no longer map the local network (reasoning). …'` Replace: `'Tools on this PC that map the network with LLTD stop finding devices (reasoning, not tested). Internet access and file sharing don\'t depend on it (reasoning).'`
- rspndr if_off. Current: `'This PC no longer appears on other computers\' network maps (reasoning).'` Replace: `'LLTD mapping tools on other computers no longer find this PC (reasoning, not tested).'`
- rspndr expl. Current: `'… so this PC shows up on their network maps.'` Replace: `'Answers other computers\' Link Layer Topology Discovery requests, so this PC can be found on the network.'`

## Should-fix

- **s1. wcifs if_off** (row 7). Current: `'Features built on container file isolation (Windows Sandbox, for example) may fail (reasoning, not tested).'` Replace: `'Features built on container file isolation may fail (reasoning, not tested); Windows Sandbox is a likely example (not sourced).'`
- **s2. rdbss / mrxsmb** (rows 9–10).
  - rdbss expl. Current: `'The file system library network redirectors are built on; the SMB client is one.'` Replace: `'The file system driver the SMB client\'s redirector runs on. Other network redirectors link a static copy of the library and don\'t need it.'`
  - mrxsmb note. Current: `'@RDBSS names the Microsoft Server Message Block (SMB) redirector as a non-monolithic network mini-redirector built on RDBSS.'` Replace: `'@RDBSS: "The Microsoft Server Message Block (SMB) redirector … is currently the only driver that can be built as a non-monolithic network mini-redirector driver"; rdbss.sys "is automatically loaded if any non-monolithic network mini-redirectors are registered".'`
- **s3. bowser if_off** (row 12). Current: `'On Windows 10 the Workstation service (LanmanWorkstation) lists it as a dependency, so the SMB client may fail to start (reasoning).'` Replace: `'If the Workstation service (LanmanWorkstation) lists it as a dependency, Windows won\'t start that service, and the SMB client stops (reasoning). Check it: run sc qc LanmanWorkstation and look under DEPENDENCIES.'`
- **s4. storvsc note** (row 19). Current: `note=_HV_NOTE`. Replace: `note='Reasoning: @HVNET describes VMBus and a network client (NetVSC); it does not mention storage or name this driver.'`
- **s5. OEM wording** (rows 20, 22).
  - `_OEM`. Current: `' The vendor does not document this service; …'` Replace: `' Hadron found no vendor documentation for this service; …'`
  - RtkAudioUniversalService expl. Current: `'… Realtek does not say what it does (reasoning).'` Replace: `'… Hadron found no Realtek page that says what it does.'`
  - RtkAudioUniversalService if_off. Current: `'Sound itself comes from the audio driver, not from this service (reasoning).'` Replace: `'Playback should not need it, because sound goes through the audio driver (reasoning, not tested).'`
  - MSI_Center_Service basis_note. Current: `'… in MSI Center\'s folder, from Micro-Star; …'` Replace: `'… in MSI Center\'s folder (or One Dragon Center\'s, its older name), from Micro-Star; …'`
- **s6. LMS if_off** (row 23). Current: `'AMT is a business (vPro) feature; on a PC without it, …'` Replace: `'AMT is a business (vPro) feature (reasoning); on a PC without it, …'`
- **s7. BrokerInfrastructure stock_note** (row 29). Current: `'@STARTMENU (a Start menu symptom on Windows 10): "BrokerInfrastructure is an automatic service." The page does not mention delayed start; Hadron reads it as plain Automatic (reasoning).'` Replace: `'@STARTMENU (a Start menu symptom on Windows Server 2012 R2, Windows 10 and Windows Server 2016): "BrokerInfrastructure is an automatic service." The page does not mention delayed start; Hadron reads it as plain Automatic (reasoning). The page names no build; Hadron uses it as the Windows 10 stock value.'`
- **s8. Chip, F8, F1 help and detail** (row 31), `module.cpp`.
  - `StockChipLabel`. Current: `L"Default: Windows stock"`. Replace: `L"Default: Windows stock (" + std::to_wstring(StockKnown()) + L" items)"`.
  - Console F8. Current: `L"Stock"`. Replace: `L"Stock (" + n + L")"`.
  - F1 help. Current: `L"Plan: Default, Windows stock"`. Replace: `L"Plan: Default, Windows stock (only the " + n + L" items with a sourced value)"`.
  - Detail line (module.cpp:2051). Current: `StartLabel(...) + L" (" + BasisLabel(e->stockBasis) + L")"`. Replace with `… + L" (" + BasisLabel(e->stockBasis) + (e->stockBasis == L"published" ? L"; the page names no build)" : L")")`.
  - Update the UI selftests at module.cpp:4002/4008 to match.
- **s9. file.net label** (row 32). Current, in the kb.py comment and the README: `"a third-party, user-contributed process database"`. Replace: `"a third-party process database (not the vendor; its pages mix its own write-ups with user ratings)"`. Later, replace the name citations with an `owner-measured` `sc qc` capture where the software is installed.

## OK (no change)
Fax, usbhub, usbvideo, usbprint, usbscan, usbser, FileCrypt, mrxsmb20, NdisVirtualBus, WFPLWFS, vmbus, netvsc, Mystic_Light_Service, jhi_service. Stock notes for msiserver, W32Time and WSearch. Counts and goldens. The selftest's stock-basis check. The design-doc and README stock sections, which already say no page names 19045.

**Sources consulted outside the KB:** [Features removed from Windows 8 (Wikipedia)](https://en.wikipedia.org/wiki/Features_removed_from_Windows_8), used only to check the Network Map claim, not proposed as a KB source.

**Files:** `/home/user/Hadron/tools/services/kb.py`, `/home/user/Hadron/tools/services/genkb.py` (`expand()`, `check_entry`), `/home/user/Hadron/modules/services/plan.cpp:396`, `/home/user/Hadron/modules/services/module.cpp` (lines 2051, 2339, 2929, 3482), `/home/user/Hadron/tests/golden/services/plan-services-stock.txt`. Scratchpad export with regenerated db: `/tmp/claude-0/-home-user/06948d58-303b-5075-bd9c-8ae81495b222/scratchpad/br/`.
