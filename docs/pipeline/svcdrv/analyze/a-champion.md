# svcdrv Analyze: program champion (`champion`)

Branch `module/services-svcdrv` @ 34ee85c, prebuilt MinGW Release in /home/user/Hadron/build. Everything below ran under
Wine 9 + Xvfb with the fixture only. I drove the real GUI with SendInput from a small driver exe I wrote
(scratch `champion/drv/driver.cpp`), not only the `--screenshot` path. No tracked file was changed.

## Findings table

| # | Rating | Area | Finding |
|---|---|---|---|
| E1 | minor (top owner-expectation gap) | F4 Default | "Default" can't take the owner back to Windows defaults: "Windows stock" is empty (0 of 109, "Nothing to change"), and "first snapshot" records the PC as it is the first time the page runs, which on his already-tuned PC is his tuned state |
| E2 | minor (expectation) | F8 explanations | The KB has 88 services and 11 drivers. Core stock services (RpcSs, DcomLaunch, Audiosrv, ...) and almost every driver show "Not in Hadron's KB". Services fall back to Windows' own registered description; drivers get no explanation at all |
| E3 | minor (expectation) | F4 presets | Presets only touch KB items. OEM and consumer extras (third-party) are never in Pure bloat, contrary to the idea text. Minimum leaves every stock service that has no KB entry alone. Drivers: Pure bloat is always empty; Stripped and Minimum touch 7 KB drivers at most |
| E4 | minor (expectation) | F6 TI | The TI toggle says "Not Defender, PPL or Tamper Protection", and the Defender stack is never in a preset. The idea named WinDefend as a TI example. Honest and correct, but likely not what he pictures when he asks for a "run as trustedinstaller toggle" |
| E5 | minor | F1 columns | There is no Image path column (detail pane only). Type, Account and Company are dropped below roughly 1700 px of list width: invisible at the default 1200x800 and in the suite. The "Hadron" column shows the lowest level, not the target start type, and ignores hardware evidence (BluetoothUserService shows "stripped" while the detail says "kept by every preset") |
| E6 | minor (deferred, stated in 3-build.md) | F5, F9, F1 | Not built: custom presets with export/import, measured before/after RAM and process count, the Authenticode signer column, and the Event 7040 account in drift |
| U1 | minor (bug, reproduced) | tabs | Switching tabs leaves a stale highlighted row while the detail pane says "Select an item". Selecting row 5 on Services, then Drivers, highlights `disk`. module.cpp:834 clears selName_, but Refilter (module.cpp:651) only selects when `list_.Selected() < 0`. Fix: on tab switch call `list_.Select(V_.empty() ? -1 : 0)` and set selName_, or drop the `< 0` condition when selName_ is empty |
| U2 | minor (core, all modules) | focus rect | The list's focus rect is invisible. core/ui/widgets.cpp:208-211 draws `FocusRectC(..., p.accent())` on a selected row whose fill is the accent. Zoomed shot z11 shows no outline. Fix: draw it in selText/onAccent on the selected row. The search box and detail pane focus indicators do show |
| U3 | minor | keyboard | Tab cycles only list → filter → detail. Chips and buttons are mouse-only: the Services/Drivers tabs, presets, the TI toggle, start type, Start/Stop/Restart, Undo/Resume, Apply. CLAUDE.md only requires lists to be keyboard-navigable, so this is not a rule break. Keeping Enter from applying is deliberate (module.cpp:974) |
| U4 | minor | sort | After a header-click sort the selected row can end up off-screen. Sort by Display name with AudioEndpointBuilder selected: the view shows S..U, and the selected row is near the end |
| U5 | minor | sort | On narrow columns the sort arrow overlaps the header text ("vs first", "Hadron") |
| U6 | minor | sort | The "Hadron" column sorts alphabetically (bloat, minimum, no preset, protected, stripped), not by level |
| U7 | minor (code read) | prefs | The persisted sort is an index into the visible columns (module.cpp:362/416/610), and the visible set changes with window width, so the restored sort can land on another column or fall back to Name |
| U8 | needs Windows | high contrast | Not testable under Wine: SPI_SETHIGHCONTRAST fails with error 1439 and the registry flag is ignored. The code path exists (module.cpp:78 Pal, widgets.cpp:17/32). The host does not handle WM_SETTINGCHANGE, so switching HC on while Hadron runs is not picked up (core issue) |
| U9 | minor (suite-wide) | rule | Nothing in the UI or docs says "no screen-reader support in v1"; only CLAUDE.md mentions it |
| U10 | minor | text | The "Stop TrustedInstaller" plan's Why column just says "stop" |

**Counts: serious 0, minor 16** (U8 is a Needs-Windows item). Nothing I found crashes, writes wrong config or breaks a
hard rule. E1–E4 are the items the owner is most likely to read as "this isn't what I asked for".

