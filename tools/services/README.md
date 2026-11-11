# Services knowledge base

The KB has three files:
- `kb.py` is the source.
- `genkb.py` is the generator, and the hard-rule-7 gate.
- `modules/services/data/services.db` is the output. It is embedded in the exe as RCDATA `SERVICES_KB`.

Commands:
```
python3 tools/services/genkb.py --selftest          # 70 must-fail cases, 17 ok texts, 3 marker cases, a warning case, the committed KB
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
| `stock_disagree` | Two third-party tables that disagree: the text (two `@KEY`s, the word "disagree"). Shown, never applied; can't be combined with `stock` |
| `builds` | Five-digit builds the entry is for. The default is `BUILD` (19045) |

The Windows stock table:
- It is `STOCK` at the end of `kb.py`: `(name, "start/delayed", note)` rows that fill `stock`, `stock_basis`
  (`published`) and `stock_note` on the entry of that name. Each row needs an entry (the file asserts it).
- A row goes in only when a Microsoft page for Windows client states that service's default or expected start
  type. The note quotes it and says what the page applies to. "Automatic" without "(Delayed Start)" is written
  `2/0` (the plan then clears a lingering DelayedAutostart), so a page that doesn't say which one needs a
  "(reasoning)" clause in the note.
- Third-party tables (fix/svcdrv-stock, DECISIONS 2026-11-11, owner): `TABLE_WINTIPS` and `TABLE_TWEAKHOUND` at
  the end of `kb.py` are two independent captures of a Windows 10 service list. A service with no Microsoft value
  gets one only where both tables give the same start type ("Trigger Start" ignored); where they differ the entry
  gets `stock_disagree` and no value. Drivers: neither table lists any, so none.
- Each K line carries `stock_src`: `microsoft` (every cited page is on learn.microsoft.com), `table` (any other
  page), `disagree`, or empty. The module words a value by it: "from a Microsoft page (it names no build)" or "from
  two agreeing third-party tables (they name no build)".
- genkb fails a `published` stock note that doesn't say it "names no build"; one that cites a non-Microsoft page
  and doesn't say "third-party table" or cites fewer than two sources; a `stock_disagree` with fewer than two
  sources, without "disagree", or next to a `stock` value. A driver with a stock value needs `sysok`.
- On a PC the module's first source is the PC's own WinSxS component manifests
  (`modules/services/sxsstock.cpp`); the KB value is its cross-check. See docs/design/services.md.
- Not usable as client stock: the `@IOT` and `@PERUSER` "Startup Type (Default)" columns ("the current version of
  Windows"; they disagree with each other), `@WS16` (Server 2016) and BITS (its page gives a start type that
  depends on queued jobs). `docs/pipeline/fixes/svcdrv-kb.md` has the details.
- Build numbers are not on the digit allow-list, so notes say "the Windows 10 stock value", not the build.

Sources:
- They are the `SOURCES` keys. Each carries its `r3 [n]` id from
  `docs/pipeline/svcdrv/research/r3-documentation.md`.
- Don't add a URL that isn't in r3 without reading the page first. The driver pages (`HIDKBM`, `USBHOST`,
  `USBCLASSES`, `STORPORT`, `STORNVME`, `STORAHCI`, `FLTMGR`, `BASICDISPLAY`, `USBAUDIO2`) were added in Finalize
  and read on 2026-11-11; `docs/pipeline/svcdrv/finalize/kb.md` lists what each one says. The stock pages
  (`WUERRCLIENT`, `W32TWG`, `TASKSCHED`, `SEARCHPERF`), the driver pages `UVC`, `USBPRINT`, `RDBSS`, `LLTDCSP`,
  `HVNET` and the OEM pages (`MSIBOARD`, `INTELLMS`, `INTELDAL`, `FN*`) were added in fix/svcdrv-kb;
  `docs/pipeline/fixes/svcdrv-kb.md` lists what each says.
- OEM entries are explanation-only (third-party items are never in a preset). The `FN*` sources are file.net, a
  third-party process database (not the vendor; its pages mix its own write-ups with user ratings): cite them for a service's name and folder only, never for what
  it does, and keep the basis `reasoning`.
- In text, `@KEY` becomes the source's label. In basis and contested notes it becomes "label (url)".
- Every note says which OS the source was written for (IoT Enterprise, Server 2016, VDI), because none of them
  is about a gaming desktop.

Numbers:
- No numbers about savings: no RAM, CPU, FPS or latency figures.
- Any number in the text needs an `owner-measured` or `published` basis. The gate rejects it otherwise. On top of
  the bios unit check (`2 s`, `120 MB`, `5%`), every digit fails unless it is:
  - inside a `[[shown|kind|@KEY]]` marker;
  - part of an identifier that starts with a letter (`IPv6`, `usbaudio2`, `Wdf01000`, `dot1x`);
  - on the allow-list `DIGIT_ALLOW` in `genkb.py`: `Windows 10`, `Windows 10/11`, `Server 2016`, `20H2`, `1607`,
    `1703`, `1903`, `2004`, `0x...` codes, `USB 2`/`USB 3.0`/`USB Audio 2.0`, `PS/2`, `class 1`,
    `subclass N, protocol N`, `32-bit`/`64-bit`, `6to4`, `Start value must be 0`.

  A new identifier with digits goes on the allow-list in the same change, with a selftest ok text. Number words
  can't be listed that way, so the gate also rejects counts and units in words (`forty processes`, `two seconds`,
  `one second`, `ten percent`), any size word (`a gigabyte`) and fractions (`half`, `a third`, `a quarter`).
  "one" on its own is not a count ("one instance per user"); write instructions in words ("restart three times").
- A number with a basis is marked `[[shown|published|@KEY]]`, as in the bios KB. The db gets the shown text (the
  module never renders markers); genkb fails if `[[` or `]]` would reach a K line.
- The KB needs at least 60 entries; there is no upper bound.

## services.db format
- UTF-8, LF line ends, TAB-separated fields, bios escaping.
- `V\t1\tservices`, then `B\t<build>`.
- One `K` line per entry, 27 fields (the module still reads 26-field lines, without `stock_src`):
```
K name kind builds title expl if_off test
  lvBloat lvStripped lvMinimum            target 3|4 or empty
  whyBloat whyStripped whyMinimum
  sysok never basis basisNote pbasis pbasisNote
  stock stockBasis stockNote hw contested sources stock_src
```
- `stockNote` holds the `stock_disagree` text on a `disagree` line (stock and stockBasis empty).
- `basisNote`, `pbasisNote`, `stockNote` and `contested` carry expanded "label (url)" text. The module shows them
  as written.
