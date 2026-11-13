#!/bin/bash
# DiskMap acceptance run under Wine (docs/design/diskmap.md "Acceptance").
#   tests/tools/diskmap_check.sh <build-dir> <out-dir>
# 1. Builds the make_diskmap_tree.py fixture in a fresh temp folder (the generator refuses to
#    overwrite, and this script deletes nothing).
# 2. --selftest against that fixture, standalone and in the suite: asserts 17,992,005 B with the
#    hard link counted once and the 10 MB sentinel behind the outside symlink excluded.
# 3. --screenshot of all three layouts (Baseline, Three-Pane, Console) in both themes, standalone, plus
#    one suite shot per layout (the module under the host's content offset and chrome), one xvfb-run
#    each, since several Wine runs inside one Xvfb session are flaky.
set -euo pipefail
BUILD="${1:?build dir}"; OUT="${2:?output dir}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$OUT"
FIX="$(mktemp -d)"
python3 -I "$HERE/tests/fixtures/make_diskmap_tree.py" "$FIX/dm" > /dev/null
TREE="$FIX/dm/tree"
export WINEDEBUG=-all

echo "== selftest (standalone)"
wine "$BUILD/HadronDiskMap.exe" --selftest --fixture "$TREE"
echo "== selftest (suite)"
wine "$BUILD/Hadron.exe" --solo diskmap --selftest --fixture "$TREE" | tail -1
# The selftests ran with no display; a screenshot that joins that wineserver session inherits its
# display-less desktop and cannot create a window. Wait for the session to end first.
wineserver -w

shot() {   # name exe args...
    local name="$1"; shift
    xvfb-run -a -s "-screen 0 1600x1000x24 -dpi 96" \
        wine "$@" --screenshot "$OUT/$name.png" --fixture "$TREE" > /dev/null 2>&1 || true
    wineserver -w
    if [ -s "$OUT/$name.png" ]; then echo "wrote $OUT/$name.png"; else echo "MISSING $OUT/$name.png"; exit 1; fi
}
for th in dark light; do
    for ly in baseline threepane console; do
        shot "diskmap-$ly-$th" "$BUILD/HadronDiskMap.exe" --theme "$th" --layout "$ly"
    done
done
for ly in baseline threepane console; do
    shot "diskmap-suite-$ly" "$BUILD/Hadron.exe" --module diskmap --layout "$ly"
done
echo "fixture left at $FIX (temp; not deleted by this script)"
