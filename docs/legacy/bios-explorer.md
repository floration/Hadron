# Legacy notes: BIOS Settings Explorer (+ UEFI dump)

Reference for porting `legacy/bios-explorer` and `legacy/uefi-dump` to the `bios` module. Written from the
source at commit `b163dc0`. Line numbers refer to these files as committed:

| File | Lines |
|---|---|
| `legacy/bios-explorer/app/bsx.cpp` | 604 |
| `legacy/bios-explorer/app/app.rc` | 3 |
| `legacy/bios-explorer/app/app.manifest` | 9 |
| `legacy/bios-explorer/app/README.txt` | 20 |
| `legacy/bios-explorer/app/settings.db` | 5,583 lines, 1,594,534 bytes (generated) |
| `legacy/bios-explorer/pipeline/unpack.py` / `ifr.py` / `builddb.py` / `gen.py` | 120 / 178 / 110 / 57 |
| `legacy/bios-explorer/pipeline/kb.py` / `kb_impact.py` / `kb2.py` | 155 / 219 / 463 |
| `legacy/uefi-dump/Dump-UEFI.ps1` / `Run-Dump.bat` | 158 / 9 |

"Unclear" marks things that can't be settled from the source alone.

---

## 1. Function map

### 1.1 `bsx.cpp`

Globals and data types: lines 21-44 (data, view state) and 298-306 (fonts, palette, hit list, layout).
`Setting` (24-29) holds one row of the map plus its live value (`hasCur`, `cur`) and its context label (`ctx`).
`Kb` (23) holds one knowledge-base entry: `title`, `expl`, `rec[5]`, `note`, `fx[3]`, `claim`.