## Feature by feature (1-idea.md)

| Feature | Status | Evidence |
|---|---|---|
| 1 Two lists + columns | **partly** | Tabs work (shots 27, services-drivers.png). Name, display, state, start type (+trigger, Boot/System), vs first, Hadron and basis are always shown. Type, account/load group and company are width-dependent. No image-path column. Signer deferred. Sorting works on every column, checked by clicking each one (m1/m2). The filter matches name, display, company, group and account ("blue" → 3 rows, shot 20). Wheel and thumb drag work (09, 70). Up/Down/PgDn work (11) |
| 2 Detail pane | **built** (stock part empty) | KB text with basis, "If it's off", "How to test", presets with pbasis, hardware evidence, depends on / needed by, failure actions (module.cpp:1463), the protection reason, and the registered description labelled "not Hadron". "Windows stock" always reads "no value ... yet" |
| 3 Live control | **built** | Start/Stop/Restart for services only. Drivers get start type only and no live buttons (shot 29). Disabled in fixture mode, so I could not drive it in the GUI |
| 4 Presets, 4 levels × 2 tabs | **partly** | All five chips open in-view plans with per-row [x] toggles (Space or click; unticking checked in shot 24), dependency warnings and evidence. Windows stock is empty (shot 21); see E1–E3 |
| 5 Custom presets / overrides / export | **partly / deferred** | Per-item overrides exist as plan-row toggles plus single-item start type. There are no saved custom presets and no export/import |
| 6 TI toggle | **built** (Windows-untested) | Off at launch, never persisted (module.cpp:237; status line says "never remembered"). Turning it on rebuilds an open plan. The plan notes change between toggle off and on (services-ti-on.png). Stop-TI plan from the banner (shot 43) |
| 7 Safety | **built** (per selftest) | The plan changes nothing until Apply. Interrupted-apply banner with Undo and Resume, both opening correct plans (shots 40–42), plus the "Hadron started TrustedInstaller ... Stop it" banner. The flushed CRC undo log, recovery zip and lock are covered by the selftest (137 checks pass standalone and via `Hadron.exe --solo services`) |
| 8 Explanations, stock only, with basis | **partly** | Every KB entry has a basis, shown in the UI. Coverage is thin (E2) |
| 9 Measured before/after | **deferred** | Not built |
| Risks: Boot-start never touched, curated System list, drift | **built** | Boot-start drivers are "protected" (ACPI, disk ...). The `sysok` gate and the never-touch groups are at plan.cpp:12-60. The "vs first" column plus a `*` on start type show drift |

## Built both ways
- `HadronServices.exe` GUI and selftest: ok.
- `Hadron.exe --solo services`: ok. It shows no sidebar (shot 80-solo.png).
- `Hadron.exe --module services` and a sidebar click from Hello → Services → BIOS → Services: ok, state kept (shots 50–52).
- In the suite at 1300 px the Basis column is dropped by design, with no horizontal scroll.

## Other checks
- Error text: every SCM/registry failure in backend_live.cpp is turned into text with `sys::ErrorText`. Load and store errors go to notices/banners (module.cpp:472-492). No MessageBox anywhere, so no dialog can open from a worker.
- Drop targets: none exist, so a Browse fallback is not needed (import was not built).
- Ctrl+F and type-to-filter go to the search box. Esc clears the filter or closes the plan.

## Needs Windows
1. High contrast on, then launch HadronServices.exe: check the list selection, chips, red/amber "Protected" text and the plan [x] column are legible (Pal at module.cpp:78).
2. At the owner's real window width (maximised 1440p/4K), confirm the Type, Account and Company columns appear, and that the ~250 services and ~400 drivers scroll smoothly.
3. With TI on, change WaaSMedicSvc → Manual through a single-item plan on a throwaway VM, not the owner's PC.

## Suggested direction for E1 (needs owner/proxy)
Build the 19045 stock table from an `owner-measured`-style capture: an SCM dump of a fresh Win10 22H2 install in a VM,
with conditions recorded (build, edition, no drivers). That is a measurement, not folklore, and it fills "Windows stock"
honestly. Alternatively, rename the chip "Default: as first seen by Hadron" and say in the UI that it is not Windows'
default.

## Screenshots I took (Wine, fixture)
All under <scratch>/champion/shots/ :
m1.png, m2.png (sort on each column), m3.png (wheel, drag, keys), m4.png (Tab focus cycle), z11.png (missing focus rect zoom),
m5.png (filter, Windows stock empty, first snapshot, bloat), m6.png (untick, TI on/off, stale Drivers selection),
m7.png (drivers minimum, single-item buttons), m8.png (interrupted banner Undo/Resume/Stop TI), m9.png (suite sidebar),
m10.png (HC attempt: Wine ignores it), m11.png (scrollbar drag, plan sort), 80-solo.png, 80-solo-small.png.
