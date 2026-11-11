# Services knowledge base

The KB has three files:
- `kb.py` is the source.
- `genkb.py` is the generator, and the hard-rule-7 gate.
- `modules/services/data/services.db` is the output. It is embedded in the exe as RCDATA `SERVICES_KB`.

Commands:
```
python3 tools/services/genkb.py --selftest          # 38 must-fail cases, 15 ok texts, a warning case, the committed KB
python3 tools/services/genkb.py                     # regenerates modules/services/data/services.db
python3 tools/services/genkb.py /tmp/services.db    # writes elsewhere (CI then runs cmp against the committed file)
```
`genkb.py` loads `tools/bios/genkb.py` by path and reuses its checks: basis kinds, `@KEY` expansion, the
"a number needs an owner-measured or published basis" rule, and escaping. The bios tool is not modified.

`genkb.py` prints `genkb: warning: ...` lines on stderr for vague quantities (the bios soft list: "twice", "tens
of", "N times faster", ...). Warnings never fail the build; read them and either reword or give the claim a basis.

## Writing an entry (`kb.py`)
Use `E(name, kind, title, expl, ...)`. For explanation-only entries (never in a preset) there are three helpers:
- `keep(name, title, expl, why, ...)`: Hadron decided to keep it, and `why` (shown as "If it's off") says why.
- `info(name, title, expl, if_off, iot_r, ws_r, ...)`: a service with the two pages' ratings (`OK`, `DND`, `NG`,
  `AD`); the basis note is built from them. Ratings are quoted with each page's label: @IOT says "Don't disable",
  @WS16 says "Do not disable".
- `drv(name, title, expl, if_off, basis, note)`: a driver. The basis is `reasoning` unless a cited page names the
  driver and says what the text says.

Explanation-only entries set `never`, so the guard gives the same result as having no entry (not in presets); only
the reason text changes. In text, say where a claim comes from: `@KEY says ...` for a page, "(reasoning)" for
Hadron's own inference.

| Field | Meaning |
|---|---|
| `kind` | `svc` or `drv` |
| `title`, `expl`, `if_off`, `test` | Plain text. `test` says how to check the effect yourself (rule 7: "test it") |
| `basis`, `basis_note` | Basis for the explanation: `owner-measured` (conditions), `published` (an `@KEY` source), `reasoning` or `folklore` |
| `levels` | `{'bloat'\|'stripped'\|'minimum': (target, why)}`. The target is 3 (Manual) or 4 (Disabled). Membership is cumulative: an item in `bloat` is also in Stripped and Minimum unless those levels name a different target |
| `pbasis`, `pbasis_note` | Basis for the preset membership. Required whenever `levels` is set |
| `sysok` | Required for a driver to be in any level |
| `never` | Explanation only: never in a preset. It can't be combined with `levels` |
| `hw` | Hardware hint: `class:<Class>`, `svc:<service>`, `self`, `vmguest` or `hvhost`. Hardware that is present or was seen before keeps the item |
| `contested` | Must name sources on both sides (two or more `@KEY`s). Contested items can't be in `bloat` |
| `stock`, `stock_basis`, `stock_note` | The Windows stock value (`"2/1"`, `"3/-"` or an int). Needs an `owner-measured` or `published` basis. Never set on storage drivers |
| `builds` | Five-digit builds the entry is for. The default is `BUILD` (19045) |

Sources:
- They are the `SOURCES` keys. Each carries its `r3 [n]` id from
  `docs/pipeline/svcdrv/research/r3-documentation.md`.
- Don't add a URL that isn't in r3 without reading the page first. The driver pages (`HIDKBM`, `USBHOST`,
  `USBCLASSES`, `STORPORT`, `STORNVME`, `STORAHCI`, `FLTMGR`, `BASICDISPLAY`, `USBAUDIO2`) were added in Finalize
  and read on 2026-11-11; `docs/pipeline/svcdrv/finalize/kb.md` lists what each one says.
- In text, `@KEY` becomes the source's label. In basis and contested notes it becomes "label (url)".
- Every note says which OS the source was written for (IoT Enterprise, Server 2016, VDI), because none of them
  is about a gaming desktop.

Numbers:
- No numbers about savings: no RAM, CPU, FPS or latency figures.
- Any number in the text needs an `owner-measured` or `published` basis. The gate rejects it otherwise. On top of
  the bios unit check (`2 s`, `120 MB`, `5%`) it rejects:
  - counts: a whole number or a number word before processes, services, drivers, threads, handles, instances,
    tasks or svchosts (`40 processes`, `thirty services`);
  - sizes and durations in words (`200 megabytes`, `two seconds`, `ten percent`);
  - fractions (`half`, `a third`, `a quarter`).
- Not counted: version and build numbers (`USB Audio 2.0 driver`, `Windows Server 2016 services`, `Windows 10/11`),
  and "one" on its own ("one instance per user").
- A number with a basis is marked `[[shown|published|@KEY]]`, as in the bios KB.
- The KB needs at least 60 entries; there is no upper bound.

## services.db format
- UTF-8, LF line ends, TAB-separated fields, bios escaping.
- `V\t1\tservices`, then `B\t<build>`.
- One `K` line per entry, 26 fields:
```
K name kind builds title expl if_off test
  lvBloat lvStripped lvMinimum            target 3|4 or empty
  whyBloat whyStripped whyMinimum
  sysok never basis basisNote pbasis pbasisNote
  stock stockBasis stockNote hw contested sources
```
- `basisNote`, `pbasisNote`, `stockNote` and `contested` carry expanded "label (url)" text. The module shows them
  as written.
