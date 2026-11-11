# svcdrv-stock audit: fix/svcdrv-stock (db97761): table stock values, disagree list, WinSxS manifest reader

Diff: `git diff origin/fix/svcdrv-kb...origin/fix/svcdrv-stock`. This was read-only: I worked on an export in scratchpad `br/`.

**What I ran**
- `genkb.py --selftest`: PASS, 0 failed, 289 entries, 27-field K lines.
- Regeneration: 289 entries; bloat 17, stripped 48, minimum 11; 213 explanation-only; 0 warnings. The output is **byte-identical** to `modules/services/data/services.db`.
- `stock_src` counts in the db: `table` 178, `microsoft` 5, `disagree` 5, empty 101 (84 drv + 17 svc). Every published stock value has basis `published` (183). No driver has `table`. These match fix doc §7.
- Using kb.py: TABLE_WINTIPS has 230 rows and TABLE_TWEAKHOUND 228. Wintips has 2 rows TweakHound lacks (DialogBlockingService, MsKeyboardFilter). 11 rows differ as text; 6 of them differ in start type (BITS, DeviceAssociationService, W32Time, WLANSVC, wscsvc, wuauserv). The other 5 differ only in a "Trigger Start" tag (CDPSvc, DoSvc, SgrmBroker, sppsvc, StorSvc). "Differ on six services" is correct. 40 service names that both tables list and agree on have no KB entry; that matches the follow-up list.

**Pages fetched (WebFetch; curl is refused by the egress proxy for both hosts)**
- @TBLTWEAKHOUND: `tweakhound.com/?p=18808` redirects to `https://www.tweakhound.com/2020/12/23/windows-10-default-services-v20h2/`.
  - Header: "Windows 10 Default Services (version 20H2, build 19042.685, 23DEC2020) by TweakHound.com", and "Default install, all options accepted during install". No edition is stated, and no other site is named as a source.
  - It has a "VirtualBox Guest Additions Service" row (Automatic). Per-user rows carry suffixes ("Sync Host_2ccb4").
- @TBLWINTIPS: the PDF's title is "Default Startup Type of Services on Windows 10 PRO OS by: wintips.org".
  - Columns: Service Name, Startup Type, Log On As, Status. No build, date or capture method is stated; 2021-09 comes only from the URL path.
  - It has a "Microsoft Office Click-to-Run Service" row and an "Agent Activation Runtime_38b455b" row, Hyper-V rows, and no VirtualBox or VMware row.
- Both pages give display names. A few names carry a short name in parentheses.

## Spot-check: 64 services against kb.py

I looked up each display name on both pages. **Every page value equals the kb.py transcription (64 of 64 on each page).** The list includes every value in either table that has "Delayed Start" and every service on the runtime list except dcsvc, which is in neither table. "KB" is the value genkb ships: `(table)` means two agreeing tables, `(microsoft)` means the STOCK row.