| Lines | Function | What it does |
|---|---|---|
| 46-50 | `W` | UTF-8 `std::string` to `wstring` (`MultiByteToWideChar`, unchecked). |
| 51-55 | `U8` | `wstring` to UTF-8 (`WideCharToMultiByte`, unchecked). |
| 56-63 | `Unesc` | Undoes `gen.py` escaping: `\n` becomes a newline, `\x` becomes `x` for any other `x`, and a trailing lone `\` is kept. |
| 64-68 | `Split` | Splits on one delimiter character and keeps empty fields. |
| 69 | `ToLL` | `_wtoi64`; an empty string gives 0. |
| 70 | `Lower` | `towlower` on each character. |
| 72-79 | `AddContext` | Labels repeated prompt names with their menu (see 4.13). |
| 80-111 | `LoadDb` | Parses the `SETTINGS` RCDATA resource (`settings.db`) into `S`, `K`, `gTitle` and `gKeyCount`. |
| 114 | `PGetFwEx` | Typedef for `GetFirmwareEnvironmentVariableExW`. |
| 115-121 | `EnablePriv` | Enables `SeSystemEnvironmentPrivilege`. Returns `GetLastError() == ERROR_SUCCESS`; the caller ignores the result. |
| 122-132 | `ApplyVars` | Decodes each setting from a map keyed `lower(name)\|lower(guid)`. Little-endian, `width` bytes at `offset`. Sets `hasCur = false` when the variable is missing or too short. |
| 133-149 | `ReadLive` | Reads each distinct (var, guid) in the map with a fixed 64 KB buffer. Returns how many variables came back non-empty. If that is at least one, it applies them and sets `gSource = "Live values read from this PC"`. |
| 150-170 | `ReadDump` | Reads `<dir>\index.csv` and `<dir>\vars\<File>`, then applies the values. `gSource = "Saved dump: " + dir`. Returns false if there is no index or no variables could be loaded. |
| 173-181 | `ZeroIsAuto` | Decides whether value 0 of an option-less setting displays as "Auto" (see 4.12). The result is cached per id. |
| 182-188 | `ValText` | Display text for a value: the matching option's text, otherwise "Auto" (`ZeroIsAuto`), otherwise decimal. Both `swprintf` branches at 186 are identical, and the IFR hex flag is never used. |
| 189 | `CurText` | `ValText(cur)`, or `-` when there is no current value. |
| 190 | `DefText` | `ValText(def)`, or `-` when there is no default. |
| 191 | `Changed` | `hasCur && hasDef && cur != def`. |
| 193-200 | `RecVal` | An "exact" recommendation: the KB rec for goal `g` is non-empty, doesn't start with `~`, and is a case-insensitive **prefix** of some option's text. The first option that matches gives the value. |
| 201-207 | `RecText` | The recommendation text with any leading `~` stripped. When `RecVal` finds an exact option, it returns that option's display text instead. |
| 208 | `Differs` | `hasCur` and an exact rec for `gGoal` that differs from `cur`. |
| 210 | `IsKey` | `0 <= kb < gKeyCount`, i.e. a tier-1 KB entry. |
| 211-215 | `GoalRank` | Sort rank for the Goal column (see 4.6). |
| 216-236 | `SortV` | `stable_sort` of the visible list by the clicked column (see 4.6). |
| 237-263 | `Refilter` | Rebuilds `V` from filter + search, applies the default ordering and then `SortV`, fixes up the selection, and recentres `gTop`. |
| 266-295 | `Export` | Writes `BIOS changes - <Goal>.txt` next to the exe and opens it (see 4.11). |
| 308 | `Fill` | Fills a solid rect; creates and deletes a brush per call. |
| 309-313 | `RRect` | Filled `RoundRect` with the given radius (default 10). Creates and deletes a brush and a pen per call, and leaves them selected in the DC after `DeleteObject`. |
| 314-316 | `Txt` | `DrawTextW` with font and colour. The default flags are single line, vertically centred, end ellipsis and no prefix. |
| 317 | `TxtW` | Text width via `GetTextExtentPoint32W`. |
| 319-325 | `Para` | Word-wrapped paragraph (`DT_CALCRECT`, then draw). Returns the height used. |
| 326-332 | `Chip` | Pill button. Registers a hit rect and returns its width plus an 8 px gap. |
| 333-337 | `Badge` | Small tinted pill in the detail pane. Advances `x`. |
| 339-481 | `Paint` | Draws the whole client area and rebuilds `hits` on every paint. Header 342-349, goal row 350-354, filter row and search box placement 355-361, list header 362-376, rows 377-401, list scrollbar 402-410, detail pane 411-480. |
| 427 | lambda `H` | Section heading inside the detail pane. |
| 483 | `Invalidate` | `InvalidateRect(h, 0, FALSE)`. |
| 484-492 | `LoadDumpDialog` | `GetOpenFileNameW` for `index.csv`, then `ReadDump(parent dir)`. Shows an error box on failure, then `Refilter`. |
| 494-579 | `Proc` | Window procedure. Cases: `WM_CREATE` 496-501 (search EDIT), `WM_CTLCOLOREDIT` 502, `WM_COMMAND` 503-507 (search `EN_CHANGE`), `WM_ERASEBKGND` 508, `WM_PAINT` 509-514 (double buffer), `WM_MOUSEMOVE` 515-532 (drag and hover), `WM_MOUSELEAVE` 533, `WM_LBUTTONUP` 534, `WM_LBUTTONDOWN` 535-559, `WM_MOUSEWHEEL` 560-565, `WM_KEYDOWN` 566-573, `WM_SIZE` 574, `WM_GETMINMAXINFO` 575, `WM_DESTROY` 576 (`PostQuitMessage`). |
| 581-604 | `wWinMain` | Sets DPI awareness, loads the DB, creates the fonts (lambda `F` at 584), loads the argv dump or live values, registers class `BSX`, creates the window, sets DWM dark mode, shows the startup MessageBox if nothing loaded, and runs the message loop. |

Dead or unused code (drop it in the port):
- `fMono` (586) is never used.
- Line 361 draws an empty string into an empty rect.
- `gLive` (40) is written at 147 and 169 and never read.
- The hit-kind comment at 304 lists kinds 1-5, but kind 6 (column header) also exists (372).
- `RRect`'s `rad` default.

### 1.2 Pipeline (`legacy/bios-explorer/pipeline`, Python 3, standard library only)

Run order: `unpack.py` → `builddb.py` (uses `ifr.py`) → `gen.py` (uses `kb.py`, `kb_impact.py`, `kb2.py`). The exact
command lines are not recorded anywhere. The ones below are inferred from `sys.argv` use.

**`unpack.py`**: `python unpack.py <SPI image> <mods.pkl>`
- `lzma_dec` (10-12): decodes an LZMA "alone" stream.
- `parse_sections` (17-54): walks FFS sections:
  - compression type 0 (uncompressed) recurses; Tiano compression is skipped (33);
  - GUID-defined LZMA / LZMA-F86 is decompressed and recursed, and errors are swallowed (41-42);
  - FV image (0x17) recurses into `parse_fv`;
  - UI section (0x15) sets the module name;
  - every other section is collected as `(type, bytes)`.
- `parse_fv` (56-88): walks the FFS files in a firmware volume. It handles the extended header, large files (attr bit 0) and 8-byte alignment. Raw/freeform files that contain an FV are recursed.
- `unpack` (90-110): scans the image for `_FVH` and parses each top-level FV. Nested ones are skipped via `covered_until`.
- `__main__` (112-120): prints module counts and setup-related names, then pickles `[(name, guid, ffs_type, [(sec_type, bytes)])]`.
- Output: the pickle. Its "563 modules" (HISTORY.md) can't be verified because the image isn't in the repo.

**`ifr.py`** (library):
- `find_string_packages` (7-23): finds HII string packages (type 0x04) by searching for `en-US\0` and validating the header.
- `parse_strings` (25-59): decodes the UEFI HII string blocks (SCSU/UCS2 single and multiple, duplicate, skip, ext). Unknown block types stop parsing.
- `find_form_packages` (62-79): finds HII form packages (type 0x02) that start with FORM_SET (0x0E) and end with `29 02`.
- `rd` (85-86): little-endian int.
- `parse_form` (88-178): the IFR opcode walk. It collects:
  - FORM_SET (106-107), FORM titles (108-109);
  - VARSTORE (110-113) and VARSTORE_EFI (114-119), with dead code at 115;
  - REF edges (120-122);
  - ONE_OF / NUMERIC / CHECKBOX questions (123-139), with width from flags, min/max/step and the hex flag;
  - ONE_OF_OPTION with default (0x10) and manufacturing default (0x20) flags (140-146);
  - DEFAULT opcodes, store 0 = standard and 1 = manufacturing (147-152);
  - SUPPRESS_IF / GRAY_OUT_IF / DISABLE_IF scopes (156-165). A condition counts as unconditional (`'true'`) only if the next opcode is TRUE (0x46) and isn't followed by AND/OR (0x15/0x16). This is a heuristic.

  The scope stack pops on END (168-176). Output: `dict(formset, varstores, forms, refs, questions)`.

**`builddb.py`**: `python builddb.py <mods.pkl> <db.json> [<dump dir>]`
- `build` (6-60):
  - takes section type 0x10 of the module whose GUID starts `899407d7` (the Setup module, 8) and parses its first string package and first form package;
  - builds the form tree: the parent is the first form that REFs a form, and the root is the first form (12-23);
  - `path` (24-30) builds the menu path `A > B > C`;
  - groups questions by `(varstore name, offset, width)` (31-52). It merges aliases, help text, options, the first non-null default and every path;
  - a group is **hidden** if no occurrence is visible: every occurrence is under an unconditional suppress or in a form that can't be reached (36, 55);
  - a group is **conditional** if every visible occurrence has some suppress/disable condition (52). Hidden groups are never conditional (56);
  - ids are assigned in IFR order (58).
- `decode` (62-95):
  - reads a dump's `index.csv` with `csv.DictReader`, which handles `""` correctly, and its `vars/*.bin`;
  - parses AMI `StdDefaults` NVAR records (69-86) into `{(name, size): bytes}`;
  - overrides `default` with the StdDefaults value when the record has the same name **and the same size** as the dumped variable (90-93). The IFR default is kept as `ifr_default`;
  - also stores `current`, which `gen.py` doesn't export.
- `__main__` (97-110): writes JSON and prints statistics (unique, hidden, conditional and changed settings).
- Note: varstore sizes from the IFR (`varstores[vid]['size']`) are parsed but never written out. The version guard in ARCHITECTURE.md needs them.

**`kb.py`**: the tier-1 KB.
- `G` (8) lists the goal keys `power, balanced, single, multi, latency`.
- `R()` (9-10) builds the per-goal rec dict.
- `SAFE14` (12-13) is a shared 13th/14th-gen warning.
- `KB` (15-155) holds **56** tuples `(regex on prompt, title, explanation, recs, note)`.
- Rec convention (comment at 4-5): an option-label prefix means an exact rec, a leading `~` means free advice, and `''` means no opinion.

**`kb_impact.py`**: tier-1 overlays keyed by KB title.
- `IMPACT` (9-186): 4-tuples (FPS/esports, other games, apps & work, claim check).
- `EXPL` (189-198): replacement explanations.
- `REC` (199-211): per-goal rec overrides.
- `NOTE` (212-219): replacement notes.
- `NOFX` (5) and `VOLT` (6-7) are shared strings.

**`kb2.py`**: tier 2.
- Each entry is `(regex, title, expl, recs, note, impact4)`.
- `NAME` (30-398) has 130 entries matched on the prompt, and `MENU` (401-459) has 26 matched on the joined menu paths.
- `FALLBACK` (461-463) is one entry. Shared strings: `N0`, `fx()`, `LEAVE`/`NOOP`, `KF`, `MEMTEST`, `TIGHT()` and the `MEMFX_*`/`SIGFX`/`VOLTFX` impact tuples (7-28).

**`gen.py`**: `python gen.py <db.json> <settings.db> [<title>]`
- `esc` (8-9): `\` becomes `\\`, a tab becomes a space, `\r` is dropped, and a newline becomes `\n`.
- It writes these lines, in order (14-35):
  - `V` (14);
  - the tier-1 `K` lines (17-22). Each note first has `Your PC:[^.]*\.\s*` removed (18), and the `EXPL`/`NOTE`/`REC`/`IMPACT` overlays are applied;
  - `T` = `len(KB)` (26);
  - the tier-2 `K` lines for `NAME`, `MENU` and `FALLBACK` (27-35).
- Each setting's KB index is chosen in this order (36-49):
  1. the first tier-1 regex `search` on the prompt;
  2. the first `NAME` regex on the stripped prompt;
  3. the first `MENU` regex on all of its paths joined with ` | `;
  4. `FALLBACK`.
- It then writes the `S` lines (50-55); at most 3 paths are kept (55).
- Prints coverage statistics (23-24, 57).
- **Bug at line 18:** the regex stops at the first `.`, so "Your PC: Override at 1.250 V. …" becomes "250 V. 13th/14th-gen …". The note of K 24 (`CPU Core Voltage Mode`) in the shipped `settings.db` really does start with "250 V.".

### 1.3 `legacy/uefi-dump`

**`Run-Dump.bat`** (9 lines): `net session` admin check (3). If that fails, it relaunches itself with `Start-Process -Verb RunAs` (5)
and exits. Otherwise it runs `Dump-UEFI.ps1` with `-ExecutionPolicy Bypass` (8) and pauses.

**`Dump-UEFI.ps1`**:
- 7-11: `WindowsPrincipal.IsInRole(Administrator)` check. On failure it prints "Please run this as Administrator (use Run-Dump.bat)." and exits 1.
- 13-107: inline C# (`Add-Type`):
  - `EnablePrivilege` (39-52) checks every call and reports 1300 (`ERROR_NOT_ALL_ASSIGNED`) separately;
  - `ReadAll` (54-82) uses `NtEnumerateSystemEnvironmentValuesEx(1)`: a size query, then +4 KB of slack, then walks `NextEntryOffset`/GUID/name at +20;
  - `Read` (84-105) calls `GetFirmwareEnvironmentVariableExW` starting at 4 KB and quadrupling on error 122, up to 4 MB. It records the Win32 error per variable, and 122 if the variable is larger than 4 MB.
- 111-115: enables the privilege, then reads everything.
- 117-136: writes `UEFI_Dump_<yyyyMMdd_HHmm>\vars\<file>.bin` and `index.csv` (format in section 3).
- 138-147: writes `system.txt` (board, BIOS, CPU from WMI).
- 149-150: `Compress-Archive` to `UEFI_Dump_<stamp>.zip`.
- 152-158: summary: the 15 largest variables and a count of failed reads.

---

## 2. `settings.db` format

Encoding:
- UTF-8, no BOM, LF line ends, a trailing newline, and no `\r` anywhere;
- one record per line, fields separated by TAB;
- the app splits the whole file on `\n` (86) and skips lines shorter than 2 characters (87);
- unknown line types are ignored.

Order in the shipped file:

| File lines | Content |
|---|---|
| 1 | `V` |
| 2-57 | tier-1 `K` 0-55 |
| 58 | `T` |
| 59-215 | tier-2 `K` 56-212 |
| 216-5583 | `S` 0-5367 |

### Line types and counts (`grep -c '^X<TAB>'`)

| Type | Count | Fields |
|---|---|---|
| `V` | 1 | 2 |
| `T` | 1 | 2 |
| `K` | 213 | 14 |
| `S` | 5,368 | 17 |

**`V`**: `V`, title. The title is escaped. Shipped value: `MSI PRO Z790-P WIFI (MS-7E06) BIOS 7E06vAF (A.F0)`.
It becomes `gTitle`, which is used only as display text. It is not machine-checkable, so there is no version guard (see 7).

**`T`**: `T`, the number of tier-1 KB entries. Shipped value: `56`. It becomes `gKeyCount`.

**`K`**: one KB entry. Fields:

| # | Field | Meaning |
|---|---|---|
| 0 | `K` | |
| 1 | index | **Ignored by the app**, which uses line order (`K.push_back`). The indices are written in order, so they agree. |
| 2 | title | |
| 3 | explanation | |
| 4-8 | recommendations | `power`, `balanced`, `single`, `multi`, `latency`. Each is an option-label prefix (exact), `~free text` (advice) or empty (no opinion). |
| 9 | note | |
| 10 | effect, FPS / esports | |
| 11 | effect, other games | |
| 12 | effect, apps & work | |
| 13 | claim check | |

- All text fields are escaped.
- The app requires at least 10 fields and reads 10-13 only if there are at least 14 (91, 95).
- Every shipped `K` line has 14 fields.

**`S`**: one setting. Fields:

| # | Field | Meaning |
|---|---|---|
| 0 | `S` | |
| 1 | id | 0-based, in IFR order. Equals the row index. |
| 2 | var | The varstore/variable name, e.g. `Setup` or `CpuSetup`. Escaped by gen.py, **not** unescaped by the app; there is no backslash in any shipped name. |
| 3 | guid | Lowercase, no braces. |
| 4 | offset | Decimal byte offset in the variable. |
| 5 | width | Bytes: 1, 2, 4 or 8. |
| 6 | kind | `oneof`, `numeric` or `checkbox`. |
| 7 | flags | `H` = hidden in every menu; `C` = shown only depending on other settings. Never both. |
| 8 | default | Decimal. Empty means no default. |
| 9 | min | Empty for non-numeric kinds. |
| 10 | max | |
| 11 | step | |
| 12 | prompt | Escaped. |
| 13 | help | Escaped. |
| 14 | menu path | Up to 3 paths joined by ` \| `; each path is `A > B > C`. Escaped. |
| 15 | options | `value 0x1F text`, joined by `0x1E`. The value is decimal; the text is escaped. Empty for numeric kinds. The app splits on the separators first and unescapes the text afterwards (104-105). |
| 16 | KB index | Into the `K` list. Never -1 in the shipped file. |

Shipped `S` statistics, computed with awk:

| Measure | Value |
|---|---|
| kind | oneof 2,937; numeric 2,295; checkbox 136 |
| width | 1 byte 4,228; 2 bytes 1,022; 4 bytes 106; 8 bytes 12 |
| flags | `H` 526; `C` 3,674 |
| default present | all 5,368 |
| options present | 3,074 settings, 13,244 options |
| more than one menu path | 358 |
| distinct (var, guid) | 20 (Setup 2,722, PchSetup 1,415, SaSetup 571, CpuSetup 514, UsbSupport 43, MeSetup 33, …) |
| prompts used by more than one setting | 410, covering 2,991 settings (these get `AddContext` labels) |

### Escaping and `Unesc`
- **Writer**, `gen.py esc`: a backslash becomes `\\`, a TAB becomes a space (lossy), `\r` is deleted, and a newline becomes `\n`. `0x1E`/`0x1F` inside option text are not escaped (none occur).
- **Reader**, `Unesc` (56-63): `\n` becomes a newline, and `\` followed by any other character gives that character. So `\\` gives `\`, and `\t` gives `t`, not a tab.
- The reader is the exact inverse of the writer for the characters the writer produces.
- `var`, `guid`, `kind` and the numbers are not unescaped.
- In the shipped file, 488 lines contain `\n` and 36 contain `\\`; the latter are ACPI paths such as `\\_SB.PC00…` in help text.

### Tier 1 and tier 2
- `K` entries with index `< gKeyCount` are **tier 1** ("key settings"): `IsKey()` (210), the blue dot, and the "Key settings" filter.
- Entries `>= gKeyCount` are **tier 2**: `NAME` 56-185, `MENU` 186-211 and `FALLBACK` 212.
- If the `T` line is missing, `gKeyCount` stays `1 << 30` (33), so every KB entry counts as key.

| Settings matched by | Count |
|---|---|
| tier 1 | 501 |
| tier-2 `NAME` | 2,964 |
| tier-2 `MENU` | 1,903 |
| `FALLBACK` | 0 |

Every setting has a KB entry, so `s.kb >= 0` always holds in practice. As a result the "What it does (BIOS help text)" heading (440) never appears with the shipped DB.

---

## 3. Snapshot (UEFI dump) format

### Produced by `Dump-UEFI.ps1`

Output folder: `UEFI_Dump_<yyyyMMdd_HHmm>` next to the script (`$PSScriptRoot`, 118). It contains:
- `index.csv`: `Export-Csv -NoTypeInformation -Encoding UTF8` (136). In Windows PowerShell 5.1 this is UTF-8 with a BOM, every field double-quoted, and CRLF line ends. In PowerShell 7, `UTF8` means no BOM. The app copes with both: it skips line 1, so the BOM doesn't matter.
- `vars\<file>.bin`: the raw variable data, one file per variable that was read successfully.
- `system.txt`: UTF-8 with a BOM. Its lines are `Dumped:`, `Board:`, `BIOS:` (manufacturer, SMBIOSBIOSVersion, release date, from WMI `Win32_BIOS`), `CPU:` and `Variables: N found, M read`. **The app doesn't read it.**

The zip is `UEFI_Dump_<stamp>.zip`, a sibling of the folder (149-150). It is made with `Compress-Archive -Path <folder>\*`, so its root holds `index.csv`, `system.txt` and `vars/…` directly, without the folder. The app can't open the zip; the user has to extract it. Whether Compress-Archive sets the UTF-8 flag (bit 11) is unclear and depends on the PowerShell/.NET version. File names are ASCII after sanitising, so in practice only the zip's folder name could be non-ASCII.

`index.csv` columns, in order:

| # | Column | Content |
|---|---|---|
| 0 | `Name` | UEFI variable name, unsanitised. |
| 1 | `Guid` | `{xxxxxxxx-xxxx-…}`: `Guid.ToString("B")`, lowercase, with braces. |
| 2 | `Size` | Bytes read; 0 if the read failed. |
| 3 | `Attributes` | Hex `0x%X` of the attributes returned (`0x0` on failure). |
| 4 | `Error` | Win32 error from `GetFirmwareEnvironmentVariableExW`. 0 = OK; 122 = larger than 4 MB. Other codes are as returned (203, 1314, 5, …). |
| 5 | `File` | The `.bin` file name, or empty if no data. |

File naming in `vars\` (123-125):
1. Replace every character outside `[A-Za-z0-9_\-\.]` with `_`.
2. Truncate to 80 characters.
3. The name is `<that>__<guid without braces>.bin`.

Two different variables collide when, with the same GUID, their names:
- differ only in replaced characters;
- share the first 80 sanitised characters;
- or differ only in case, because NTFS is case-insensitive.

When that happens the second `WriteAllBytes` silently overwrites the first, and both rows point at the same file. ARCHITECTURE.md's fix is to add an index number to every file name.

### Parsed by `ReadDump` (150-170)

- Opens `<dir>\index.csv` with `_wfopen` (MAX_PATH, no `\\?\`) and reads it all in 64 KB chunks.
- Decodes it as UTF-8 and splits it on `\n`.
- Skips the first line, assuming it is the header; the header is never checked.
- Per line, the CSV tokeniser (159) works like this:
  - `"` toggles the in-quotes state and is never copied, so an escaped `""` vanishes: `"a""b"` gives `ab`;
  - `,` outside quotes ends a field;
  - `\r` is dropped anywhere;
  - a newline inside a quoted field would split the record, because the split on lines happens first.
- Rows with fewer than 6 fields, or an empty `File` (`c[5]`), are skipped. `Size`, `Attributes` and `Error` are ignored.
- GUID: if it starts with `{`, the first and last characters are removed (both braces are assumed).
- Opens `<dir>\vars\<File>` and reads it whole. If it can't be opened, the row is skipped silently. The file's length isn't compared with `Size`.
- Key: `lower(Name)|lower(guid)`. If the same key appears again, the later row wins (`map` assignment).
- If no variables loaded, it returns false and the previous values stay. Otherwise it calls `ApplyVars`, so settings whose variable is missing from the dump become `-`, and sets `gSource = "Saved dump: <dir>"`.
- `LoadDumpDialog` (484-492):
  - filter `"UEFI dump index (index.csv)\0index.csv\0All files\0*.*\0"`;
  - title `"Open the index.csv inside a UEFI_Dump folder"`;
  - `OFN_FILEMUSTEXIST`;
  - the dump dir is the chosen file's parent, so any file in the dump folder works.

---

## 4. Parity checklist

### 4.1 Window and layout (system-DPI pixels, unscaled)
- [ ] Top-level window titled `BIOS Settings Explorer`, default size 1440×880, minimum track size 1000×640 (575, 597). DWM immersive dark title bar (attributes 20 and 19, 598-599).
- [ ] Header, in-client:
  - `BIOS Settings Explorer` in fTitle, cText at (20,12)-(600,44) (343);
  - subtitle `<gTitle>   •   <gSource>` in fSmall, cMute at (20,44)-(W-330,66) (344).
- [ ] Buttons, top right, y 18-48, right-aligned from `W-20` with a 10 px gap:
  - `Export changes`: cAcc fill, dark text RGB(10,20,30), hover RGB(110,190,255), radius 10 (346-347);
  - to its left, `Load dump...`: cChip fill, cText, hover RGB(64,68,75) (348-349).
  - Each button is `textW(fSmall)+26` wide.
- [ ] Goal row at y=76, filter row at y=112. Each starts with a 50 px wide label in fSmall, cMute.
- [ ] Search EDIT at (W-340, 113), 320×24 (360):
  - font fBody; cue banner `Search settings...` (499);
  - background RGB(44,47,52), text cText (502); no border.
- [ ] List panel: x=20, y=152, width `55%` of the client width, height `H-172`, cPanel, radius 12 (363-364).
- [ ] Detail panel: from `listX+listW+16` to `W-20`, same top and height, cPanel, radius 12 (412-413).
- [ ] Column x positions, as fractions of the list width: name `listX+14`, Current 52%, Default 70%, Goal 85%, right edge `listX+listW-10` (365-367).

### 4.2 Goal selector
- [ ] Label `Goal`, then the chips `None`, `Power saving`, `Balanced`, `Single-core`, `Multi-core`, `Low latency` (34, 352-354). The selected chip is filled with cAcc. `None` is selected at startup (`gGoal = -1`).
- [ ] Choosing a goal sets `gGoal` and calls `Refilter`.
- [ ] Choosing `None`:
  - if the filter is `Differs from goal`, it resets to `All`;
  - if the sort is the Goal column, it resets to the default order (547).
- [ ] With no goal:
  - the `Differs from goal` chip is hidden (359);
  - the `Goal` column header isn't drawn (370) and the Goal column is empty;
  - the detail pane still shows "Recommended by goal" for all 5 goals, with no goal highlighted.

### 4.3 Filters (exact labels, 358)
- [ ] Label `Show`, then the chips, one at a time (radio behaviour):

  | Chip | Shows |
  |---|---|
  | `All` | everything |
  | `Key settings` | tier-1 KB entries only (`IsKey`) |
  | `Changed from default` | `hasCur && hasDef && cur != def` |
  | `Hidden in menus` | flag `H` |
  | `Differs from goal` | only when a goal is set: an exact rec for the goal that differs from a known current value |

  Note: HISTORY.md and the spec call the fourth filter "Hidden". The real label is `Hidden in menus`.
- [ ] The filter and the search combine with AND.

### 4.4 Search
- [ ] Live, on every `EN_CHANGE` (504-505). Reads up to 255 characters.
- [ ] Case-insensitive (`towlower`) **substring** match of the whole query, with no tokenising and no trimming, against `prompt + " " + ctx + " " + path + " " + var + " " + KB title` (247).
- [ ] It does **not** match help text, option texts, values, KB explanation, offsets or GUIDs.
- [ ] An empty query matches everything.

### 4.5 Default order (`Refilter` 252-256)
- [ ] Stable sort in four tiers, keeping IFR (id) order inside each tier:
  1. key and changed;
  2. key and unchanged;
  3. non-key and changed;
  4. non-key and unchanged.
- [ ] Every filter change, search keystroke, goal change, sort click or dump load re-runs this, then `SortV`.
- [ ] Selection after `Refilter`:
  - if the selected setting is filtered out, select the first visible row (or none);
  - with no selection, select the first row;
  - `gTop` is set to 0, then, if the selection's row `k >= rowsVisible`, to `k - rows/2`, which centres it.
  - `detScroll` is **not** reset when the selection changes this way (a bug; Paint clamps it).

### 4.6 Sorting (`SortV` 216-236, click handling 552-556)
- [ ] Click a header: `Setting  (N)    grey = hidden in BIOS menus`, `Current`, `Default`, `Goal` (368). `N` is the visible row count.
- [ ] The sorted header shows `  ▲` (ascending) or `  ▼` (descending) in fBold, cAcc. Other headers are fSmall: cText on hover, cMute otherwise (373-374).
- [ ] Three-click cycle (553-554):
  - clicking a new column sorts it ascending;
  - clicking the same column again sorts it descending;
  - a third click returns to the default order (`gSortCol = -1`).
- [ ] Every sort is a `stable_sort` applied **on top of** the default order, so ties keep the default tiers.
- [ ] **Setting**: `_wcsicmp(prompt)`. The context label isn't included.
- [ ] **Current / Default**:
  - settings with no value (`-`) always sort last, in both directions (223);
  - otherwise the comparison uses the *display* text: if both start with a digit, the raw values are compared numerically;
  - if only one starts with a digit, text values come before numeric ones in ascending order;
  - otherwise the texts are compared case-insensitively. So "Auto", "Disabled" and "Enabled" sort alphabetically, before every number.
- [ ] **Goal**: `GoalRank` (211-215):

  | Rank | Meaning |
  |---|---|
  | 0 | an exact rec that differs from the current value ("change this") |
  | 1 | advice text: a `~` rec, or a rec that matches no option ("test both" advice) |
  | 2 | an exact rec equal to the current value ("already matches") |
  | 3 | an exact rec but the current value is unknown |
  | 4 | no advice for this goal: no KB entry, or an empty rec |

  - Rank 4 is always last, in both directions (230).
  - Ties are broken by `_wcsicmp(prompt)`, and the direction reverses the tie-break too.
  - README line 14 describes this order: "settings to change first, then "test both" advice, then settings that already match".

### 4.7 Rows
- [ ] Row height 26; the drawn rect is 24 high, from `listX+6` to `listX+listW-10`, radius 8 (377-383). Rows start at `listY+34`; rows visible = `(listH-40)/26`.
- [ ] Selected row: cSel fill. Hovered row: cRow fill.
- [ ] Key setting: a 7×7 cAcc dot at (nameX, rowY+9) (385). The name starts 12 px further right whether or not there is a dot.
- [ ] Name: fBody, **cMute (grey) if hidden in menus**, otherwise cText (387-388). Clipped with an ellipsis before the Current column.
- [ ] Context label (AddContext): `·  <ctx>` (U+00B7 followed by two spaces) in fSmall, RGB(105,110,118), right after the name. It is drawn only if `nameX + nameWidth + 30 < curX - 10` (389-392).
- [ ] Current: **cWarn (orange) if changed from default**, otherwise cText (393).
- [ ] Default: cMute (394).
- [ ] Goal column, only when a goal is set and the setting has a KB entry (395-399):
  - exact rec: cGood (green) if it equals the current value, cBad (red) if not;
  - advice text: cMute.
  - **Bug:** for an exact rec with no current value it compares against the stale or zero `cur` and can show green or red. The port should treat a missing current as unknown.

### 4.8 Detail pane (411-480)
- [ ] With no selection: `No setting selected` in fBody, cMute (414).
- [ ] Content is clipped to the panel, with 20 px padding, starting at `listY+18-detScroll`.
- [ ] Sections, in order:
  1. **Prompt**: fTitle, cText, wrapped.
  2. **Path**: fSmall, cMute, if non-empty. This is the raw `A > B | C > D` string.
  3. **Badges** (421-425), on one line:
     - either `Hidden in BIOS menus` (cMute) or, failing that, `Shown depending on other settings` (cMute) if conditional;
     - `Changed from default` (cWarn) if changed;
     - always the location, formatted `<var> + 0x<OFFSET hex upper> (<n> byte|bytes)`, e.g. `Setup + 0x1A (1 byte)`.
  4. `What it does`: the KB explanation, fBody, cText (428).
  5. `Real-world effect`, if `fx[0]` is non-empty (429-439):
     - rows labelled `FPS / esports`, `Other games` and `Apps & work`, 130 px wide, fBold, cMute, with fBody cText text;
     - if all three texts are identical, one row labelled `All workloads`;
     - then `Claim check` if a claim exists, in RGB(200,170,255). **It appears only inside the Real-world-effect block**, so a claim with an empty `fx[0]` is never shown.
  6. Help text (440): heading `BIOS help text` in cMute when there is a KB entry; otherwise `What it does (BIOS help text)` in cText.
  7. `Values` (441-458):
     - `Current: <cur>      Default: <def>` (six spaces);
     - for numeric settings with a min: `Range <min> to <max>, step <step>` in fSmall, cMute, where step 0 is shown as 1;
     - then one row per option, 24 high, every 26 px. The current option has a cSel background, a `● ` marker and cText; the others are prefixed with three spaces, in cMute;
     - right-aligned tags in a 200 px column, fSmall: `default ` and/or `best for <Goal>`. cGood if the option is the recommendation, otherwise cMute.
  8. `Recommended by goal` (459-467), when there is a KB entry:
     - five rows, each with a 130 px goal label: fBold and cAcc for the active goal, otherwise fBody and cMute;
     - the text is `RecText`, or `no change needed` when the rec is empty;
     - colour cGood if the rec is exact and equals a known current value, otherwise cText.
  9. `Note` (468), in cWarn, if non-empty.
- [ ] Section heading: 6 px above, fBold, cAcc, 22 high, and 24 px of advance (427).
- [ ] Selecting a row (click or key) resets the detail scroll to 0.

### 4.9 Scrollbars and dragging
- [ ] List scrollbar, only when there are more rows than fit (403-410):
  - track `listX+listW-14 .. listX+listW`, from `listY+34`, height `listH-44`;
  - thumb 8 px wide (`-11..-3`), at least 30 px tall, proportional;
  - colour RGB(70,74,80); RGB(110,116,125) while dragging or while the mouse is over the track (`gHover = -2`).
- [ ] Detail scrollbar, when the content is taller than the view (474-480): track `detX+dw-14..detX+dw`, top `listY+8`, height `listH-16`. The thumb is the same. Hot colour only while dragging; no hover state.
- [ ] Pressing on a track starts a drag (537-545):
  - on the thumb, it keeps the grab offset;
  - elsewhere on the track, the thumb's centre jumps to the cursor;
  - the mouse is captured.
- [ ] Moving maps `(y - offset - trackTop) × range / (trackH - thumbH)`, clamped. Release ends the drag.
- [ ] Hover highlight: hit test on `WM_MOUSEMOVE`, with `TrackMouseEvent(TME_LEAVE)` to clear it. The cursor is always the arrow; there is no hand cursor.

### 4.10 Mouse wheel and keys
- [ ] Wheel (560-565): `notches = delta / 120` (integer division).
  - Cursor x `>= detX` (anywhere right of the list, including the header): the detail pane scrolls 60 px per notch.
  - Otherwise the list scrolls 3 rows per notch, clamped to `[0, V.size()-1]`; Paint clamps it again.
  - y is ignored.
- [ ] `WM_KEYDOWN` on the main window (566-573). These are the only keys:

  | Key | Selection moves |
  |---|---|
  | `VK_DOWN` | +1 |
  | `VK_UP` | −1 |
  | `VK_NEXT` (PgDn) | + rows visible |
  | `VK_PRIOR` (PgUp) | − rows visible |

  - The selection is clamped to the list and scrolled into view (minimal scroll). The detail scroll resets.
  - Nothing else is handled: no Home/End, Enter, Esc, Tab, Ctrl+F, Ctrl+C or F5.
  - With no current selection, movement starts from row 0.
- [ ] Focus:
  - clicking any hit target calls `SetFocus(main)` (557), which takes focus away from the search box;
  - while the search box has focus, the arrow keys go to the EDIT and don't move the list;
  - clicking empty background doesn't move focus.
- [ ] Left click on a row selects it (549). There is no double-click, right-click or context menu.

### 4.11 Load dump and Export changes
- [ ] `Load dump...` opens the file dialog (section 3, "Parsed by ReadDump"). On failure: MessageBox text `Could not read that dump.`, caption `Load dump`, MB_ICONERROR (490). Then `Refilter` and repaint.
- [ ] `Export changes` with no goal: MessageBox `Pick a goal first (Power saving, Balanced, ...).`, caption `Export`, MB_ICONINFORMATION (267).
- [ ] Export path: `<exe dir>\BIOS changes - <Goal>.txt`. An existing file is overwritten without asking (291).
- [ ] On open failure: `Could not write <path>`, caption `Export`, MB_ICONERROR. On success the file opens with `ShellExecuteW "open"`.
- [ ] Export format: UTF-8 with a BOM, CRLF. The text, exactly:

  ```
  BIOS changes for goal: <Goal>
  <gTitle>
  <gSource>

  Read-only report: apply these yourself in the BIOS (F7 advanced mode). Change a few at a time and test.

  === Settings to change ===
  - <prompt>: <CurText>  ->  <RecText>[   [hidden in BIOS menus]]
      where: <path>                      (only if path non-empty)
      note: <KB note>                    (only if note non-empty)
  ...
  (none - everything with an exact recommendation already matches)    (only if no rows)

  === Advice (check these by hand) ===
  - <KB title> (now: <CurText>): <RecText>
  ```

  The notes in parentheses are explanations, not part of the output. The separator in the change line is two spaces, `->`, two spaces; the hidden tag is preceded by three spaces. Indents are 4 spaces.
- [ ] "Settings to change" lists every setting in the map where `Differs()` holds:
  - in IFR/id order, **ignoring the current filter, search and sort**;
  - without context labels, so duplicates differ only by `where:`.
- [ ] "Advice" has one line per KB entry, for the first setting of that entry, in id order, whose rec for the goal is non-empty and not exact.
  - `now:` shows **that** setting's current value, which may be `-`.
  - An entry is marked seen only once it is printed. A later setting of the same entry can therefore still print if earlier ones had exact recs.
  - Tier-2 entries with advice are included too.

### 4.12 ZeroIsAuto (173-181)
- [ ] Applies only when the value is 0 and the setting has **no options** (184).
- [ ] It is true if the lowercased help text contains any of: `0: auto`, `0 = auto`, `0=auto`, `0 means auto`, `0 - auto`, `0 = no override`, `0- auto`, `0 - mrc auto`, `0-auto`.
- [ ] It is also true if all of these hold:
  - kind is `numeric`;
  - var is non-empty;
  - the path contains `DRAM`, `XMP User Profile` or `Memory Overclocking` (case-sensitive);
  - min == 0.

  This is MSI's convention for memory timing fields.
- [ ] Then `Auto` is shown in Current, Default, Values and the export, and it sorts as text.
- [ ] README line 11: memory timings showing "Auto" are trained at boot, and the BIOS doesn't store the trained values.

### 4.13 AddContext labelling (72-79)
- [ ] Counts each prompt across **all** settings. For every setting whose prompt occurs more than once and whose path is non-empty:
  1. take the first path (the text before the first ` | `);
  2. take its last ` > ` segment, or the whole path if there is no `>`;
  3. use that as `ctx`. It is cleared if it equals the prompt.
- [ ] `ctx` is shown after the row name and included in the search. It isn't used in the detail title, sort or export.
- [ ] Settings with an identical prompt **and** an identical last menu segment get the same label, so they remain ambiguous. Unclear how many; this needs counting during the port.

### 4.14 Startup flow (`wWinMain` 581-604)
1. [ ] `SetProcessDPIAware()`.
2. [ ] `LoadDb()`, then `AddContext()`. If the resource is missing, the list stays empty and no error is shown.
3. [ ] Create the fonts.
4. [ ] `CommandLineToArgvW`. If `argc > 1`, `ReadDump(argv[1])` with argv[1] used as given; a relative path resolves against the CWD. Any further args are ignored.
5. [ ] If no dump was loaded, `ReadLive()`, and success is a return value > 0. A bad dump path therefore **silently falls back** to live.
6. [ ] If nothing loaded, `gSource = "No values loaded - run as administrator, or use Load dump..."`.
7. [ ] `Refilter()`, register class `BSX`, create and show the window.
8. [ ] If nothing loaded, show a MessageBox **after** `ShowWindow`/`UpdateWindow` (600-601), owned by the main window:
   - caption `BIOS Settings Explorer`, MB_ICONINFORMATION;
   - text `Couldn't read the BIOS settings from this PC.\n\nRun BIOS Settings Explorer as administrator, or click "Load dump..." and open the index.csv from a UEFI_Dump folder.`
9. [ ] Message loop. The value reads in steps 4-5 run on the UI thread before the window exists.

### 4.15 README promises (README.txt)
- [ ] "It only READS. It never writes anything to the BIOS." (4)
- [ ] Orange = changed from the BIOS defaults (7); blue dot = key setting (8); grey rows = hidden (15); repeated names show their port or core (15).
- [ ] "Export changes" writes the list "saved next to the exe" (16). The port changes this.
- [ ] The BIOS-version warning is text only (19-20).

---

## 5. Theme

Palette (301-303):

| Token | RGB | Use |
|---|---|---|
| `cBg` | 24, 25, 28 | window background |
| `cPanel` | 32, 34, 38 | list and detail panels |
| `cRow` | 38, 40, 45 | hovered row |
| `cSel` | 46, 70, 96 | selected row, current option |
| `cText` | 228, 230, 233 | primary text |
| `cMute` | 140, 145, 152 | secondary text, hidden names, default column |
| `cAcc` | 86, 170, 255 | accent: selected chips, key dot, headings, sorted header, Export button |
| `cWarn` | 240, 180, 80 | changed value, "Changed from default" badge, Note |
| `cBad` | 240, 110, 100 | exact rec ≠ current |
| `cGood` | 110, 200, 140 | exact rec = current, "best for" tag |
| `cLine` | 52, 55, 60 | **unused** |
| `cChip` | 44, 47, 52 | idle chip, Load dump button, search box background (500, 502) |

Inline colours:

| RGB | Lines | Use |
|---|---|---|
| 58, 62, 68 | 329 | chip hover |
| 10, 20, 30 | 330, 347 | text on cAcc |
| 110, 190, 255 | 347 | Export hover |
| 64, 68, 75 | 349 | Load dump hover |
| 105, 110, 118 | 391 | context label |
| 70, 74, 80 / 110, 116, 125 | 409, 479 | scroll thumb idle / hot |
| 200, 170, 255 | 438 | claim check text |
| per-channel `c/4 + 20` | 335 | badge background |

Fonts (584-586): `CreateFontW(-px, …, CLEARTYPE_QUALITY, …)`. The sizes are character heights in pixels with **no DPI scaling**.

| Font | Size | Weight | Face |
|---|---|---|---|
| `fTitle` | 22 px | FW_SEMIBOLD (600) | Segoe UI |
| `fBody` | 15 px | FW_NORMAL | Segoe UI |
| `fSmall` | 13 px | FW_NORMAL | Segoe UI |
| `fBold` | 15 px | FW_SEMIBOLD | Segoe UI |
| `fMono` | 13 px | FW_NORMAL | Consolas (unused) |

Metrics, in px:

| Element | Metrics |
|---|---|
| Chip (326-332) | width `textW(fSmall)+22`, height 26, corner radius 26 (pill), gap 8. On: cAcc with RGB(10,20,30) text. Off: cChip with cText, hover RGB(58,62,68). Text centred. |
| Badge (333-337) | width `textW(fSmall)+14`, height 20, radius 8, gap 6. Text in the badge colour; background the same colour/4 + 20. |
| Header buttons | width `textW(fSmall)+26`, height 30 (y 18-48), radius 10, gap 10. |
| Panels | radius 12. |
| Rows | pitch 26, height 24, radius 8. Key dot 7×7 (radius 7). |
| Scroll thumb | width 8, track width 14, minimum length 30, radius 6. |
| Detail pane | padding 20. Section heading block 6+24. Option rows 26 pitch. Goal and effect labels 130 wide. Tag column 200 wide. |
| Row labels | `Goal` and `Show` labels 50 wide. |

---

## 6. Edge cases

- **Variable buffer size.**
  - `ReadLive` uses a fixed `vector<BYTE>(1<<16)` (138). A variable over 64 KB returns 0 with `ERROR_INSUFFICIENT_BUFFER`, which is treated as missing, and every setting in it shows `-`.
  - The Setup varstores are far smaller in practice, but the port must grow the buffer on error 122.
  - Dump-UEFI grows its buffer up to 4 MB. `ReadDump` reads files of any size.
- **Missing variables.** A missing var, or one that is too short, gives `hasCur = false`:
  - `-` in Current, sorted last, never "Changed", never "Differs";
  - in Goal sort it is rank 3 (exact rec) or rank 1 (advice).

  Live reads only the 20 (var, guid) pairs referenced by the map, not every NVRAM variable.
- **Privilege failure.**
  - `EnablePriv`'s result is ignored (134).
  - `LookupPrivilegeValueW` (118) and the return value of `AdjustTokenPrivileges` (119) are unchecked; only `GetLastError()==0` is tested.
  - If every read fails, the user sees the generic "run as administrator" box.
- **Error codes.** `GetLastError()` is never read after a failed read, so the code can't tell apart:
  - 203 (`ERROR_ENVVAR_NOT_FOUND`, a map mismatch);
  - 1314 (privilege not held);
  - 5 (access denied, protected);
  - 122 (buffer too small);
  - 1 (`ERROR_INVALID_FUNCTION`, a legacy-BIOS boot). `GetFirmwareType` is never called.
- **Partial reads, offset + width beyond size.** `ApplyVars` (126) requires `offset + width <= size`, otherwise `-`. There is no warning when the variable's size differs from what the map expects. `builddb.decode` applies StdDefaults only when the sizes are equal.
- **Value decoding.**
  - Little-endian into a signed `long long`. A width-8 value with the top bit set becomes negative; its display text starts with `-`, so it sorts as text.
  - A checkbox reads the whole byte.
- **CSV quoting.**
  - `""` is dropped (see section 3, "Parsed by ReadDump"). A variable name containing `"` would be mangled, so its key wouldn't match and its settings would show `-`.
  - A quoted newline breaks the row.
  - The header row is assumed to be line 1 and is never validated.
- **Duplicate names.**
  - In the dump, the later row with the same lowercased name and GUID wins.
  - Map keys and lookups lowercase the variable name (124, 140, 166), but UEFI names are case-sensitive, so two variables differing only in case would collide.
  - In the UI, duplicate prompts are disambiguated by `AddContext`, but not when the menu segments are identical too (4.13).
  - In the export, duplicates are distinguished only by `where:`.
- **KB index out of range.** `S` field 16 is not bounds-checked against `K.size()`. A bad DB crashes on `K[s.kb]`.
- **Empty list.** Keys return early when there are no rows; with no selection the detail pane shows `No setting selected`.
- **Wheel.** `delta / 120` truncates, so high-resolution wheels and touchpads, which send deltas below 120, don't scroll at all.
- **DPI.**
  - `SetProcessDPIAware()` (582) and manifest `<dpiAware>true</dpiAware>` (app.manifest 8) give system DPI only.
  - Every metric and font is a raw pixel constant, so at 150% the UI is drawn at 100% size: small but sharp. On a second monitor with a different DPI, it is bitmap-stretched by DWM.
  - The minimum size of 1000×640 is unscaled.
- **Long paths.**
  - `MAX_PATH` buffers for the export (268) and the open dialog (485).
  - No `\\?\` prefix on `_wfopen`.
  - `GetModuleFileNameW` is not looped.
- **Search buffer.** Text over 255 characters is truncated (505).
- **Missing icon.** `LoadIconW(hi, L"APPICON")` (595), but app.rc defines no icon, so the class icon is NULL.

---

## 7. Rule violations and contract gaps

`CLAUDE.md` on disk now says (rule 4): *every exe is `requireAdministrator`, with no non-admin mode and no relaunch*.
docs/ARCHITECTURE.md (build outputs table, `Admin::Optional`) and docs/HISTORY.md ("should be `Admin::Optional` +
asInvoker") still describe the older rule. **The lead needs to reconcile these.** The verdicts below follow the current CLAUDE.md.

### Known list (confirmed or corrected)

| # | Issue | Where | Verdict | Port fix |
|---|---|---|---|---|
| 1 | Fixed 64 KB variable buffer | bsx.cpp 138, 143-144 | Confirmed | `IFirmwareVars` live: `GetFirmwareEnvironmentVariableExW` with a buffer that grows on 122. |
| 2 | Window class `L"BSX"` | bsx.cpp 594, 597 | Confirmed | `Hadron.bios.<name>`, as a child of the host HWND; no top-level window. |
| 3 | `requireAdministrator` | app.manifest 5; README 3 | **Now compliant** with the current CLAUDE.md rule 4. It conflicts only with ARCHITECTURE/HISTORY. | Generated manifest. If the process isn't elevated (e.g. Wine), say so in the UI and degrade to snapshot mode, with no relaunch loop. |
| 4 | Export writes next to the exe | bsx.cpp 268-270, 291 | Confirmed. It also **silently overwrites** an existing file of the same name (291, `"wb"`), and `GetModuleFileNameW` isn't looped. | Save dialog owned by `IHost::Frame()`. Default name `BIOS changes - <Goal>.txt`; the dialog asks before overwriting. |
| 5 | CSV parser ignores `""` escapes | bsx.cpp 159 | Confirmed: `""` is deleted, not turned into `"`. Quoted newlines also break it. | An RFC 4180 parser in the snapshot implementation. |
| 6 | No SMBIOS version guard | — (only README 19-20) | Confirmed. The map has only a display title (`V`), and `gen.py` doesn't emit the varstore sizes `ifr.py` parses. | Emit board, BIOS version and the expected size per varstore from the pipeline. Read SMBIOS types 0 and 2 via `GetSystemFirmwareTable('RSMB')`. On a mismatch, show a banner and mark values "unverified". |
| 7 | Run-Dump.bat uses `net session` | Run-Dump.bat 3-6 | Confirmed. If LanmanServer is disabled and elevation succeeds, `net session` still fails, so it relaunches forever (incident 3). | Retire the script; snapshots move into the module. If a script survives, use `fltmc`, relaunch at most once, and ship it in a zip (rule 6). |
| 8 | `SetProcessDPIAware` | bsx.cpp 582; app.manifest 8 | Confirmed | PerMonitorV2 from the host. Every metric through `S(px)`; fonts rebuilt in `OnDpiChanged`. |
| 9 | "MessageBox before window" | bsx.cpp 600-601 | **Corrected:** the box is shown *after* `ShowWindow`/`UpdateWindow`, owned by the main window. The real problems: (a) it is a modal box at startup, and a module's `Create` must not open dialogs unprompted; (b) its text also appears when an argv dump path failed, which is misleading; (c) all firmware reads happen on the UI thread before the window exists (590-591). | An inline empty state or banner in the module view with the reason (see #10) and a `Load snapshot...` button. Read on a `Worker` after `Create`. |
| 10 | Can't tell 203 / 1314 / access denied apart | bsx.cpp 134, 143-147 | Confirmed: `GetLastError` is never read and `EnablePriv`'s result is discarded. Also: no `GetFirmwareType()` and no `TokenElevation` check. | Report per-variable state: 203 = not found (map mismatch), 1314 = privilege missing, 5 = protected. Treat the privilege as enabled only if `AdjustTokenPrivileges` succeeded and `GLE != ERROR_NOT_ALL_ASSIGNED`. |

### Additional findings

| # | Issue | Where | Rule / contract | Port fix |
|---|---|---|---|---|
| 11 | Owns a message loop, `PostQuitMessage` in `WM_DESTROY`, top-level `CreateWindowExW`, DWM attributes on its own window | 576, 594-602 | Module contract | `Create(IHost&, parent, rect)` returns a child HWND; the host owns the frame, dark mode and the loop. |
| 12 | `MessageBoxW` for every message | 267, 292, 490, 601 | "Dialogs: owned by Frame, use `hx::ui::Confirm()`" | `Confirm()`/host dialogs. Use inline status (`IHost::SetStatus`) for non-blocking notices such as "pick a goal first". |
| 13 | Errors not surfaced with their text | 82-84 (missing resource gives an empty list, silently; `LoadResource`/`LockResource` unchecked); 151, 163 (`_wfopen` failures give a generic message or are skipped silently); 293-294 (`fwrite`, `ShellExecuteW` unchecked); 596-597 (`RegisterClassW`/`CreateWindowExW` unchecked) | Code standards: check every Win32 call and surface the error text | Check each call; show `FormatMessage` text. A dump with unreadable `vars` files should report how many were skipped. |
| 14 | Firmware and dump I/O on the UI thread | 590-591, 489-490 | "Long work runs on `Worker` threads" | Live read and snapshot load on a `Worker` that posts `WM_APP+0x100+n` to the module HWND. (The reads are small, but a dump on slow or network media can stall.) |
| 15 | Long-path handling missing | 268, 485-489, 151, 163 | Code standards: `GetFullPathNameW` + `\\?\`, loop `GetModuleFileNameW` | Use the `core/sys` long-path helpers. Make the `argv` dump path absolute. |
| 16 | Resource name `SETTINGS` and missing icon `APPICON` | app.rc 3; bsx.cpp 81, 595 | ARCHITECTURE "Data embedding": module-prefixed RCDATA; only the exe `.rc` owns `ICON 1` | `BIOS_SETTINGS` RCDATA in `modules/bios/resources/bios.rc`. |
| 17 | Manifest lacks PerMonitorV2, longPathAware, supportedOS and VERSIONINFO | app.manifest, app.rc | ARCHITECTURE "Build outputs" | Generated `.rc`/manifest via `configure_file`. |
| 18 | Keyboard and accessibility gaps: no focus rect, no high-contrast fallback, no Home/End, Tab, Esc or Enter, no keyboard scrolling of the detail pane | 566-573 | Code standards UI | Use the `core/ui` list view, which provides nav, focus rects and high contrast. Keep parity keys and add the standard ones. |
| 19 | Wheel truncates sub-120 deltas | 562 | UI: every list scrolls with the wheel | Accumulate the delta. The host forwards `WM_MOUSEWHEEL`; use the cursor position for routing. |
| 20 | No crash minidump handler | — | Code standards | Host-provided. |
| 21 | Case-insensitive variable-name matching | 124, 140, 166 | Correctness (UEFI names are case-sensitive) | Match the name exactly; compare GUIDs case-insensitively. |
| 22 | Hard-coded `UNICODE` defines, no `_WIN32_WINNT`, no `win_compat.h`; `EM_SETCUEBANNER` defined locally | 4-12 | Code standards | Use `core/sys/win_compat.h`. |
| 23 | Dump-UEFI writes next to the script and silently overwrites a same-minute dump (`New-Item -Force`, `WriteAllBytes`, `Compress-Archive -Force`) | Dump-UEFI.ps1 118-126, 150 | Rule 1 spirit, and ARCHITECTURE "never write next to the exe" | "Save snapshot" in the module: a Save dialog, a zip with UTF-8 flag bit 11, and no silent overwrite. |
| 24 | Dump file-name collisions overwrite data | Dump-UEFI.ps1 123-126 | ARCHITECTURE snapshot fixes | Prefix the file name with an index number, as specified. The reader must still accept legacy dumps. |
| 25 | Rule 2 (firmware read-only) | all files | **Compliant**: no `SetFirmwareEnvironmentVariable*`, `NtSetSystemEnvironmentValue*` or `SetVariable` anywhere in scope (grep) | Keep the CI grep. |
| 26 | Rule 3 (no third-party) | pipeline | **Compliant**: standard-library Python (`lzma`, `uuid`, `struct`, `pickle`, `csv`, `json`); the app is Win32 only | — |
| 27 | Owner-specific text inside the KB | kb2.py 13 (`KF`: "Your 14700KF has no integrated graphics…", used at 43, 237, and similar text at 384, 450); kb_impact.py 90 ("a fixed 53x"); gen.py 18 strips only `Your PC:` sentences, and that regex corrupts K 24 ("250 V.") | Rule 7 (honest, checkable claims) and personal data in shipped content | kb-auditor: make machine-specific text conditional on SMBIOS/CPUID facts, or phrase it generally. Remove the broken regex and keep the owner's state out of the KB source. |
| 28 | The owner's 9900K prefetcher result is labelled "Users have reported…" | K 12 explanation (kb.py 51); impact text "about +20% 1% lows reported in Overwatch on a 9900K" (kb_impact.py 57) | Rule 7: it must be labelled as **his** measurement on a different CPU | Basis `owner-measured` (9900K, Overwatch, 1% lows), and say it is a different CPU. |
| 29 | Inconsistent microcode versions | `SAFE14` says "0x12B or newer" (kb.py 13); the TVB claim says "0x125 and newer" (kb_impact.py 123) | Rule 7 | kb-auditor to settle with `published` URLs. |
| 30 | The UI never shows a basis, and the DB has no field for one | `K` line format | Rule 7: "The UI shows the basis" | Add basis fields per claim and number to the KB source, `gen.py`'s output and the detail pane. The generator must fail when a basis is missing. |

### Numbers in KB text without a basis (rule 7)

No KB entry, in any tier, carries a `basis`; the format has no field for one.

The count below comes from a heuristic regex over all `K` text fields in `settings.db`. It counts numbers followed by `%`, ms, µs, ns, W, V, A, mV, MHz, GHz, KB/MB/GB, °C, `x`, fps or cycles, with "1% lows"/"0.1% lows" excluded as metric names. It finds **71 quantity mentions in 37 of 213 KB entries**:

| | Mentions | Entries |
|---|---|---|
| Tier 1 | 37 | 20 of 56 |
| Tier 2 | 34 | 17 of 157 |

Qualitative quantities ("a few percent") and spec-style numbers (DDR5-4800, 0x12B) are **not** counted, so kb-auditor's full count will be higher.

Five examples:
1. K 0 `Hyper-Threading`: "Adds roughly 10-30% throughput in heavily multi-threaded work" (kb.py 17; also kb_impact.py 15).
2. K 12 `Hardware Prefetcher`: "about +20% in Overwatch on a 9900K" (kb.py 51; kb_impact.py 57). This one is also mislabelled; see #28.
3. K 37 `XMP`: "often 5-15% vs the JEDEC default" in CPU-bound 1% lows (kb_impact.py 144; the same claim is in kb2.py 108).
4. K 4 `Intel Speed Shift (HWP)`: "ramps up in about 1 ms instead of ~30 ms" (kb.py 32).
5. K 172 `PCIe link speed` (tier 2): "Gen3 vs Gen4 is usually 1-3% on fast GPUs" (kb2.py 366).

Others worth flagging:
- `IA ICCMax` "307 A" (K 32);
- Resizable BAR "up to about 10%" (kb_impact.py 156);
- Turbo Boost Max 3.0 "about 100 MHz" on a 14700K (kb_impact.py 23-24);
- CPU C-States "about 1.4-1.5 V and up" (kb_impact.py 215).

---

## Contradictions with docs/HISTORY.md

| HISTORY claim | Finding |
|---|---|
| 5,368 settings | Confirmed: 5,368 `S` lines. |
| "about 57 in-depth entries" | **56.** `T` = 56; `kb.py` `KB` has 56 entries. |
| covering 501 settings | Confirmed: 501 settings have KB index < 56. |
| Tier 2 covers all 5,368 | Confirmed: 2,964 by name and 1,903 by menu. FALLBACK matches 0. |
| 150 variables in the owner's dump, all varstore sizes match | **Can't verify:** the dump isn't in the repo, and the app doesn't record sizes. The map references only 20 (var, guid) pairs. |
| 563 modules | Can't verify: the image isn't in the repo. |
| IFR defaults wrong for about 95 settings | Can't verify: only the final default is shipped. |
| Filter "Hidden" | The label is `Hidden in menus`. |
| Admin should be `Admin::Optional` + asInvoker | Conflicts with the current CLAUDE.md rule 4 (every exe `requireAdministrator`). |
| The startup MessageBox comes before the window | The MessageBox comes after `ShowWindow`. |
