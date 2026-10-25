#!/usr/bin/env bash
# Headless end-to-end tests: CeeLing dispatcher + the real shim queue, no window.
#   tests/run_headless.sh
# Fails if any check prints FAIL, if fewer than EXPECTED checks ran, or if the program does not exit 0.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED=25
W="${TMPDIR:-/tmp}/app-cling-headless"
rm -rf "$W"; mkdir -p "$W/work"
rm -f "$W/t.exe"
WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/headless_main.clg" "$W/t.exe" --headless || { echo "BUILD FAILED"; exit 1; }
[ -f "$W/t.exe" ] || { echo "BUILD FAILED (no binary)"; exit 1; }
OUT="$("$W/t.exe")"; RC=$?
echo "$OUT"
PASS=$(printf '%s\n' "$OUT" | grep -c '^PASS ')
FAIL=$(printf '%s\n' "$OUT" | grep -c '^FAIL ')
echo "headless: $PASS passed, $FAIL failed (expected $EXPECTED checks, exit code $RC)"
[ "$RC" -eq 0 ] && [ "$FAIL" -eq 0 ] && [ "$PASS" -eq "$EXPECTED" ]
