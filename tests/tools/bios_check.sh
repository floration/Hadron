#!/bin/bash
# BIOS module acceptance run under Wine (docs/design/bios.md "Acceptance").
#   tests/tools/bios_check.sh <build-dir> <out-dir>
# 1. Builds the make_bios_snapshot.py fixture in a fresh temp folder from the embedded settings.db and
#    zips it (Python zipfile, deflated) so both snapshot sources are exercised. Deletes nothing.
# 2. --selftest standalone (folder fixture) and in the suite (--snapshot zip).
# 3. Screenshots, one xvfb-run each: Baseline/Console x dark/light from the folder fixture, a non-solo
#    suite shot from the zip, the empty state, a goal shot (Goal column colours) and a detail-pane shot
#    of a sourced entry (--select: Basis row and "Sources for numbers"), and the guarded-write plan
#    overlay and value editor over a MockBackend (--write-demo plan|edit).
set -euo pipefail
BUILD="${1:?build dir}"; OUT="${2:?output dir}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$OUT"
FIX="$(mktemp -d)"
python3 -I "$HERE/tests/fixtures/make_bios_snapshot.py" "$HERE/modules/bios/data/settings.db" "$FIX/snap" > /dev/null
python3 -I - "$FIX/snap" "$FIX/snap.zip" <<'EOF'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in files:
            if f != "expected.json":
                p = os.path.join(root, f)
                z.write(p, os.path.relpath(p, src))
EOF
export WINEDEBUG=-all
# Ubuntu's wine64 package doesn't put wineserver on PATH; find it, or skip the wait.
# Waiting matters: a shot started while the previous (display-less) wineserver is still alive
# attaches to it and gets no window.
WS="$(command -v wineserver || command -v wineserver64 || find /usr/lib /usr/libexec /opt -name 'wineserver*' -type f 2>/dev/null | head -1 || true)"
wswait() { if [ -n "$WS" ]; then "$WS" -w || true; else while pgrep -x 'wineserver(64)?' > /dev/null 2>&1 || pgrep -f '[w]ineserver' > /dev/null 2>&1; do sleep 1; done; fi; }

echo "== selftest (standalone, folder fixture)"
wine "$BUILD/HadronBios.exe" --selftest --fixture "$FIX/snap"
echo "== selftest (suite, zip fixture)"
wine "$BUILD/Hadron.exe" --solo bios --selftest --snapshot "$FIX/snap.zip" | tail -1
wswait

shot() {   # name exe args...
    local name="$1"; shift
    xvfb-run -a -s "-screen 0 1600x1000x24 -dpi 96" wine "$@" --screenshot "$OUT/$name.png" > "$FIX/$name.log" 2>&1 || true
    wswait
    if [ -s "$OUT/$name.png" ]; then echo "wrote $OUT/$name.png"; else echo "MISSING $OUT/$name.png"; cat "$FIX/$name.log"; exit 1; fi
}
for th in dark light; do
    for ly in baseline threepane console; do
        shot "bios-$ly-$th" "$BUILD/HadronBios.exe" --fixture "$FIX/snap" --theme $th --layout $ly
    done
done
shot bios-suite-baseline "$BUILD/Hadron.exe" --module bios --snapshot "$FIX/snap.zip" --layout baseline
shot bios-suite-console "$BUILD/Hadron.exe" --module bios --snapshot "$FIX/snap" --layout console
shot bios-suite-threepane "$BUILD/Hadron.exe" --module bios --snapshot "$FIX/snap" --layout threepane
shot bios-empty-dark "$BUILD/HadronBios.exe"
shot bios-goal-latency-dark "$BUILD/HadronBios.exe" --fixture "$FIX/snap" --goal 5
shot bios-detail-iccmax-dark "$BUILD/HadronBios.exe" --fixture "$FIX/snap" --goal 5 --select "IA ICC Max Current Limit Override"
# Guarded writes (docs/design/bios-write.md): --write-demo drives a MockBackend built from the fixture;
# nothing real is read or written (the --screenshot latch blocks the real backend anyway).
shot bios-write-plan "$BUILD/HadronBios.exe" --fixture "$FIX/snap" --write-demo plan
shot bios-write-edit-dark "$BUILD/HadronBios.exe" --fixture "$FIX/snap" --write-demo edit
echo "fixture left at $FIX (temp; not deleted by this script)"
