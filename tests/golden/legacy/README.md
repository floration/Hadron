# Legacy baselines (Phase 0)

Recorded 2026-11-11 from the unchanged legacy tools, built with `tests/tools/build_legacy.sh`
(MinGW-w64 GCC 13, win32 thread model) and run under Wine 9.0 + Xvfb at 96 DPI, 1600x1000.
The Wine prefix had `z:` removed; fixtures were mapped as drives. Wine has no Segoe UI, so judge
layout and values, not font metrics.

| File | How it was made | What a port must match |
|---|---|---|
| `legacy-diskmap.png` | `DiskMap.exe D` with `D:` = `make_diskmap_tree.py` output `tree/`, captured after 25 s | Layout, categories, totals |
| `diskmap-tree-expected.json` | written by `make_diskmap_tree.py` | Legacy shows **17 files, 17.3 MB** = `bytes_per_link` (18,112,005 B): the hard link is counted twice and the 10 MB sentinel behind the outside symlink is not counted. No junction under Linux/Wine; the junction case needs the Windows CI job |
| `legacy-bios.png` | `BiosSettingsExplorer.exe E:\` with `E:` = `make_bios_snapshot.py` output, after 15 s | Layout, decoded values, changed-from-default colouring |
| `bios-snapshot-expected.json` | written by `make_bios_snapshot.py` | The 9 changed settings show their new value in orange; all others equal the default |

Not yet recorded: the "Export changes" checklist (legacy writes it next to the exe and needs a
click; no input automation in the sandbox yet). DiskMap's "Not reachable" figure reflects the host
disk under Wine and is not part of the golden.
