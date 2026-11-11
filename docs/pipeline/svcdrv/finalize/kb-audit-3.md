# svcdrv kb-audit 3: genkb digit allow-list (1b6e074) and S rewordings (9817ebb)

Diff `fe827bc..9817ebb -- tools/services modules/services/data`, branch `module/services-svcdrv` (HEAD 94cafef). Read-only except this file.
**Ran:** `--selftest` PASS, 0 failed (50 must-fail, 15 ok, 1 warn, 265 entries); regeneration byte-identical to `services.db`, with presets unchanged (bloat 17, stripped 48, minimum 11); 35 probe texts through `render()` at 9817ebb and fe827bc (scratchpad `kb3/`). Audit-2 M1 (stale goldens) is closed: e3cfd92 regenerated them (`plan-services-stripped.txt:234` carries the S15 text).
**Pages fetched live:** @STORPORT, @STORAHCI, @BASICDISPLAY, @USBAUDIO2, @USBCLASSES, and the @IOT rows for KtmRm, TermService, iphlpsvc, Appinfo and WpnUserService. WS16 rows come from `research/ws2016-services.tsv` (iphlpsvc: "None").

| # | Entry | Claim | Basis | Verdict | Note / rewording |
|---|---|---|---|---|---|
| 1 | storport (S1) | "Microsoft's AHCI driver (StorAHCI) is one, and Microsoft encourages vendors to use it" | published @STORPORT | unsourced (cite) | "encouraged" is on @STORPORT. "StorAHCI, a Storport miniport" is on @STORAHCI only, and the note doesn't cite it: s1 |
| 2 | USBXHCI (S2) | USB 2 ports on boards with no separate USB 2 controller | reasoning, inline | supported | Applied as asked |
| 3 | BasicDisplay (S3) | Used from removing the old driver to installing the new one | published | supported | The live page says exactly this |
| 4 | hvservice (S4) | Support driver; "never changes it while the hypervisor is running" | reasoning | supported | `plan.cpp:183-185` CheckGuard returns never when `hw->hypervisor` |
| 5 | HDAudBus (S5) | "Some Intel boards run onboard audio on Intel's own bus driver (Smart Sound Technology) … then it does not depend on this one" | reasoning (entry) | unsourced | Vendor fact with no label. It goes past audit-2's wording, which named no SST: s2 |
| 6 | 29 drivers (S6) | "Hadron cites no Microsoft page for it" | reasoning | supported | Applied as asked |
| 7 | iphlpsvc (S7) | Xbox party chat "reported to need Teredo (not stated on either page)"; test via Xbox Networking | none named | supported (label) | Neither row mentions Xbox. The Xbox support page needs JavaScript, so it couldn't be fetched. The basis kind is folklore, so name it: s3 |
| 8 | TPM (S8) | BitLocker TPM protector and Hello keys fail; anti-cheats "(not sourced)" | reasoning / none | supported (label) | s3 |
| 9 | Appinfo (S9) | A normal-desktop start goes through Appinfo's UAC prompt | reasoning | supported | Fits `requireAdministrator` (rule 4) |
| 10 | TermService (S10) | Effect, then @IOT: clear the Remote tab checkboxes "rather than disable it" | reasoning + published | supported | The live row gives this advice and rates it Don't disable |
| 11 | WpnUserService (S11) | @IOT rating is for IoT Enterprise | published | supported | The live row is DND, which matches the KB |
| 12 | KtmRm (S13) | Any MSDTC transaction involving a KRM fails | published | supported | Matches the live row word for word |
| 13 | usbaudio, USBSTOR (S14) | "USB Audio 1.0 driver"; "USB disks that don't use UAS" | published | supported | @USBAUDIO2 quote checked. "Bulk-Only" is gone; UAS comes from the Uaspstor row |
| 14 | XboxGipSvc (S15) | Driver name unsourced; a Bluetooth controller doesn't keep the row | reasoning, inline | supported | Applied as asked |
| 15 | SysMain test | "Restart three times each way" | (instruction) | supported | Words, not a count; matches README |
| 16 | gate (S12) | Fails all 12 audit-2 bypass texts | selftest | supported | — |
| 17 | gate, README, docstring | "rejects counts … in words (`forty processes`)" | selftest | **wrong** (regression) | Passes now, failed at fe827bc: "Removes dozens of processes.", "Removes hundreds of services.", "Thousands of handles are freed.", "Twenty or so services stop." `WORD_COUNT_RE` dropped the old `of`/`or so` slot: M1 |
| 18 | gate DIGIT_ALLOW | Release numbers are identifiers | — | **wrong** (regression) | `\b(?:1607\|1703\|1903\|2004)\b` is not anchored, so "Saves 1903 handles.", "1607 fewer processes." and "Removes 2004 services on Windows 10." pass (all failed at fe827bc): M1 |
| 19 | gate residuals | (not claimed by README) | probe | supported | Pass both before and after: "Boots x2 faster.", "fps20", "½"/"⅓", "a fifth", "one tenth", "a millisecond", "a minute": s4 |
| 20 | README | 50 must-fail, 15 ok, 1 warning | selftest | supported | — |
| 21 | services.db | Regenerates byte-identical; no preset change | genkb | supported | — |

