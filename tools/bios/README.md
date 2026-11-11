# tools/bios: BIOS knowledge base and settings.db generator

Build tools for the `bios` module (Python 3, standard library only; rule 3 allows build tools).

| File | What it is |
|---|---|
| `kb.py` | The knowledge base: one `E(...)` record per entry. Tier 1 (`T1`, 56), then tier 2 (`NAME` 130, `MENU` 26, `FALLBACK` 1). Port of legacy `kb.py` + `kb_impact.py` + `kb2.py` with the `IMPACT`/`EXPL`/`REC`/`NOTE` overlays already applied and the rule-7 fixes made. **Edit this file** to change KB text. |
| `genkb.py` | Builds `modules/bios/data/settings.db` (v2) and enforces rule 7. |

## Regenerate

```
python3 tools/bios/genkb.py legacy/bios-explorer/app/settings.db modules/bios/data/settings.db
```

Commit the regenerated `settings.db` with the `kb.py` change: it is the shipped data, embedded as RCDATA
`BIOS_SETTINGS`. Every KB diff goes to `kb-auditor` before merge.

The run prints the K counts, the number of settings matched to tier 1 / NAME / MENU / FALLBACK, and how many settings'
KB index differs from the legacy file. With the regexes unchanged from legacy the counts are **501 / 2,964 / 1,903 / 0**
and the difference is 0. If you change a regex or the entry order on purpose, say so in the commit message.

## settings.db v2 line format

UTF-8 without BOM, LF line ends, trailing newline, one record per line, fields separated by TAB. Text fields are
escaped as in legacy `gen.py`: `\` becomes `\\`, a TAB becomes a space, CR is dropped, LF becomes `\n`. The reader's
`Unesc` turns `\n` into a newline and `\x` into `x`.

| Line | Fields (0-based) |
|---|---|
| `V` | 1 title (copied from the legacy file) |
| `B` | 1 SMBIOS board product (`MS-7E06`), 2 BIOS version (`7E06vAF`): the version guard |
| `K` | 1 index, 2 title, 3 explanation, 4-8 recs (power, balanced, single, multi, latency), 9 note, 10-13 impact (FPS/esports, other games, apps & work, claim check), **14 basis**, **15 basis_note**, **16 sources** |
| `T` | 1 tier-1 count (written between the tier-1 and tier-2 `K` lines) |
| `S` | the 17 v1 fields, copied verbatim from the legacy file except field 16 (KB index), which is re-matched |

So a v2 `K` line has 17 fields: the 14 v1 fields followed by basis, basis_note and sources (the 15th, 16th and 17th
fields). A v1 reader that reads fields 0-13 is unaffected.

- `basis`: `owner-measured` | `published` | `reasoning` | `folklore`.
- `basis_note`: the conditions for owner-measured, the URL(s) for published (space-separated), one line otherwise.
- `sources`: one item per marked number in the entry's text, items joined by `0x1E`; each item is
  `text 0x1F kind 0x1F ref` (kind `owner-measured` or `published`; ref = conditions or URL(s), space-separated).
  Empty when the entry has no marked numbers. `text` and `ref` are escaped.

Order: `V`, `B`, tier-1 `K` lines, `T`, tier-2 `K` lines (NAME, MENU, FALLBACK), `S` lines.

KB matching (legacy `gen.py` order): the first tier-1 regex that `search`es the prompt; else the first NAME regex on
the stripped prompt; else the first MENU regex on the setting's menu paths joined with ` | ` (from `S` field 14, which
keeps at most 3 paths); else FALLBACK. Regexes are case-insensitive.

## Basis rules (CLAUDE.md rule 7)

- **Every tier-1 entry has a basis.** Tier-2 entries carry one too. An entry whose text is mostly reasoning
  with a sourced number is `reasoning` and keeps the number's marker; today only the Hardware Prefetcher entry is
  `owner-measured` and every other entry is `reasoning`.
- **Every number in user-facing text** (title, explanation, recs, note, impact) and in `basis_note` is written
  `[[text|kind|ref]]`. `text` contains a digit and is at most 40 characters. `ref` is one or more `@KEY` names
  (space-separated) from `kb.SOURCES`; genkb expands them in the output:
  - `owner-measured`: the key starts with `OWNER` and its value is the conditions (CPU, game, metric), e.g. the
    9900K prefetcher result;
  - `published`: the key's value is an `https://` URL you actually fetched and read that supports the exact figure.
  `kb.SOURCES` has a comment per key saying what the page states (and whether it is a secondary source).
