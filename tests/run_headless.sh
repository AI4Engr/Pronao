#!/usr/bin/env bash
# Headless tests: no window. CeeLing end-to-end (dispatcher + the real native layer + test doubles for the window and the dialogs),
# the file policy against the real file system, program lookup, and the native process runner and folder watcher on their own.
#   tests/run_headless.sh
# Fails if any check prints FAIL, if fewer than the expected number of checks ran, or if a program does not exit 0.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_HEADLESS=214
EXPECTED_FILES=139
EXPECTED_PROGRAMS=18
EXPECTED_TAURIPORT=50
EXPECTED_SITE=29
W="${TMPDIR:-/tmp}/app-cling-headless"
rm -rf "$W"; mkdir -p "$W/work"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PLAT=win ;; *) PLAT=posix ;; esac
"${CXX:-g++}" --std=c++17 -O1 "$HERE/tests/proc_helper.cpp" -o "$W/proc_helper.exe" || { echo "HELPER BUILD FAILED"; exit 1; }
export APP_PROC_ECHO="$W/proc_helper.exe"
OK=1

# a CeeLing program built against the test doubles: all programs share one work directory, so the modules are built once
runProgram() { # name expected
    WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/$1.clg" "$W/$1.exe" --headless || { echo "BUILD FAILED: $1"; OK=0; return; }
    OUT="$("$W/$1.exe")"; RC=$?
    PASS=$(printf '%s\n' "$OUT" | grep -c '^PASS ')
    FAIL=$(printf '%s\n' "$OUT" | grep -c '^FAIL ')
    printf '%s\n' "$OUT" | grep -v '^PASS'
    echo "$1: $PASS passed, $FAIL failed (expected $2 checks, exit code $RC)"
    { [ "$RC" -eq 0 ] && [ "$FAIL" -eq 0 ] && [ "$PASS" -eq "$2" ]; } || OK=0
}
runProgram headless_main "$EXPECTED_HEADLESS"
runProgram files_main "$EXPECTED_FILES"
runProgram programs_main "$EXPECTED_PROGRAMS"
# the tauriport port against the real `lore` tool (only where it is installed)
if command -v lore > /dev/null 2>&1 || [ -n "${LORE_CLI_PATH:-}" ]; then
    EXTRA="$HERE/tests/support.clg $HERE/examples/tauriport/tauriport.clg" runProgram tauriport_main "$EXPECTED_TAURIPORT"
else
    echo "tauriport_main: SKIPPED (lore is not installed)"
fi

# the program's own web files: compiled in from tests/site, looked up the way the window does it
APP_SITE_DIR="$(cd "$HERE/tests/site" && (cygpath -m "$PWD" 2>/dev/null || pwd))" ASSETS="$HERE/tests/site" runProgram site_main "$EXPECTED_SITE"

# the native process runner and folder watcher on their own, against the real helper program and real file changes
bash "$HERE/scripts/pack_assets.sh" - "$W/stub"          # (no embedded web files in these two)
NATIVE="$HERE/shim/watch.cpp $HERE/shim/assets.cpp $W/stub/assets_embedded.cpp $HERE/shim/os.cpp $HERE/shim/$PLAT/fs.cpp $HERE/shim/$PLAT/proc.cpp $HERE/shim/$PLAT/watch.cpp $HERE/tests/doubles/headless_shim.cpp $HERE/tests/doubles/test_support.cpp"
for t in proc watch assets; do
    "${CXX:-g++}" --std=c++17 -O1 "$HERE/tests/${t}_$( [ $t = assets ] && echo test || echo shim_test).cpp" $NATIVE -pthread -o "$W/$t.exe" || { echo "$t TEST BUILD FAILED"; OK=0; continue; }
    TOUT="$("$W/$t.exe")"; TRC=$?
    printf '%s\n' "$TOUT" | grep -v '^PASS'
    [ "$TRC" -eq 0 ] || OK=0
done
[ "$OK" -eq 1 ]
