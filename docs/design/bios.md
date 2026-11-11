# bios port — build spec

Lead-owned spec for porting `legacy/bios-explorer` (+ `legacy/uefi-dump`) to the `bios` module. Legacy
reference: **`docs/legacy/bios-explorer.md`** (function map §1, `settings.db` format §2, snapshot format §3,
parity checklist §4, theme §5, edge cases §6, rule violations §7). Read that, not the legacy source.

## Goal
`bios` module ("BIOS Settings"): every setup setting of the owner's board (5,368 from the shipped
`settings.db`) with its live value, default, menu path, help, explanation and per-goal advice; snapshots
(save / load / compare to live); export of the changes a goal recommends. **Read-only firmware (rule 2):**
the module never writes NVRAM, never flashes, never runs SCEWIN/AMISCE.
**Since 2026-11-11:** guarded setting writes (rule 2 rewritten) are specified in `docs/design/bios-write.md`.

## Two work packages (run in parallel)
**A — module** (`modules/bios/*`): UI, live firmware read, version guard, snapshots, export.
**B — knowledge base** (`tools/bios/*`, `modules/bios/data/settings.db`): port the KB with basis fields and a
generator that enforces rule 7. A and B meet only at the **settings.db v2 format** below.

## settings.db v2 (the contract between A and B)
Same encoding and escaping as v1 (legacy §2): UTF-8 no BOM, LF, TAB-separated, `esc`/`Unesc` rules.
- `V` title (as v1).
- **`B` board-guard**: `B`, smbios board product (e.g. `MS-7E06`), BIOS version (e.g. `7E06vAF`). New.
- `T` tier-1 count (as v1).
- **`K`**: v1's 14 fields, then **15 `basis`** (`owner-measured` | `published` | `reasoning` | `folklore`),
  **16 `basis_note`** (conditions for owner-measured, URL(s) for published, one line otherwise),
  **17 `sources`**: one entry per number in the entry's text, `number 0x1F kind 0x1F ref`, joined by `0x1E`
  (kind is `owner-measured` or `published`; ref is conditions or URL). Empty when the text has no numbers.
- `S`: v1's 17 fields unchanged (data from the BIOS image; the image isn't in the repo, so these lines are
  carried over verbatim from `legacy/bios-explorer/app/settings.db`, re-matched to the new KB).

The module must read v1 too (fields 15–17 absent → basis shown as "not stated").

## B — knowledge base and generator
- `tools/bios/kb.py`: port of `kb.py` + `kb_impact.py` + `kb2.py` with overlays already applied, one record
  per entry: regex, title, explanation, recs (5 goals), note, impact×4, `basis`, `basis_note`. Numbers in
  text are marked `[[text|kind|ref]]`; the generator renders the plain text and emits field 17.
- `tools/bios/genkb.py <legacy settings.db> <out settings.db>` (Python 3, stdlib only, a build tool — rule 3
  allows it): reads `V` and the `S` lines from the legacy file, writes `V`, `B` (`MS-7E06`, `7E06vAF`), the
  tier-1 `K` lines, `T`, tier-2 `K` lines, then the `S` lines with field 16 re-matched in gen.py's order
  (tier-1 regex on prompt → `NAME` on stripped prompt → `MENU` on joined paths → `FALLBACK`).
  **It fails** (non-zero exit, names the entry) if: a tier-1 entry has no `basis`; any number in rendered
  text is not inside `[[…]]` with kind `owner-measured` or `published` (number heuristic: legacy §7 regex —
  %, ms/µs/ns, W/V/A/mV, MHz/GHz, KB/MB/GB, °C, `x`, fps, cycles; "1% lows"/"0.1% lows" are metric names);
  a `published` ref is not an `https://` URL.