**Verdict counts: supported 17, overstated 0, unsourced 2, wrong 2 (21 rows).** S1–S11 and S13–S15 are all applied; S12 is replaced by the allow-list.
**Hardware:** no preset changed, so nothing new is disabled on the owner's PC. For HDAudBus on the PRO Z790-P (ALC897, HD Audio), whether SST is in use is unknown; the text now says how to check. The TPM anti-cheat clause stays moot on Windows 10.

## Must-fix
**M1. Gate regressions from 1b6e074.** In `genkb.py` (tested in scratchpad: selftest passes, KB byte-identical, the 7 texts fail, "Present on Server 2016 (the 1607 release)." and "Windows 10 (1903 and up)" pass):
- `WORD_COUNT_RE`: replace `r'(?:\s+of\s+the)?'` with `r'(?:\s+or\s+so)?(?:\s+of(?:\s+the)?)?'`.
- `DIGIT_ALLOW`: replace `\b(?:1607|1703|1903|2004)\b` with
  `\b(?:1607|1703|1903|2004)\b(?![\s-]+(?:of\s+(?:the\s+)?)?(?:(?:more|fewer|less|extra|additional|background|running|Windows|Microsoft)[\s-]+)*(?:processe?s?|services?|drivers?|threads?|handles?|instances?|tasks?|svchosts?|apps?|programs?)\b)`.
- Add the 7 texts from rows 17–18 as must-fail cases, add the two pass texts as ok texts, and update the README counts (57 must-fail, 17 ok).

## Should-fix
- **s1. storport basis_note:** append " @STORAHCI calls StorAHCI \"a Storport miniport\"; @STORPORT: \"All vendors are encouraged to use Storport where possible\"."
- **s2. HDAudBus if_off:** "Audio devices on that bus stop (reasoning). Some Intel boards run onboard audio on Intel's Smart Sound Technology bus driver instead, and then it does not depend on this one (reasoning, not sourced). The audio controller's Driver details in Device Manager show which driver loads."
- **s3. Name the basis kind.** iphlpsvc: "… reported to need Teredo (folklore: not stated on either page). Test it: …". TPM: "BitLocker with a TPM protector and TPM-backed Windows Hello keys fail (reasoning). Some anti-cheats are reported to check the TPM (folklore)."
- **s4. Gate residuals (not regressions).**
  - `IDENT_RE`: add `(?![xX]\d)` after `\b`, so multipliers like `x2` are not masked. `x86`/`x64` would then need DIGIT_ALLOW; the KB uses neither.
  - `FRACTION_RE`: add `[¼-¾⅐-⅞]|(?:a|one)[\s-]+(?:fifth|tenth)s?`.
  - `WORD_UNIT_RE`: add `an?\s+(?:milli|micro|nano)second`. Leave plain "a second" alone, because it is usually the ordinal.
