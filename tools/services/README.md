# Services knowledge base

The KB has three files:
- `kb.py` is the source.
- `genkb.py` is the generator, and the hard-rule-7 gate.
- `modules/services/data/services.db` is the output. It is embedded in the exe as RCDATA `SERVICES_KB`.

Commands:
```
python3 tools/services/genkb.py --selftest          # 22 must-fail cases, the ok cases, and the committed KB
python3 tools/services/genkb.py                     # regenerates modules/services/data/services.db
python3 tools/services/genkb.py /tmp/services.db    # writes elsewhere (CI then runs cmp against the committed file)
```
`genkb.py` loads `tools/bios/genkb.py` by path and reuses its checks: basis kinds, `@KEY` expansion, the
"a number needs an owner-measured or published basis" rule, and escaping. The bios tool is not modified.

## Writing an entry (`kb.py`)
Use `E(name, kind, title, expl, ...)`, or `keep(...)` for explanation-only entries that are never in a preset.

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
- Don't add a URL that isn't in r3 without reading the page first.
- In text, `@KEY` becomes the source's label. In basis and contested notes it becomes "label (url)".
- Every note says which OS the source was written for (IoT Enterprise, Server 2016, VDI), because none of them
  is about a gaming desktop.

Numbers:
- No numbers about savings: no RAM, CPU, FPS or latency figures.
- Any number in the text needs an `owner-measured` or `published` basis. The gate rejects it otherwise.

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
