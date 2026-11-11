#!/usr/bin/env bash
# What the compiler-inserted run-time checks (bounds on v[i]/s[i], null on every object dereference) cost on pronao's own code.
#   scripts/bench_checks.sh [rounds]        (default 5)
# Builds tests/bench_main.clg twice (checks on, then --no-bounds-checks --no-null-checks), runs them alternately, and prints the best time of each
# operation in microseconds per call plus the difference. Both builds use the product's optimisation level (-O1, scripts/build.sh's default) and the
# test doubles for the window, so a call's trip through WebView2 is NOT in the numbers (it would only make the share of the checks smaller).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROUNDS="${1:-5}"
W="${TMPDIR:-/tmp}/pronao-bench"
mkdir -p "$W"
for v in on off; do
    F="--pool-shared"; [ $v = off ] && F="--pool-shared --no-bounds-checks --no-null-checks"
    ( CLING_FLAGS="$F" WORK="$W/work_$v" OBJ_CACHE="$W/cache_$v" bash "$HERE/scripts/build.sh" "$HERE/tests/bench_main.clg" "$W/bench_$v.exe" --headless > "$W/build_$v.log" 2>&1 \
        || { cat "$W/build_$v.log"; echo "BUILD FAILED ($v)"; exit 1; } ) &
done
wait
[ -f "$W/bench_on.exe" ] && [ -f "$W/bench_off.exe" ] || exit 1
# (the checks really are in one build and not the other)
echo "checks in the generated code: on = $(cat "$W"/work_on/*.cpp | grep -o 'cling::at(\|cling::nn(' | wc -l), off = $(cat "$W"/work_off/*.cpp | grep -o 'cling::at(\|cling::nn(' | wc -l)"
for r in $(seq "$ROUNDS"); do
    for v in on off; do "$W/bench_$v.exe" | sed "s/^/$v /"; done
done | awk '
    $3 == "-1" { bad = 1 }
    { key = $1 " " $2; if (!(key in best) || $3 < best[key]) best[key] = $3 }
    END {
        if (bad) { print "a call was refused: the numbers mean nothing"; exit 1 }
        printf "%-6s %12s %12s %8s\n", "", "checks on", "checks off", "cost"
        n = split("add read list", ops, " ")
        for (i = 1; i <= n; i++) {
            o = ops[i]; a = best["on " o]; b = best["off " o]
            printf "%-6s %9.2f us %9.2f us %+7.1f%%\n", o, a, b, (a - b) / b * 100
        }
    }'
