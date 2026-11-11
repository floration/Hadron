#!/bin/bash
# Services Baseline design renders under Wine (docs/pipeline/svcdrv/design/baseline.md).
#   tests/tools/services_baseline_shots.sh <build-dir> <out-dir> [main|extra|focus|all]
# Fixture mode only: nothing reads or changes a real service. --accent stands in for the Windows accent;
# --focus draws a keyboard focus state, --sort a sorted list, --plan ti-stop the Stop TrustedInstaller plan.
set -uo pipefail
BUILD="${1:?build dir}"; OUT="${2:?output dir}"; ONLY="${3:-all}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
FIX="$HERE/tests/fixtures/services"
mkdir -p "$OUT"
export WINEDEBUG=-all
WS="$(command -v wineserver || command -v wineserver64 || find /usr/lib /usr/libexec /opt -name 'wineserver*' -type f 2>/dev/null | head -1 || true)"
wswait() { if [ -n "$WS" ]; then "$WS" -w || true; else while pgrep -f '[w]ineserver' > /dev/null 2>&1; do sleep 1; done; fi; }
fail=0
shot() {   # name exe args...  (one retry: a shot that joins a dying wineserver gets no window)
    local name="$1"; shift
    for _ in 1 2; do
        xvfb-run -a -s "-screen 0 1600x1000x24 -dpi 96" wine "$@" --screenshot "$OUT/$name.png" > /dev/null 2>&1 || true
        wswait
        [ -s "$OUT/$name.png" ] && { echo "wrote $OUT/$name.png"; return; }
    done
    echo "MISSING $OUT/$name.png"; fail=1
}
S="$BUILD/HadronServices.exe"
A=(--fixture "$FIX" --select DiagTrack --accent 0078D7)
if [ "$ONLY" = all ] || [ "$ONLY" = main ]; then
    for t in light dark fun boring; do shot "baseline-$t" "$S" "${A[@]}" --theme "$t"; done
fi
if [ "$ONLY" = all ] || [ "$ONLY" = extra ]; then
    shot baseline-plan-stripped-dark "$S" "${A[@]}" --theme dark --plan stripped
    shot baseline-plan-stripped-light "$S" "${A[@]}" --theme light --plan stripped
    shot baseline-suite-dark "$BUILD/Hadron.exe" --module services "${A[@]}" --theme dark
    shot baseline-suite-sort-company-light "$BUILD/Hadron.exe" --module services --fixture "$FIX" --select cphs --accent 0078D7 --theme light --sort company:desc
    shot baseline-sort-hadron-dark "$S" --fixture "$FIX" --select BluetoothUserService --accent 0078D7 --theme dark --sort kb
    shot baseline-bluetooth-dark "$S" --fixture "$FIX" --select BluetoothUserService --accent 0078D7 --theme dark
    shot baseline-drivers-light "$S" --fixture "$FIX" --tab drivers --accent 0078D7 --theme light
    shot baseline-interrupted-boring "$S" --fixture "$FIX/interrupted" --accent 0078D7 --theme boring
    shot baseline-plan-ti-stop-light "$S" --fixture "$FIX/interrupted" --accent 0078D7 --theme light --plan ti-stop
    shot baseline-accent-grey-fun "$S" --fixture "$FIX" --select DiagTrack --accent 808080 --theme fun
    shot baseline-accent-grey-light "$S" --fixture "$FIX" --select DiagTrack --accent 808080 --theme light
    shot baseline-accent-red-dark "$S" --fixture "$FIX" --select DiagTrack --accent E81123 --theme dark
    shot baseline-accent-red-boring "$S" --fixture "$FIX" --select DiagTrack --accent E81123 --theme boring
    shot baseline-accent-yellow-light "$S" --fixture "$FIX" --select DiagTrack --accent FFB900 --theme light
    shot baseline-accent-green-light "$S" --fixture "$FIX" --select DiagTrack --accent 10C010 --theme light
fi
if [ "$ONLY" = all ] || [ "$ONLY" = focus ]; then
    for t in light dark fun boring; do shot "baseline-focus-list-$t" "$S" "${A[@]}" --theme "$t" --focus list; done
    shot baseline-focus-chip-dark "$S" "${A[@]}" --theme dark --focus 2          # TrustedInstaller switch
    shot baseline-focus-preset-light "$S" "${A[@]}" --theme light --focus 5      # Pure bloat removal
    shot baseline-focus-plan-boring "$S" --fixture "$FIX" --accent 0078D7 --theme boring --plan stripped --focus 0   # Cancel
fi
exit $fail