| Service | @TBLWINTIPS | @TBLTWEAKHOUND | KB | Tag |
|---|---|---|---|---|
| BITS | Manual | Automatic (Delayed Start) | disagree | delayed, runtime |
| CDPSvc | Automatic (Delayed Start) | Automatic (Delayed Start, Trigger Start) | 2/1 (table) | delayed |
| DeviceAssociationService | Manual (Trigger Start) | Automatic (Trigger Start) | disagree | |
| DoSvc | Automatic (Delayed Start) | Automatic (Delayed Start, Trigger Start) | no KB entry | delayed |
| SgrmBroker | Automatic (Delayed Start) | Automatic (Delayed Start, Trigger Start) | no KB entry | delayed |
| sppsvc | Automatic (Delayed Start) | Automatic (Delayed Start, Trigger Start) | 2/1 (table) | delayed |
| StorSvc | Automatic (Delayed Start) | Automatic (Delayed Start, Trigger Start) | 2/1 (table) | delayed |
| W32Time | Automatic (Delayed Start) | Manual (Trigger Start) | 3/- (microsoft) | delayed |
| WlanSvc | Automatic | Manual | disagree | runtime |
| wscsvc | Automatic | Automatic (Delayed Start) | disagree | delayed |
| wuauserv | Automatic (Trigger Start) | Manual (Trigger Start) | disagree | |
| MapsBroker | Automatic (Delayed Start) | Automatic (Delayed Start) | 2/1 (table) | delayed |
| UsoSvc | Automatic (Delayed Start) | Automatic (Delayed Start) | 2/1 (table) | delayed |
| DispBrokerDesktopSvc | Automatic (Delayed Start) | Automatic (Delayed Start) | no KB entry | delayed |
| edgeupdate | Automatic (Delayed Start) | Automatic (Delayed Start) | no KB entry | delayed |
| WSearch | Automatic (Delayed Start) | Automatic (Delayed Start) | 2/1 (microsoft) | delayed |
| stisvc | Manual | Manual | 3/- (table) | runtime |
| IKEEXT | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | runtime |
| PolicyAgent | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | runtime |
| iphlpsvc | Automatic | Automatic | 2/0 (table) | runtime |
| WinHttpAutoProxySvc | Manual | Manual | 3/- (table) | runtime |
| WinRM | Manual | Manual | 3/- (table) | runtime |
| dmwappushservice | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | runtime |
| camsvc | Manual | Manual | no KB entry | runtime |
| ssh-agent | Disabled | Disabled | no KB entry | runtime |
| DiagTrack | Automatic | Automatic | 2/0 (table) | |
| XblGameSave | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| SysMain | Automatic | Automatic | 2/0 (table) | |
| DPS | Automatic | Automatic | 2/0 (table) | |
| PcaSvc | Manual | Manual | 3/- (table) | |
| Spooler | Automatic | Automatic | 2/0 (table) | |
| RemoteRegistry | Disabled | Disabled | 4/- (table) | |
| Fax | Manual | Manual | 3/- (table) | |
| TrkWks | Automatic | Automatic | 2/0 (table) | |
| Themes | Automatic | Automatic | 2/0 (table) | |
| EventLog | Automatic | Automatic | 2/0 (table) | |
| tzautoupdate | Disabled | Disabled | 4/- (table) | |
| MSDTC | Manual | Manual | 3/- (table) | |
| Audiosrv | Automatic | Automatic | 2/0 (table) | |
| TabletInputService | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| lfsvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| RetailDemo | Manual | Manual | 3/- (table) | |
| LanmanServer | Automatic (Trigger Start) | Automatic (Trigger Start) | 2/0 (table) | |
| NetTcpPortSharing | Disabled | Disabled | 4/- (table) | |
| WerSvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| wlidsvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| mpssvc | Automatic | Automatic | 2/0 (table) | |
| msiserver | Manual | Manual | 3/- (microsoft) | |
| TimeBrokerSvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (microsoft) | |
| BrokerInfrastructure | Automatic | Automatic | 2/0 (microsoft) | |
| WbioSrvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| WpnService | Automatic | Automatic | 2/0 (table) | |
| WinDefend | Automatic | Automatic | 2/0 (table) | |
| bthserv | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| wisvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| PhoneSvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| SEMgrSvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| SCardSvr | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| WMPNetworkSvc | Manual | Manual | 3/- (table) | |
| XboxGipSvc | Manual (Trigger Start) | Manual (Trigger Start) | 3/- (table) | |
| XblAuthManager | Manual | Manual | 3/- (table) | |
| XboxNetApiSvc | Manual | Manual | 3/- (table) | |
| MsKeyboardFilter | Disabled | (not listed) | no KB entry | |
| DialogBlockingService | Disabled | (not listed) | no KB entry | |

Also fetched: "Microsoft (R) Diagnostics Hub Standard Collector Service" is **Manual on both pages**. The fix doc lists it as unmapped, but it maps to the existing KB entry `diagnosticshub.standardcollector.service` (s4).