- `basis_note` names its sources the same way (`@KEY`): an owner-measured basis must name an `@OWNER*` key, a
  published basis an https key. genkb expands the keys, so the shipped basis_note is readable text or URLs.
- A number you cannot source is **rewritten without the number**: say it qualitatively, and for workload-dependent
  effects say "test it" and how (e.g. "compare 1% lows with a frame-time logger over a few identical runs").
  Never invent or guess a URL, never cite a page you did not read, never present an estimate as a measurement.
- Text about one particular machine (the owner's) does not belong in the KB; phrase it generally
  ("on CPUs without integrated graphics (Intel F models)").

`genkb.py` exits 1 and names the entry, the field and the text when:
- a tier-1 entry has no basis, or a basis is not one of the four values;
- an `owner-measured` basis names no `@OWNER*` key, or a `published` basis names no https key;
- a marker has another kind, a ref that is not `@KEY`(s), an unknown key, a key of the wrong type, or a text with
  no digit / over 40 characters;
- a number is not inside a marker (text fields and basis_note);
- text, an expanded marker ref or a `SOURCES` value contains an owner-specific phrase (case-insensitive, any
  whitespace including NBSP): `14700 KF`/`14700KF`, "your PC", "your 14700", `53x`, `7E06`, "his earlier/current/own",
  "owner's", "my PC/CPU/board/rig/1xxxx". The approved way to cite the owner's measurement is "Hadron's author".
  The `B` line is not text and is exempt.
- a marker inside `basis_note` breaks the same marker rules as in text (it is rendered, never shipped literally).

It prints **warnings** (not failures) for things worth a second look: "a few percent", "several watts", "halves",
"half", "doubles", "twice", "-fold", "tens of", "an hour", "N times faster/more/…", `DDR5-4800`-style speeds not
preceded by "e.g.", and "gained/improved/lost/increased 1% lows". Reviewed phrases are allow-listed per entry in
`SOFT_ALLOW` (DMI "roughly halves", Gear mode "runs it at half").

`python3 tools/bios/genkb.py --selftest` checks the heuristic on positive and negative cases, the marker and basis
rules, the denylist and the escaping, and exits 1 on any failure. Run it after changing genkb.

### Number heuristic

Case-insensitive. A number (optionally signed, `~`, or a range `a-b` / `a to b`), then any spaces or hyphens, then
one of: `% ‰ percent ms µs μs us ns ps s sec(s) seconds min minutes hr(s) hours day(s) week(s) year(s) nm dB mV mA mW
kW mOhm mΩ kΩ Ω ohm(s) W watts V volts A amps Hz kHz MHz GHz KB MB GB TB KiB MiB GiB TiB bytes GB/s MB/s GT/s MT/s
Gbps Mbps Gbit °C °F C degree(s) x × times fold fps cycles`, not followed by a letter or digit. Also decimal `G` link
speeds such as `2.5G`. A number glued to a preceding letter, digit or `.` is not a quantity (`DDR5`, `M.2`, `0x12B`).

Not flagged, because they are names or identifiers rather than quantity claims:
- the metric names "1% lows" and "0.1% lows" (also "1% and 0.1% lows"); "21% lows" and "+1% lows" are still
  flagged;
- spec-style identifiers: `DDR5-4800`, `Gen4`, `x16`/`x8` lane widths, model numbers (`i7-14700K`, `9900K`),
  microcode ids (`0x12B`), `PL1`/`PL2`, "Gear 2", "TPM 2.0", "Windows 11", "Above 4G" (the setting's name);
- unitless counts and setting values (e.g. "0 = default", "3+ runs", "entered here as 1228"). These are reviewed by
  hand and by kb-auditor; the heuristic does not enforce them.
