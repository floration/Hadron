# modules/bios/data

`settings.db` is embedded as RCDATA `BIOS_SETTINGS` (`modules/bios/resources/bios.rc`). Format: settings.db v2,
see `docs/design/bios.md` "settings.db v2".

**The file here is temporary.** Work package B (`tools/bios/genkb.py`) generates the real v2 file with basis
fields; its output replaces this one. Until then this is the legacy v1 file converted mechanically:

- a `B` line (`MS-7E06`, `7E06vAF`) after `V`;
- three empty fields (15 `basis`, 16 `basis_note`, 17 `sources`) on every `K` line, so every entry shows
  "Basis: not stated";
- `V`, `T`, the `S` lines and every byte of KB text unchanged.

Throwaway converter (run from the repo root, Python 3, stdlib only):

```sh
python3 -I - <<'EOF'
src = open('legacy/bios-explorer/app/settings.db', encoding='utf-8', newline='').read()
out = []
for line in src.split('\n'):
    if line.startswith('K\t'):
        line += '\t\t\t'
    out.append(line)
    if line.startswith('V\t'):
        out.append('B\tMS-7E06\t7E06vAF')
open('modules/bios/data/settings.db', 'w', encoding='utf-8', newline='').write('\n'.join(out))
EOF
```
