# Fix: svcdrv-presets (custom presets, measured before/after)

Follow-up ideas 4 and 5 of `docs/pipeline/svcdrv/7-finalize.md`. Branch `fix/svcdrv-presets`. Formats, flags and
the calls made are in `docs/design/services.md` (sections Formats, Spec ambiguities, Command line); this note is
the summary for review.

## What was built
- **Custom presets** (`modules/services/custom.*`, plan source in `plan.*`, page in `module.cpp`).
  - A preset or single plan has "Save ticks as custom preset…" (Console F7). It saves the included, unblocked rows'
    targets under a name to `presets\<stamp>.txt` in the data folder (`CREATE_NEW`, flushed, after `EnsureRoot`'s A1
    ancestor check).
  - The "Custom presets" page (Baseline header button, Console F12) lists them: sortable, scrollable and
    keyboard-navigable. It offers Open plan, Export…, Import… and Remove. Remove writes a tombstone; nothing is deleted.
  - Opening or importing a preset only builds a plan (`BuildCustom`). It goes through the existing guards,
    dependency check, confirm, snapshot, undo log and apply, unchanged. The undo log's source is
    `custom:<tab>:<name>`.
  - The guard is the built-in presets' guard: never-touch, never-preset, no KB entry (third-party), drivers not marked
    safe. Dropped items are named in one plan note. A stricter start for a hardware-bound item gets the presets'
    hardware check (kept while the hardware is present or seen, unticked for an unseen hot-plug class).
  - Plain-text format `HXPRESET 1` with a `hadron`/`build`/`tab`/`name` header and one `name<TAB>start-type` line
    per item. The parser is our own and strict; each refusal says which line failed.
  - Export: a Browse (Save as) dialog, then `CREATE_NEW`. An existing file is refused with a message and never replaced.
    Import: a Browse (Open) dialog. Cloud placeholders and offline files are refused unread. The dialogs run on the
    UI thread; the file work runs on a `Worker`.
- **Measure** (`modules/services/measure.*`, page in `module.cpp`).
  - It records RAM in use, total RAM, committed memory, the commit limit, the process count and the `svchost.exe`
    count. With them it records the time (UTC), boot time, minutes after boot, build, machine GUID and the plan's
    title. Each record goes to `measure\measure-<stamp>.txt` (`CREATE_NEW`).
  - "Measure before" is on the plan view (Console F8). After a restart, a banner offers "Measure after"; the Measure
    page (Console Ctrl+M) offers both.
  - The compare table is titled `owner-measured`. It shows Before, After, and After − before, plus each side's
    conditions.
  - Warnings appear when minutes after boot, build or usable physical memory differ.
  - "Between them" names the Hadron applies recorded between the before and the after's boot, from the undo logs,
    or says that none was, so the difference is not from a Hadron change.
  - "Test it: a fair comparison" gives the guidance: same apps, same minutes after boot, repeat the pair, test frame
    rates in the game. It contains no numbers.
  - The page says Hadron never predicts savings. The only numbers shown are recorded values.

## Tests
- `--selftest`: 346 checks, 0 failed (282 on main). New checks:
  - export → file → import → export byte-identical;
  - an export onto an existing file is refused;
  - 21 import refusals plus the 4096-item cap, each with its line;
  - BOM/CRLF accepted;
  - guard drops (WinDefend, RpcSs, Fax; Beep and nvlddmkm on drivers);
  - the custom golden;
  - a custom apply on the fake backend, then Undo;
  - store save, duplicate refusal and remove;
  - the measurement record byte round trip, the missing-value refusal, pairing and a live `TakeMeasurement`;
  - review fixes: the import attribute check (folder, link, each placeholder attribute), a folder and an oversized file
    refused through the handle, the hardware guard (bthserv kept, FrameServer unticked, no hardware information kept),
    `AppliesBetween`, a queued rebuild outside the plan view, "no plan opened" reasons, the "Between them" text;
  - UI flows: the save form, the import refusal and acceptance, Console F12/F2/Esc, and the 800x560 click-target
    layout of both pages in both layouts.
- `tests/tools/services_check.sh`: passes (KB, fixture, both selftests, 10 + 1 golden plans, screenshots).
- Release (no test hooks) and the test-hooks build: `HadronServices.exe` and `Hadron.exe` build with MinGW and no
  new warnings.

## Goldens
- New: `tests/golden/services/plan-services-custom.txt`, the fixture's "Gaming rig" custom plan (13 items, with
  bthserv and FrameServer for the hardware guard: bthserv kept, FrameServer and Spooler unticked as hot-plug classes).
  Every existing golden is byte-unchanged.

## Screenshots
`services-{presets,import-error,plan-custom,save-preset,measure}-{baseline,console}.png`, dark theme, rendered by
`services_check.sh` and copied to `/mnt/project-files/pipeline/fixes/svcdrv-presets/`.

## Open items
- `CMakeLists.txt` (root) changed: it adds `custom.cpp` and `measure.cpp`, the `HX_HADRON_VERSION` define for the
  preset header, and `comdlg32` for the Open/Save dialogs. No `core/` change was needed.
- The `OFFLINE`/recall-attribute import refusal on a real file is not exercised under Wine: the file system there
  doesn't keep the attribute. Windows CI runs it; the attribute check itself is tested on every attribute.
- An import now refuses a link (reparse point) to a preset file too; the user picks the target instead.
- `Fax` and third-party items are dropped from custom presets because they have no KB entry, as for built-in presets.
  If the KB gains an entry for them (fix/svcdrv-kb), the drop note changes and `plan-services-custom.txt` may need
  regenerating.
- Undo of a custom apply writes an absent `DelayedAutostart` back as 0, as every Undo does (existing design).
- In fixture mode, the measure buttons and the "measure after" banner are off or hidden; they work live only.
- The preset and measure fixtures are hand-written and not generated by `make_services_fixture.py`.
- Wording to check in Design review: the plan hint and page intro texts. They have not been through the
  Design/Review stages.
