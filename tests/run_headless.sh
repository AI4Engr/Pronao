#!/usr/bin/env bash
# Headless tests: no window. CeeLing end-to-end (dispatcher + the real native layer + test doubles for the window and the dialogs),
# the file policy against the real file system, program lookup, and the native process runner and folder watcher on their own.
#   tests/run_headless.sh          (SLOW=1: build with -O1 like the product instead of -O0)
# Fails if any check prints FAIL, if fewer than the expected number of checks ran, or if a program does not exit 0.
# What can run at the same time does: modules are compiled in parallel and kept in OBJ_CACHE, so the first program pays for them and the others
# only compile their own main; the programs run together; the native tests are linked from objects compiled once. -O0 because these tests check
# behaviour, not speed (and libstdc++'s own assertions are on in an unoptimised build, which only makes them stricter); the sanitizer runs use -O1.
# The output of every part is printed in a fixed order at the end.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_HEADLESS=295
EXPECTED_FILES=147
EXPECTED_PROGRAMS=18
EXPECTED_TAURIPORT=50
EXPECTED_SITE=38
EXPECTED_STORE=54
EXPECTED_LINKS=57
EXPECTED_INSTANCE=19
EXPECTED_PLACEMENT=41
EXPECTED_MENU=100
W="${TMPDIR:-/tmp}/app-cling-headless"
rm -rf "$W"; mkdir -p "$W"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PLAT=win ;; *) PLAT=posix ;; esac
CXX="${CXX:-g++}"
OPT_LEVEL="-O0"; [ -n "${SLOW:-}" ] && OPT_LEVEL="-O1"
export OPT="$OPT_LEVEL" OBJ_CACHE="$W/cache"
"$CXX" --std=c++17 -O1 "$HERE/tests/proc_helper.cpp" -o "$W/proc_helper.exe" || { echo "HELPER BUILD FAILED"; exit 1; }
export APP_PROC_ECHO="$W/proc_helper.exe"
OK=1

# build one CeeLing program against the test doubles (log in $W/<name>.build)
buildProgram() { # name
    WORK="$W/work_$1" bash "$HERE/scripts/build.sh" "$HERE/tests/$1.clg" "$W/$1.exe" --headless > "$W/$1.build" 2>&1
}
# run it and judge it: everything it prints goes to $W/<name>.log, its verdict to $W/<name>.verdict
runProgram() { # name expected
    local name="$1" expected="$2" out rc pass fail
    {
        if [ ! -f "$W/$name.exe" ]; then
            cat "$W/$name.build"; echo "BUILD FAILED: $name"; echo bad > "$W/$name.verdict"; return
        fi
        mkdir -p "$W/run_$name"       # (a scratch folder: a test that writes into its current folder must not leave files in the repository)
        out="$(cd "$W/run_$name" && "$W/$name.exe")"; rc=$?
        pass=$(printf '%s\n' "$out" | grep -c '^PASS ')
        fail=$(printf '%s\n' "$out" | grep -c '^FAIL ')
        printf '%s\n' "$out" | grep -v '^PASS'
        echo "$name: $pass passed, $fail failed (expected $expected checks, exit code $rc)"
        if [ "$rc" -eq 0 ] && [ "$fail" -eq 0 ] && [ "$pass" -eq "$expected" ]; then echo ok > "$W/$name.verdict"; else echo bad > "$W/$name.verdict"; fi
    } > "$W/$name.log" 2>&1
}

# first (in the background): the native process runner and folder watcher on their own (real helper program, real file changes); the shared native sources are
# compiled once, in parallel, then each test is compiled and linked against those objects
bash "$HERE/scripts/pack_assets.sh" - "$W/stub"          # (no embedded web files in these)
NSRC="$HERE/shim/watch.cpp $HERE/shim/assets.cpp $W/stub/assets_embedded.cpp $HERE/shim/os.cpp $HERE/shim/$PLAT/fs.cpp $HERE/shim/$PLAT/proc.cpp $HERE/shim/$PLAT/watch.cpp $HERE/shim/$PLAT/instance.cpp $HERE/tests/doubles/headless_shim.cpp $HERE/tests/doubles/test_support.cpp"
NOBJS=""
NP=()
i=0
for s in $NSRC; do
    i=$((i + 1))
    "$CXX" --std=c++17 $OPT_LEVEL -c "$s" -o "$W/native_$i.o" &
    NP+=($!)
    NOBJS="$NOBJS $W/native_$i.o"