- Rule-7 content fixes (legacy §7 #27–#30): remove owner-specific text ("Your 14700KF…", "a fixed 53x")
  or phrase it generally ("on CPUs without integrated graphics (Intel F models)…"); drop gen.py's broken
  `Your PC:` regex; the 9900K prefetcher result becomes `owner-measured` ("owner's 9900K, Overwatch, 1% lows,
  about +20% — a different CPU; test it on yours"); settle the microcode-version inconsistency with a
  published source or remove the version number.
- A number with no source you actually read is **rewritten without the number** (qualitative, plus "test it"
  and how), never given an invented URL. Workload-dependent effects say "test it".
- Every KB diff goes to `kb-auditor` before merge.

## A — module
**Files:** `modules/bios/{module.cpp,standalone.cpp,firmware.{h,cpp},snapshot.{h,cpp},db.{h,cpp}}`,
`modules/bios/resources/bios.rc` (RCDATA `BIOS_SETTINGS` = `modules/bios/data/settings.db`), registry
factory line, CMake, MODULES.md. Until B lands, embed a v2 file made by converting the legacy one with
empty basis fields (a throwaway script in the builder's branch is fine; B's output replaces it).

**Live read (`firmware.cpp`, on a `Worker`):** `TokenElevation` + `GetFirmwareType` (not UEFI → snapshot mode
with a banner). Enable `SeSystemEnvironmentPrivilege` on the **worker thread only** (diskmap's
`ThreadBackupPrivilege` pattern; counts only if `GLE != ERROR_NOT_ALL_ASSIGNED`). Read each of the 20
(var, guid) pairs with `GetFirmwareEnvironmentVariableExW`, buffer grows on 122. Per-variable state shown:
203 not found (map mismatch), 1314 privilege missing, 5 protected. Names match **exactly**; GUIDs
case-insensitively. Decode values per `S` offset/width; `ZeroIsAuto` and `AddContext` as legacy §4.12–4.13.

**Version guard:** `GetSystemFirmwareTable('RSMB')`, parse SMBIOS type 0 (BIOS version) and type 2 (board
product). If they don't match the `B` line → banner "This map was built for MS-7E06 / 7E06vAF; your board
reports …", and every value is marked **unverified**.

**Snapshots (replace `Dump-UEFI.ps1`/`Run-Dump.bat`):**
- *Save snapshot*: enumerate all variables (`NtEnumerateSystemEnvironmentValuesEx(1)` via `GetProcAddress`,
  read-only) on a Worker; folder picked with `IFileDialog` (`FOS_PICKFOLDERS`) owned by `IHost::Frame()`;
  writes `UEFI_Dump_<stamp>\` with `index.csv` (legacy columns), `vars\<NNNN>_<sanitised>__<guid>.bin`
  (index prefix — no collisions), `system.txt`. Never overwrite: if the folder exists, add `-2`, `-3`.
- *Load snapshot*: a folder (pick `index.csv`) **or a `.zip`** (own zip reader + inflate decoder, rule 3).
  RFC 4180 CSV parser (`""`, quoted newlines). Accepts legacy dumps (no index prefix). Reports how many
  rows were skipped and why.
- *Compare*: snapshot vs live (or vs default): a "Changed" column/filter.

**Export changes:** Save dialog (`GetSaveFileNameW`, `OFN_OVERWRITEPROMPT`), default name
`BIOS changes - <Goal>.txt`; content as legacy §4.11. No write next to the exe.

**UI (core/ui, both layouts, all themes):** goal selector, filters (legacy §4.3 labels), search, sortable
`ListView` (legacy §4.6 sort keys), detail pane (§4.8) **showing the basis** of every explanation/advice/effect
and the sources of its numbers, tier-1 dot, banners (not elevated / not UEFI / version mismatch / snapshot
mode). No modal dialog from `Create`; empty state with "Load snapshot…" when nothing can be read.
Baseline = list + detail pane (legacy look); Console = dense list + compact detail.

**Headless:** `--selftest`: settings.db v1+v2 parse (counts: 5,368 S; K = T + tier-2), value decode on
synthetic var bytes (widths 1/2/4/8, offsets), ZeroIsAuto, CSV edge cases, zip+inflate round-trip on a
stored and a deflated sample (generate the deflated sample with Python's zlib at build/test time or embed a
tiny one), SMBIOS parse on a synthetic table, sanitise+index naming. `--snapshot <dir|zip>` loads a snapshot
for screenshots. `--screenshot` renders deterministically from the fixture
`tests/fixtures/` BIOS snapshot (see `docs/legacy`).

## Rule-forced deviations (owner OK as one batch)
1. Version guard + "unverified" values on a board/BIOS mismatch (#6).
2. Snapshots move into the module; the `.bat`/`.ps1` are retired; snapshot file names get an index
   prefix; never overwrite (#7, #23, #24).
3. Export via a Save dialog instead of next to the exe; asks before overwriting (#4).
4. No startup message box: inline banners/empty state (#9, #12).
5. Variable names matched case-sensitively (#21).
6. KB: every entry shows its basis; unsourced numbers rewritten without the number; owner-specific text
   made general; the 9900K result labelled as the owner's measurement on a different CPU (#27–#30).
7. Platform/contract fixes with no visible behaviour change (#1, #2, #8, #10, #11, #13–#20, #22).

Settled during the build (lead, 2026-11-11, under items 1, 4 and 6 above):
- Export lists the basis of every note/advice line and the source of each number printed (rule 7).
- Values are marked unverified when the board/BIOS check can't run (no SMBIOS, snapshot without
  `system.txt`), and a snapshot loaded for compare is checked against its own `system.txt`.
- MSI SMBIOS reports the BIOS version as `A.F0`; the guard accepts it as `7E06vAF` (basis: the legacy
  title `BIOS 7E06vAF (A.F0)`). Still to confirm on the owner's PC.
- The detail pane shows "Claim check" even when an entry has no effect text (legacy hid it).
- Snapshot loads are capped (4 MB per variable, 16 MB per text file, 256 MB in total) against hostile files.

## Acceptance
Builds standalone (`HadronBios.exe`) and in the suite; `--selftest` passes under Wine; screenshots in both
layouts × dark/light plus a non-solo suite shot; `genkb.py` passes and its output is what's embedded;
kb-auditor passes; reviewer passes (no open category 1–4); real firmware reads verified on the owner's PC.
