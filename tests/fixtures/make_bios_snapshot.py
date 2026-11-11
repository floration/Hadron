#!/usr/bin/env python3
"""Build a synthetic BIOS snapshot in the legacy UEFI-dump layout (index.csv + vars/) from the
defaults in settings.db, then change a few settings to known values. No real NVRAM is involved.

Usage: python3 -I make_bios_snapshot.py <settings.db> <empty-target-dir>
Writes <target>/index.csv, <target>/vars/*.bin and <target>/expected.json.
Note: varstore sizes are the highest offset+width the map uses, not the real NVRAM sizes.
"""
import csv, json, os, sys

# (prompt, menu-path prefix or None, option text or integer value). Mirrors the owner's
# deliberate low-latency setup described in docs/HISTORY.md.
CHANGES = [
    ("Hyper-Threading", None, "Disabled"),
    ("Intel Turbo Boost", None, "Disabled"),
    ("Intel Speed Shift Technology", None, "Disabled"),
    ("EIST", None, "Disabled"),
    ("CSM Support", None, "Enabled"),
    ("Re-Size BAR Support", None, "Disabled"),
    ("Above 4G memory/Crypto Currency mining", None, "Disabled"),
    ("Hardware Prefetcher", None, "Disabled"),
    ("Adjacent Cache Line Prefetch", None, "Disabled"),
]


def unesc(s):
    return s.replace("\\n", "\n").replace("\\t", "\t").replace("\\\\", "\\")


def load(db):
    settings = []
    with open(db, encoding="utf-8") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if p[0] != "S" or len(p) < 17:
                continue
            opts = []
            if p[15]:
                for o in p[15].split("\x1e"):
                    kv = o.split("\x1f")
                    if len(kv) == 2:
                        opts.append((int(kv[0]), unesc(kv[1])))
            settings.append(dict(id=int(p[1]), var=p[2], guid=p[3], off=int(p[4]), width=int(p[5]),
                                 default=int(p[8]) if p[8] else None, prompt=unesc(p[12]),
                                 path=unesc(p[14]), opts=opts))
    return settings


def put(buf, off, width, value):
    for i in range(width):
        buf[off + i] = (value >> (8 * i)) & 0xFF


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    db, out = sys.argv[1], os.path.abspath(sys.argv[2])
    if os.path.exists(out) and os.listdir(out):
        sys.exit("target is not empty: " + out)
    settings = load(db)
    stores = {}
    for s in settings:
        key = (s["var"], s["guid"])
        stores[key] = max(stores.get(key, 0), s["off"] + s["width"])
    bufs = {k: bytearray(n) for k, n in stores.items()}
    for s in settings:
        if s["default"] is not None:
            put(bufs[(s["var"], s["guid"])], s["off"], s["width"], s["default"])

    applied = []
    for prompt, path, want in CHANGES:
        hits = [s for s in settings if s["prompt"] == prompt and (path is None or s["path"].startswith(path))]
        for s in hits:
            val = want if isinstance(want, int) else next((v for v, t in s["opts"] if t == want), None)
            if val is None:
                continue
            put(bufs[(s["var"], s["guid"])], s["off"], s["width"], val)
            applied.append(dict(id=s["id"], prompt=prompt, path=s["path"], value=val, default=s["default"]))
    if not applied:
        sys.exit("no change matched; check prompts against settings.db")

    os.makedirs(os.path.join(out, "vars"))
    rows = []
    for (var, guid), buf in sorted(bufs.items()):
        safe = "".join(c if c.isalnum() or c in "_-." else "_" for c in var)
        name = "%s__%s.bin" % (safe, guid)
        with open(os.path.join(out, "vars", name), "wb") as f:
            f.write(buf)
        rows.append([var, "{%s}" % guid, str(len(buf)), "0x7", "", name])
    # Same shape as Dump-UEFI.ps1: Export-Csv -NoTypeInformation -Encoding UTF8 (BOM, all quoted, CRLF).
    with open(os.path.join(out, "index.csv"), "w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f, quoting=csv.QUOTE_ALL, lineterminator="\r\n")
        w.writerow(["Name", "Guid", "Size", "Attributes", "Error", "File"])
        w.writerows(rows)
    exp = dict(variables=len(rows), settings=len(settings), changed=applied,
               unmatched=[c[0] for c in CHANGES if c[0] not in {a["prompt"] for a in applied}])
    with open(os.path.join(out, "expected.json"), "w") as f:
        json.dump(exp, f, indent=2)
    print(json.dumps(dict(variables=len(rows), changed=len(applied), unmatched=exp["unmatched"]), indent=2))


if __name__ == "__main__":
    main()
