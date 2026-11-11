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

- **Every tier-1 entry has a basis.** Tier-2 entries carry one too (all `reasoning` or `published` today).
- **Every number in user-facing text** (title, explanation, recs, note, impact) is written `[[text|kind|ref]]`:
  - `owner-measured`: ref = the conditions (CPU, game, metric), e.g. the 9900K prefetcher result;
  - `published`: ref = an `https://` URL you actually fetched and read that supports the exact figure.
  `kb.py` keeps the URLs in `SOURCES`, with a comment saying what each page states; refs may use `@KEY`.
- A number you cannot source is **rewritten without the number**: say it qualitatively, and for workload-dependent
  effects say "test it" and how (e.g. "compare 1% lows with a frame-time logger over a few identical runs").
  Never invent or guess a URL, never cite a page you did not read, never present an estimate as a measurement.
- Text about one particular machine (the owner's) does not belong in the KB; phrase it generally
  ("on CPUs without integrated graphics (Intel F models)").

`genkb.py` exits 1 and names the entry, the field and the text when:
- a tier-1 entry has no basis, or a basis is not one of the four values;
- an `owner-measured` basis has no conditions, or a `published` basis has no `https://` URL;
- a `[[...]]` marker has another kind, or a published ref is not an `https://` URL;
- a number is not inside a marker.

### Number heuristic

A number (optionally signed, `~`, or a range `a-b` / `a to b`) followed by one of
`% ms µs us ns mV mOhm W V A MHz GHz KB MB GB °C x fps cycles MT/s` (the legacy §7 list plus mOhm and MT/s).

Not flagged, because they are names or identifiers rather than quantity claims:
- the metric names "1% lows" and "0.1% lows" (also "1% and 0.1% lows");
- spec-style identifiers with no unit after the number: `DDR5-4800`, `Gen4`, `x16`/`x8` lane widths, model numbers
  (`i7-14700K`, `9900K`), microcode ids (`0x12B`), `PL1`/`PL2`, "Gear 2", "TPM 2.0", "Windows 11";
- unitless counts and setting values (e.g. "0 = default", "3+ runs"). These are reviewed by hand and by kb-auditor;
  the heuristic does not enforce them.