done
for p in "${NP[@]}"; do wait "$p" || OK=0; done
TPIDS=()
for t in proc watch assets instance; do
    src="$HERE/tests/${t}_$( [ $t = assets ] && echo test || echo shim_test).cpp"
    (
        "$CXX" --std=c++17 $OPT_LEVEL "$src" $NOBJS -pthread $( [ "$PLAT" = win ] && echo "-ladvapi32 -lshell32 -lole32" ) -o "$W/$t.exe" > "$W/$t.log" 2>&1 || { echo "$t TEST BUILD FAILED" >> "$W/$t.log"; echo bad > "$W/$t.verdict"; exit 0; }
        TOUT="$("$W/$t.exe")"; TRC=$?
        printf '%s\n' "$TOUT" | grep -v '^PASS' >> "$W/$t.log"
        if [ "$TRC" -eq 0 ]; then echo ok > "$W/$t.verdict"; else echo bad > "$W/$t.verdict"; fi
    ) &
    TPIDS+=($!)
done

# the first program fills the module cache; the others then compile only their own main (and run alongside)
buildProgram headless_main
runProgram headless_main "$EXPECTED_HEADLESS" &
PIDS=($!)
NAMES="headless_main"
buildProgram files_main &
B1=$!
buildProgram programs_main &
B2=$!
buildProgram store_main &
B3=$!
buildProgram links_main &
B4=$!
buildProgram instance_main &
B5=$!
buildProgram placement_main &
B6=$!
buildProgram menu_main &
B7=$!
BS=("$B1" "$B2" "$B3" "$B4" "$B5" "$B6" "$B7")
( ASSETS="$HERE/tests/site" WORK="$W/work_site_main" bash "$HERE/scripts/build.sh" "$HERE/tests/site_main.clg" "$W/site_main.exe" --headless > "$W/site_main.build" 2>&1 ) &
BS+=($!)
HAVE_LORE=0
if command -v lore > /dev/null 2>&1 || [ -n "${LORE_CLI_PATH:-}" ]; then
    HAVE_LORE=1
    ( EXTRA="$HERE/tests/support.clg $HERE/examples/tauriport/tauriport.clg" WORK="$W/work_tauriport_main" bash "$HERE/scripts/build.sh" "$HERE/tests/tauriport_main.clg" "$W/tauriport_main.exe" --headless > "$W/tauriport_main.build" 2>&1 ) &
    BS+=($!)
fi

for p in "${BS[@]}"; do wait "$p"; done
runProgram files_main "$EXPECTED_FILES" &
PIDS+=($!)
runProgram programs_main "$EXPECTED_PROGRAMS" &
PIDS+=($!)
runProgram store_main "$EXPECTED_STORE" &
PIDS+=($!)
runProgram links_main "$EXPECTED_LINKS" &
PIDS+=($!)
runProgram instance_main "$EXPECTED_INSTANCE" &
PIDS+=($!)
runProgram placement_main "$EXPECTED_PLACEMENT" &
PIDS+=($!)
runProgram menu_main "$EXPECTED_MENU" &
PIDS+=($!)
APP_SITE_DIR="$(cd "$HERE/tests/site" && (cygpath -m "$PWD" 2>/dev/null || pwd))" runProgram site_main "$EXPECTED_SITE" &
PIDS+=($!)
NAMES="$NAMES files_main programs_main store_main links_main instance_main placement_main menu_main site_main"
if [ "$HAVE_LORE" -eq 1 ]; then
    runProgram tauriport_main "$EXPECTED_TAURIPORT" &
    PIDS+=($!)
    NAMES="$NAMES tauriport_main"
else
    echo "tauriport_main: SKIPPED (lore is not installed)"
fi
for p in "${PIDS[@]}" "${TPIDS[@]}"; do wait "$p"; done

for n in headless_main files_main programs_main store_main links_main instance_main placement_main menu_main tauriport_main site_main; do
    [ -f "$W/$n.log" ] || continue
    cat "$W/$n.log"
    [ "$(cat "$W/$n.verdict" 2> /dev/null)" = ok ] || OK=0
done
for t in proc watch assets instance; do
    cat "$W/$t.log"
    [ "$(cat "$W/$t.verdict" 2> /dev/null)" = ok ] || OK=0
done
[ "$OK" -eq 1 ]