The mapping `_table_cfg` is correct for every row above. Delayed becomes 2/1, Automatic with or without Trigger becomes 2/0, Manual becomes 3/- and Disabled 4/-. Trigger is ignored, as documented.

## Verdict table

| # | Entry | Claim | Basis given | Verdict | Note |
|---|---|---|---|---|---|
| 1 | 178 `table` values | Both tables list the same start type | published (two tables, each labelled with its URL) | supported | 64 of 64 spot-checked values match on both pages, including every Delayed value. |
| 2 | "Two independent captures" (kb.py comment, fix doc, DECISIONS) | Independent; they differ on six services; each has rows the other lacks (Office in one, VirtualBox in the other) | page inspection | supported | Confirmed: Office C2R appears only on wintips, VirtualBox Guest Additions only on TweakHound, and 6 start types differ. They also differ in Trigger tags, per-user suffixes (`_2ccb4` vs `_38b455b`) and dates. Neither names the other as a source. Not a copy. |
| 3 | `_TABLE_HEAD` labels | "@TBLWINTIPS (Windows 10 Pro, no version stated)"; "@TBLTWEAKHOUND (Windows 10 20H2)" | published | supported, incomplete | True as far as it goes. It leaves out the capture conditions the pages show: TweakHound's list comes from a machine with VirtualBox Guest Additions (a VM guest), and wintips's from a PC with Office installed. Neither is a clean physical install (s2). |
| 4 | Table note | "Neither is a capture of this release, so the value names no build" | reasoning (unstated) | overstated (mild) | For wintips, "not 22H2" is an inference from the URL's `2021/09` upload path, and the note doesn't say so. The fix doc's "published 2021-09" has the same source (s2). |
| 5 | 5 `disagree` entries | Tables disagree, so nothing is applied; the text names both | published | supported | BITS, DeviceAssociationService, WlanSvc, wscsvc and wuauserv all verified on both pages. |
| 6 | W32Time stock note | Microsoft value 3/- kept | published (@W32TWG) | supported, incomplete | Wintips says Automatic (Delayed Start). The KB cites that table elsewhere but this note doesn't mention it (s1). |
| 7 | Plan `basis` for manifest-read values | `p.basis = L"owner-measured"`; the detail renders `BasisLabel` as "measured by Hadron's author" | owner-measured | **wrong** | The author didn't measure it. Hadron reads a file on whatever PC it runs on, at runtime (M1). |
| 8 | Manifest condition text | "Windows stock start type read from this PC's WinSxS component manifests, Windows build N.UBR (edition)" | owner-measured | overstated | What is read is the start type the component manifests and edition file *declare*. The doc says that calling it "stock" was validated by comparing against "known values" that "came from the probe session's knowledge, not a source" (fix doc §7). Say what was read (M1). |
| 9 | Runtime-list UI text | "Windows can set this start type at runtime from the hardware or software it finds, so no file or table value is reliable for this PC" | none | **wrong** (4 services), unsourced (8) | The source's own docs (sxsstock.h, design doc) say BITS, WinRM, dcsvc and dmwappushservice only have **DelayedAutostart** written at runtime. Yet the text says "this start type", and the golden prints it for dmwappushservice, whose stock is Manual, where DelayedAutostart doesn't matter. The fix doc says the list "came from the probe session's knowledge, not a source" (M2). |
| 10 | Setting-default text | "such defaults can be Server values" | reasoning (sxsstock.h: Themes default 4 vs Auto on a client, from the probe) | supported | It is a caution and leads to not applying. It could cite the Themes example, but this is optional. |
| 11 | Plan note (manifests read) | "Stock values come first from this PC's own WinSxS component manifests (build …), cross-checked with Hadron's KB. Where they disagree, both are shown and nothing is applied." | — | supported | True for every StockFor branch. The runtime-list and setting-default cases are covered by the separate "N more are shown" note. |
| 12 | Plan count note | "93 of 110 items have a Windows stock value from this PC's manifests or a measured or published basis …" | — | supported | Counts only Apply picks (withStock); already-set items are included, as before. |
| 13 | "N more are shown and not applied: their sources disagree, only a setting default gives them, or Windows sets them at runtime." | — | — | overstated (inherits #9) | Fixed by M2's wording. |
| 14 | Rows line | "6 ticked, 0 unticked, 4 kept or protected" | — | overstated (mild) | The 4 are shown-not-applied rows, not kept or protected items. Their "Planned" value (e.g. SysMain "Auto -> Manual") is a value that will not be written (s6). |
| 15 | Detail / plan wording per stockSrc | `microsoft`: "published; the page names no build"; `table`: "published; two agreeing third-party tables, no build named"; manifest: "this PC's component manifests, build N"; `disagree`: "not applied: the third-party tables disagree" | — | supported | I traced every StockFor branch. The say/says grammar is right in each one. `kbText` is set to "two agreeing third-party tables" for `disagree` too, but it is never printed when `hasK` is false, so no disagree item is ever described as "agreeing". One gap: when the manifest has only a setting default and the KB entry is `disagree`, the text reads "no table confirms it". That is true but vague. Optional. |
| 16 | Fixture goldens / UI selftest | `basis: owner-measured -- … read from this PC's WinSxS component manifests, Windows build 19045.5737 (Professional)` for hand-written fixture XML (e.g. "this PC's manifests say Manual" for SysMain) | — | overstated | Invented test values are labelled as read from this PC and measured by the author. CI uploads them as screenshots (s3). |
| 17 | Fix doc | "None: 101 … 17 services the tables don't list" and "Unmapped rows (ignored): … Microsoft (R) Diagnostics Hub Standard Collector Service …" | — | wrong | Both tables list it (Manual), and the KB has `diagnosticshub.standardcollector.service` (s4). |
| 18 | DECISIONS row 2, sxsstock.h | "showed the component manifests … hold each build's start types"; "probe round 2, which matched the known values" | owner (DECISIONS) | overstated | The "known values" came from the probe session's own knowledge (fix doc §7). The probe also found 12 services where the manifests didn't match (s5). |
| 19 | Fix doc perf / coverage | "18,883 manifests decompressed with no failure in about 18 s" (19045.2965 Pro) | owner-measured (probe) | supported | It is a dev-doc number with its build and edition. It isn't shown to users. |
| 20 | Counts (fix doc, README) | 183 sourced = 178 table + 5 microsoft; 5 disagree; 101 none (84 drv, 17 svc); 40 follow-ups; 70 must-fail cases | genkb | supported | Re-run and matched, except the "17 … don't list" wording (#17). |
| 21 | Hardware | — | — | supported | Nothing in the diff depends on an iGPU. The owner's edition is Pro, which matches wintips. No values are hardware-specific except the runtime-list services, which are never applied. |

**Verdict counts: supported 12, overstated 6, unsourced 0 as a row verdict, wrong 3 (21 rows).**
- Supported: 1, 2, 3, 5, 6, 10, 11, 12, 15, 19, 20, 21. Rows 3 and 6 are true but leave something out (s2, s1).
- Overstated: 4, 8, 13, 14, 16, 18.
- Wrong: 7, 9 (wrong for 4 services and unsourced for the other 8), 17.
- The 64-value spot-check is all supported.

## Must-fix (2)

**M1. Manifest-read values are labelled "measured by Hadron's author"** (rows 7, 8). `modules/services/plan.cpp`, in StockFor's manifest Apply path, sets `p.basis = L"owner-measured";`. The plan detail (`module.cpp:2211`) renders it through `BasisLabel`, which gives `"measured by Hadron's author"`. The golden prints `basis: owner-measured -- Windows stock start type read from this PC's WinSxS component manifests, …`.
- Problem: nobody measured it, the author least of all. Hadron reads Microsoft's component definitions on whatever PC it runs on. Under rule 7 it fits best in the owner-measured class (first-hand, on the target PC, with conditions). It is not the owner's measurement, though, and the label says it is. Separately, "Windows stock start type" asserts that the declared value *is* stock. The only check of that was against unsourced values.
- Replacement:
  - plan.cpp: give it its own token, `p.basis = L"this-pc";`.
  - kb.cpp `BasisLabel`: add `if (basis == L"this-pc") return L"read on this PC by Hadron (first-hand; rule 7 owner-measured class)";`. Keep `owner-measured` meaning the author's measurement.
  - Condition text: current `L"read from this PC's WinSxS component manifests, Windows build " + build + edition`. Replace the Apply note with:
    `L"The start type this PC's WinSxS component manifests install the service with (read by Hadron on this PC, Windows build " + build + L" " + edition + L", from " + SxsSrcText(...) + L"). Hadron uses it as the Windows stock value."`
  - Reason: current `L"Windows stock start type from this PC's component manifests (build N)"`. Replace with `L"Start type this PC's component manifests declare (build N)"`.
  - Regenerate `plan-services-stock.txt`, and make the selftest check (selftest.cpp) accept `this-pc` where it requires published or owner-measured.

**M2. Runtime-list text states an unsourced claim as fact, and gets 4 services wrong** (rows 9, 13). In `plan.cpp` StockFor, the current text is:
`L"Windows can set this start type at runtime from the hardware or software it finds, so no file or table value is reliable for this PC"`
- Problem: the fix doc says the list "came from the probe session's knowledge, not a source". For BITS, WinRM, dcsvc and dmwappushservice, Hadron's own docs say only DelayedAutostart is written at runtime, yet the text says "this start type" (golden: dmwappushservice, stock Manual).
- Replacement: split `SxsRuntimeDependent` into a start list and a delayed list, and word each one:
  - Start list (WlanSvc, stisvc, camsvc, IKEEXT, PolicyAgent, iphlpsvc, ssh-agent, WinHttpAutoProxySvc): `L"Hadron doesn't apply a stock value here: this service's start type may be changed after setup by the hardware or software found (reasoning, from a probe on one PC; not checked per service)"`.
  - Delayed list (BITS, WinRM, dcsvc, dmwappushservice): `L"Hadron doesn't apply a stock value here: Windows may write this service's DelayedAutostart after setup (reasoning, from a probe on one PC; not checked per service)"`. Alternatively, block a delayed-list service only when the target is Auto and apply Manual and Disabled values normally. That is a behaviour change, so it is the lead's call.
  - Plan note: current `L"… their sources disagree, only a setting default gives them, or Windows sets them at runtime."` Replace with `L"… their sources disagree, only a setting default gives them, or Hadron treats them as set after setup."`
  - Also fix the comment in `sxsstock.h`: "start types Windows sets at runtime from the hardware or software it finds, not from any file". Make it "services the probe flagged as changed after setup (probe-session judgement, not a source)".

## Should-fix (6)

**s1. W32Time stock note omits a contrary table** (row 6). Current (kb.py STOCK, W32Time): `'… The page names no build; Hadron uses it as the Windows 10 stock value.'`
Replacement: append ` @TBLWINTIPS (third-party) lists "Automatic (Delayed Start)" and @TBLTWEAKHOUND "Manual (Trigger Start)"; Hadron keeps the Microsoft page's value.`
If the lead prefers a general rule, apply the same append to any STOCK row a table contradicts. Of the five, only W32Time is contradicted.

**s2. Table labels and "no build" inference** (rows 3, 4).
- LABELS `TBLTWEAKHOUND`: current `'TweakHound\'s "Windows 10 Default Services v20H2" (third-party table)'`. Replace with `'TweakHound\'s "Windows 10 Default Services v20H2" (third-party table; build 19042.685, a default install with VirtualBox Guest Additions, so a VM)'`.
- LABELS `TBLWINTIPS`: current `'wintips.org\'s "Default Startup Type of Services on Windows 10 PRO OS" (third-party table)'`. Replace with `'wintips.org\'s "Default Startup Type of Services on Windows 10 PRO OS" (third-party table; no build stated, uploaded 2021-09 per its URL, Office installed)'`.
- `_TABLE_HEAD` / note: current `' Neither is a capture of this release, so the value names no build;'`. Replace with `' Neither table is a capture of build 19045 (TweakHound states 19042; wintips states no build, and its 2021-09 upload predates 19045), so no build is named;'`. Keep the literal "names no build" if genkb's check needs it.
- Fix doc §7 Sources table: "(published 2021-09)" becomes "(uploaded 2021-09, per the URL path)".

**s3. Fixture manifests shown as "this PC's" and author-measured** (row 16). The golden reads `basis: owner-measured -- Windows stock start type read from this PC's WinSxS component manifests, Windows build 19045.5737 (Professional) …`. The fixture's SysMain "Manual" is invented (the comment in `b-services.manifest` says hand-written). Add `bool fixture` to `SxsStock`, set it in `MakeFakeSxsReader`/`ScanSxs`, and when it is set word the condition `L"read from the test fixture's hand-written manifests (not a real PC), build " + build`. Update the goldens and the UI selftest string.

**s4. Diagnostics Hub row called unmapped** (rows 17, 20). Fix doc §7 currently reads `Unmapped rows (ignored): … Microsoft (R) Diagnostics Hub Standard Collector Service, …` and `17 services the tables don't list (per-user templates, OEM and third-party entries)`.
- Either map it (`'diagnosticshub.standardcollector.service': 'Manual'` in both tables, verified on both pages). That makes 179 table values and 16 none-services. Update the counts.
- Or reword to: `17 services with no table value (per-user templates, OEM and third-party entries, and diagnosticshub.standardcollector.service, whose table row was not mapped)`.

**s5. "Probe matched the known values"** (row 18).
- DECISIONS row: current `A read-only probe on the owner's PC showed the component manifests (plus the edition file and setting defaults) hold each build's start types`. Replace with `A read-only probe on the owner's PC (19045.2965 Pro) read every component manifest; their start types (with the edition file and setting defaults) matched what the probe session expected for most services, and 12 that didn't are never applied. The expected values were the session's own, not a source; the published check is the cross-check with the tables`.
- sxsstock.h: `(probe round 2, which matched the known values)` becomes `(probe round 2; checked against the probe session's expected values, not a source)`.

**s6. Shown rows counted as "kept or protected"** (row 14). Current `Rows: 6 ticked, 0 unticked, 4 kept or protected; already set: 87` (plan.cpp:972 and module.cpp:2774/3743). Count shown-not-applied rows separately (`4 not applied`), or reword to `kept, protected or not applied`. For a Show row, set `r.after = r.before` so "Planned" doesn't show a value that won't be written. The target can stay in the "Not applied:" text.

## OK (no change needed)
- 178 table values, 5 disagree entries and 5 Microsoft values are transcribed correctly (64 of 64 spot-checked on each page).
- The "two independent sources" claim holds: different extra rows (Office C2R vs VirtualBox), 6 start-type differences, different per-user suffixes, and no shared attribution.
- Every StockFor branch's say/says wording, and "agreeing" is never used for a `disagree` entry.
- The setting-default caution ("can be Server values") is backed by the probe's Themes example and leads only to not applying.
- The plan notes for manifests-read, manifests-failed and no-data are true in each case, and so is the Drivers-tab note ("Hadron has no Windows stock value for any item on this tab").
- Counts in the README and fix doc (apart from s4) and the genkb selftest (70 must-fail) match.
- No numbers without a label in the new user-facing text: build numbers come from the PC or the cited table, and the counts are computed.
- Hardware: no iGPU-dependent text, edition Pro matches wintips, and no preset changed.
