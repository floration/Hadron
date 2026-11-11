#!/bin/bash
# DiskMap acceptance run under Wine (docs/design/diskmap.md "Acceptance").
#   tests/tools/diskmap_check.sh <build-dir> <out-dir>
# 1. Builds the make_diskmap_tree.py fixture in a fresh temp folder (the generator refuses to
#    overwrite, and this script deletes nothing).
# 2. --selftest against that fixture, standalone and in the suite: asserts 17,992,005 B with the
#    hard link counted once and the 10 MB sentinel behind the outside symlink excluded.
# 3. --screenshot of Baseline and Console (dark) plus Baseline light, one xvfb-run each, since
#    several Wine runs inside one Xvfb session are flaky.
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

shot() {   # theme layout name
    xvfb-run -a -s "-screen 0 1600x1000x24 -dpi 96" \
        wine "$BUILD/HadronDiskMap.exe" --theme "$1" --layout "$2" --screenshot "$OUT/$3.png" --fixture "$TREE" \
        > /dev/null 2>&1 || true
    wineserver -w
    if [ -s "$OUT/$3.png" ]; then echo "wrote $OUT/$3.png"; else echo "MISSING $OUT/$3.png"; exit 1; fi
}
shot dark baseline diskmap-baseline-dark
shot dark console diskmap-console-dark
shot light baseline diskmap-baseline-light
echo "fixture left at $FIX (temp; not deleted by this script)"
