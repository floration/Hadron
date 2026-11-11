#!/bin/bash
# Services module acceptance run under Wine (docs/design/services.md "Acceptance").
#   tests/tools/services_check.sh <build-dir> <out-dir>
# 1. KB: genkb selftest, then a regenerated services.db must equal the committed one byte for byte.
# 2. Fixture: make_services_fixture.py into a temp folder; every file must equal the committed copy.
# 3. --selftest standalone and in the suite (fake backend, temp data dirs; the live part only enumerates).
# 4. --dump-plan for every tab x level must equal tests/golden/services/plan-<tab>-<level>.txt.
# 5. Screenshots, one xvfb-run each. Nothing here touches a real service: fixture mode never applies.
set -euo pipefail
BUILD="${1:?build dir}"; OUT="${2:?output dir}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$OUT"
TMP="$(mktemp -d)"

echo "== KB generator"
python3 -I "$HERE/tools/services/genkb.py" --selftest
python3 -I "$HERE/tools/services/genkb.py" "$TMP/services.db"
cmp "$TMP/services.db" "$HERE/modules/services/data/services.db"
echo "ok: services.db is what genkb.py generates"

echo "== fixture"
python3 -I "$HERE/tests/fixtures/services/make_services_fixture.py" "$TMP/fix" > /dev/null
for f in snapshot.txt hw.txt first.txt interrupted/snapshot.txt interrupted/hw.txt interrupted/first.txt \
         interrupted/undo-20261110-213012.log delayedlinger/snapshot.txt delayedlinger/hw.txt delayedlinger/first.txt \
         forged/snapshot.txt forged/hw.txt forged/first.txt forged/undo-20261110-210000.log \
         multi/snapshot.txt multi/hw.txt multi/first.txt multi/undo-20261110-210000.log multi/undo-20261110-220000.log; do
    cmp "$TMP/fix/$f" "$HERE/tests/fixtures/services/$f"
done
echo "ok: the committed fixture is what make_services_fixture.py writes"
FIX="$HERE/tests/fixtures/services"

export WINEDEBUG=-all
# See bios_check.sh: wait for the previous wineserver so each shot gets its own display.
WS="$(command -v wineserver || command -v wineserver64 || find /usr/lib /usr/libexec /opt -name 'wineserver*' -type f 2>/dev/null | head -1 || true)"
wswait() { if [ -n "$WS" ]; then "$WS" -w || true; else while pgrep -f '[w]ineserver' > /dev/null 2>&1; do sleep 1; done; fi; }

echo "== selftest (standalone)"
wine "$BUILD/HadronServices.exe" --selftest --fixture "$FIX" | tail -3
echo "== selftest (suite)"
wine "$BUILD/Hadron.exe" --solo services --selftest --fixture "$FIX" | tail -1
wswait

echo "== golden plans"
for tab in services drivers; do
    for lvl in default bloat stripped minimum; do
        wine "$BUILD/HadronServices.exe" --dump-plan "$lvl" --tab "$tab" --fixture "$FIX" --out "$TMP/plan-$tab-$lvl.txt"
        cmp "$TMP/plan-$tab-$lvl.txt" "$HERE/tests/golden/services/plan-$tab-$lvl.txt"
    done
done
# A7 regression: a lingering DelayedAutostart=1 under Manual, restored to plain Auto.
wine "$BUILD/HadronServices.exe" --dump-plan default --tab services --fixture "$FIX/delayedlinger" --out "$TMP/plan-delayedlinger.txt"
cmp "$TMP/plan-delayedlinger.txt" "$HERE/tests/golden/services/plan-services-default-delayedlinger.txt"
echo "ok: 8 plans and the delayedlinger plan match tests/golden/services"
wswait

shot() {   # name exe args...
    local name="$1"; shift
    xvfb-run -a -s "-screen 0 1600x1000x24 -dpi 96" wine "$@" --screenshot "$OUT/$name.png" > "$TMP/$name.log" 2>&1 || true
    wswait
    if [ -s "$OUT/$name.png" ]; then echo "wrote $OUT/$name.png"; else echo "MISSING $OUT/$name.png"; cat "$TMP/$name.log"; exit 1; fi
}
S="$BUILD/HadronServices.exe"
shot services-list "$S" --fixture "$FIX"
shot services-drivers "$S" --fixture "$FIX" --tab drivers
shot services-detail-kb "$S" --fixture "$FIX" --select DiagTrack
shot services-detail-protected "$S" --fixture "$FIX" --select RpcSs
shot services-plan-stripped "$S" --fixture "$FIX" --plan stripped
shot services-interrupted "$S" --fixture "$FIX/interrupted"
shot services-forged-undo "$S" --fixture "$FIX/forged" --plan undo-last
shot services-multi "$S" --fixture "$FIX/multi"
shot services-ti-on "$S" --fixture "$FIX" --ti on --plan stripped --select dmwappushservice
shot services-ti-list "$S" --fixture "$FIX" --ti on --select dmwappushservice
shot services-light "$S" --fixture "$FIX" --theme light --select WSearch
shot services-suite "$BUILD/Hadron.exe" --module services --fixture "$FIX"
echo "temp files left at $TMP (not deleted by this script)"
