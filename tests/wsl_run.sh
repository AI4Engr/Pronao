#!/usr/bin/env bash
# The WSL half of tests/run_sanitizers.sh: compiles and runs everything under the sanitizers.
#   wsl_run.sh <directory prepared by run_sanitizers.sh>
# Speed: every object is compiled once per (source, flags) and shared by the sections that need it (the libraries and the native layer are the same
# files in every ASan program and in every TSan program), the first compiles run in parallel, and the sections themselves run MAXPAR at a time,
# each writing to its own file; the output is printed in the order the sections are listed, with the seconds since the start.
set -uo pipefail
T0=$(date +%s)
cd "$1"
EXPECTED=282
EXPECTED_FILES=123
EXPECTED_PROGRAMS=15
ASAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
TSAN="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
LIBS="cjson_item cjson_api cjson_parse cjson_print text core files assets app embedded"
EXPECTED_SITE=38
MAXPAR="${MAXPAR:-8}"
mkdir -p sec

tag() { echo "$1" | md5sum | cut -c1-6; }

# compile <source> to <object> once (atomically: sections run at the same time and may ask for the same object)
cc_o() { # src obj flags...
    local src="$1" obj="$2"
    shift 2
    [ -f "$obj" ] && return 0
    g++ --std=c++17 "$@" -c "$src" -o "$obj.$$" || return 1
    mv -f "$obj.$$" "$obj"
}

# the native layer and the test doubles (POSIX files), compiled from <repo dir> with <flags>; prints the object list
NATIVE_SRC="shim/watch.cpp shim/assets.cpp shim/os.cpp shim/posix/fs.cpp shim/posix/proc.cpp shim/posix/watch.cpp tests/doubles/headless_shim.cpp tests/doubles/test_support.cpp"
native_objs() { # repo flags
    local o="" f t pids=() p rc=0
    t=$(tag "$2")
    for f in $NATIVE_SRC; do
        local obj="$1/.obj_$t.$(echo $f | tr '/' '_').o"
        cc_o "$1/$f" "$obj" $2 &
        pids+=($!)
        o="$o $obj"
    done
    # the embedded web files: none, unless ASSETS_DIR holds a packed folder
    local a="${ASSETS_DIR:-$1}"
    local eobj="$1/.obj_$t.embedded_$(echo "$a" | md5sum | cut -c1-6).o"
    cc_o "$a/assets_embedded.cpp" "$eobj" $2 &
    pids+=($!)
    o="$o $eobj"
    for p in "${pids[@]}"; do wait "$p" || rc=1; done
    [ $rc -eq 0 ] || return 1
    [ -f "$a/assets_blob.o" ] && o="$o $a/assets_blob.o"
    echo "$o"
}

# build a CeeLing program (<main>) from <variant dir> and <repo dir> with <flags> into <exe>
build_program() { # variant repo flags exe main
    local objs="" f natives t pids=() p rc=0
    t=$(tag "$3")
    for f in $LIBS $5; do
        local obj="$1/$f.$t.o"
        cc_o "$1/$f.cpp" "$obj" $3 -pthread -Wno-unknown-pragmas &
        pids+=($!)
        objs="$objs $obj"
    done
    for p in "${pids[@]}"; do wait "$p" || rc=1; done
    [ $rc -eq 0 ] || return 1
    natives=$(native_objs "$2" "$3") || return 1
    g++ $3 $objs $natives -pthread -o "$4" || return 1
}
# TSan needs ASLR off on some WSL kernels (its shadow-memory mapping fails otherwise)
run_tsan() { # exe out
    # (bounded: a lock-removal control can corrupt a container and spin; its races are reported long before)
    APP_PROC_ECHO="$PWD/proc_helper" TSAN_OPTIONS=halt_on_error=0 timeout "${TSAN_TIMEOUT:-120}" setarch "$(uname -m)" -R "./$1" > "$2" 2>&1
}
# a C++ test (<source>) linked with the native layer from <repo dir>
build_cpp_test() { # source repo flags exe
    local natives
    natives=$(native_objs "$2" "$3") || return 1
    g++ --std=c++17 $3 -I "$2/tests" "$2/tests/$1" $natives -pthread -o "$4" || return 1
}

# the program the process tests run (every section below that needs it finds it through APP_PROC_ECHO)
g++ --std=c++17 -O1 repo/tests/proc_helper.cpp -o proc_helper || { echo "HELPER BUILD FAILED"; exit 1; }
bash repo/scripts/pack_assets.sh repo/tests/site site_assets || { echo "PACK FAILED"; exit 1; }

# ---- the sections; each prints its own lines and returns non-zero if it failed -----------------------------------------------------------------
sec_asan_program() { # name expected
    local name="$1" rc pass bad
    echo "== ASan+UBSan: $name"
    build_program work repo "$ASAN" $name.asan $name || { echo "BUILD FAILED"; return 1; }
    APP_PROC_ECHO="$PWD/proc_helper" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./$name.asan > $name.asan.out 2>&1
    rc=$?; pass=$(grep -c '^PASS ' $name.asan.out); bad=$(grep -c '^FAIL ' $name.asan.out)
    echo "   exit $rc, $pass passed, $bad failed"
    if [ $rc -ne 0 ] || [ "$pass" -ne "$2" ] || [ "$bad" -ne 0 ]; then grep "^FAIL" $name.asan.out; tail -30 $name.asan.out; return 1; fi
}
sec_asan_headless() { sec_asan_program headless_main $EXPECTED; }
sec_asan_files() { sec_asan_program files_main $EXPECTED_FILES; }
sec_asan_programs() { sec_asan_program programs_main $EXPECTED_PROGRAMS; }

sec_asan_site() {
    local rc pass bad
    echo "== ASan+UBSan: the program's own web files (packed here, looked up as the window does)"
    ASSETS_DIR="$PWD/site_assets" build_program work repo "$ASAN" site_main.asan site_main || { echo "BUILD FAILED"; return 1; }
    APP_SITE_DIR="$PWD/repo/tests/site" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./site_main.asan > site_main.asan.out 2>&1
    rc=$?; pass=$(grep -c '^PASS ' site_main.asan.out); bad=$(grep -c '^FAIL ' site_main.asan.out)
    echo "   exit $rc, $pass passed, $bad failed"
    if [ $rc -ne 0 ] || [ "$pass" -ne $EXPECTED_SITE ] || [ "$bad" -ne 0 ]; then grep "^FAIL" site_main.asan.out; tail -30 site_main.asan.out; return 1; fi
}

sec_asan_proc() {
    local rc
    echo "== ASan+UBSan: program launching (real child processes)"
    build_cpp_test proc_shim_test.cpp repo "$ASAN" proc.asan || return 1
    APP_PROC_ECHO="$PWD/proc_helper" ASAN_OPTIONS=detect_leaks=1 ./proc.asan > proc.asan.out 2>&1; rc=$?
    echo "   exit $rc: $(tail -1 proc.asan.out)"
    [ $rc -eq 0 ] || { grep -v '^PASS' proc.asan.out | head -30; return 1; }
}

sec_asan_watch() {
    local rc
    echo "== ASan+UBSan: folder watcher (real inotify events)"
    build_cpp_test watch_shim_test.cpp repo "$ASAN" watch.asan || return 1
    ASAN_OPTIONS=detect_leaks=1 ./watch.asan > watch.asan.out 2>&1; rc=$?
    echo "   exit $rc: $(tail -1 watch.asan.out)"
    [ $rc -eq 0 ] || { grep -v '^PASS' watch.asan.out | head -30; return 1; }
}

sec_tsan_watch() {
    local rc races
    echo "== TSan: folder watcher"
    build_cpp_test watch_shim_test.cpp repo "$TSAN" watch.tsan || return 1
    run_tsan watch.tsan watch.tsan.out; rc=$?
    races=$(grep -c 'WARNING: ThreadSanitizer' watch.tsan.out)
    echo "   exit $rc, $races TSan reports: $(grep 'watch shim test' watch.tsan.out | tail -1)"
    if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then grep -v '^PASS' watch.tsan.out | head -40; return 1; fi
}

sec_assets() {
    local rc races ok=0
    echo "== ASan+UBSan and TSan: the asset table"
    g++ --std=c++17 $ASAN -pthread repo/tests/assets_test.cpp -o assets.asan || return 1
    ASAN_OPTIONS=detect_leaks=1 ./assets.asan > assets.asan.out 2>&1; rc=$?
    echo "   ASan exit $rc: $(tail -1 assets.asan.out)"
    [ $rc -eq 0 ] || { grep -v '^PASS' assets.asan.out | head -20; ok=1; }
    g++ --std=c++17 $TSAN -pthread repo/tests/assets_test.cpp -o assets.tsan || return 1
    run_tsan assets.tsan assets.tsan.out; rc=$?; races=$(grep -c 'WARNING: ThreadSanitizer' assets.tsan.out)
    echo "   TSan exit $rc, $races reports"
    [ $rc -eq 0 ] && [ "$races" -eq 0 ] || { head -30 assets.tsan.out; ok=1; }
    return $ok
}

sec_stress_asan() {
    local rc
    echo "== ASan+UBSan: queue stress"
    build_cpp_test queue_stress.cpp repo "$ASAN" stress.asan || return 1
    ASAN_OPTIONS=detect_leaks=1 ./stress.asan > stress.asan.out 2>&1; rc=$?
    echo "   exit $rc: $(tail -1 stress.asan.out)"
    [ $rc -eq 0 ] || { tail -30 stress.asan.out; return 1; }
}

sec_stress_tsan() {
    local rc races
    echo "== TSan: queue stress"
    build_cpp_test queue_stress.cpp repo "$TSAN" stress.tsan || return 1
    run_tsan stress.tsan stress.tsan.out; rc=$?
    races=$(grep -c 'WARNING: ThreadSanitizer' stress.tsan.out)
    echo "   exit $rc, $races TSan reports: $(grep 'queue stress' stress.tsan.out | tail -1)"
    if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then head -40 stress.tsan.out; return 1; fi
}

sec_tsan_program() { # variant
    local variant="$1" rc races pass bad
    echo "== TSan: CeeLing end-to-end tests with worker threads ($variant)"
    build_program $variant repo "$TSAN" headless.tsan.$variant headless_main || { echo "BUILD FAILED"; return 1; }
    run_tsan headless.tsan.$variant headless.tsan.$variant.out; rc=$?
    races=$(grep -c 'WARNING: ThreadSanitizer' headless.tsan.$variant.out)
    pass=$(grep -c '^PASS ' headless.tsan.$variant.out); bad=$(grep -c '^FAIL ' headless.tsan.$variant.out)
    echo "   exit $rc, $pass passed, $bad failed, $races TSan reports"
    if [ $rc -ne 0 ] || [ "$races" -ne 0 ] || [ "$pass" -ne $EXPECTED ] || [ "$bad" -ne 0 ]; then grep "^FAIL" headless.tsan.$variant.out; head -60 headless.tsan.$variant.out; return 1; fi
}
sec_tsan_work() { sec_tsan_program work; }
sec_tsan_pool() { sec_tsan_program work_pool; }

sec_control1() {
    local races
    echo "== TSan negative control 1: the queue's push without its lock (queue stress; must report a race)"
    build_cpp_test queue_stress.cpp nolock "$TSAN" stress.nolock || return 1
    TSAN_TIMEOUT=30 run_tsan stress.nolock stress.nolock.out      # (the broken queue may corrupt itself and hang the later phases: its races are reported first)
    races=$(grep -c 'WARNING: ThreadSanitizer' stress.nolock.out)
    echo "   $races TSan reports"
    if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run above proves nothing"; return 1; fi
}

sec_control2() {
    local races
    echo "== TSan negative control 2: reply side without its lock (CeeLing workers answering concurrently; must report a race)"
    build_program work nolock_ret "$TSAN" headless.nolockret headless_main || { echo "BUILD FAILED"; return 1; }
    run_tsan headless.nolockret headless.nolockret.out
    races=$(grep -c 'WARNING: ThreadSanitizer' headless.nolockret.out)
    echo "   $races TSan reports"
    if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run of the CeeLing tests proves nothing"; return 1; fi
}

sec_control4() {
    local rc
    echo "== negative control 4: wait-any without its notification (the ping-pong test must see the lost wakeup and fail)"
    build_cpp_test queue_stress.cpp nonotify "-O1" stress.nonotify || return 1
    ./stress.nonotify > stress.nonotify.out 2>&1; rc=$?
    echo "   exit $rc: $(grep -c 'wakeup was lost' stress.nonotify.out) lost-wakeup report(s)"
    if [ $rc -eq 0 ] || ! grep -q 'wakeup was lost' stress.nonotify.out; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the wait-any tests prove nothing"; return 1; fi
}

sec_control3() {
    local races
    echo "== TSan negative control 3: Channel::send without its lock (a producer thread and the consumer; must report a race)"
    build_program work_nochannel repo "$TSAN" headless.nochannel headless_main || { echo "BUILD FAILED"; return 1; }
    run_tsan headless.nochannel headless.nochannel.out
    races=$(grep -c 'WARNING: ThreadSanitizer' headless.nochannel.out)
    echo "   $races TSan reports"
    if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run of the Channel checks proves nothing"; return 1; fi
}

# Phase 1 runs everything that does not depend on timing; phase 2 (the queue stress, whose ping-pong test measures how late a wakeup is, and the two
# controls built on it) runs alone afterwards, so a busy machine cannot make a correct wakeup look late.
PHASE1="sec_asan_headless sec_asan_files sec_asan_programs sec_asan_site sec_asan_proc sec_asan_watch sec_tsan_watch sec_assets sec_tsan_work sec_tsan_pool sec_control2 sec_control3"
PHASE2="sec_stress_asan sec_stress_tsan sec_control1 sec_control4"
SECTIONS="sec_asan_headless sec_asan_files sec_asan_programs sec_asan_site sec_asan_proc sec_asan_watch sec_tsan_watch sec_assets sec_stress_asan sec_stress_tsan sec_tsan_work sec_tsan_pool sec_control1 sec_control2 sec_control4 sec_control3"

# the libraries and the native layer every ASan / TSan program shares are compiled first, all at once
( build_program work repo "$ASAN" /dev/null headless_main > /dev/null 2>&1 ) &
( build_program work repo "$TSAN" /dev/null headless_main > /dev/null 2>&1 ) &
( build_program work_pool repo "$TSAN" /dev/null headless_main > /dev/null 2>&1 ) &
wait

run_phase() { # sections...
    local s
    for s in "$@"; do
        while [ "$(jobs -rp | wc -l)" -ge "$MAXPAR" ]; do wait -n; done
        (
            t=$(date +%s)
            $s > "sec/$s.out" 2>&1
            echo $? > "sec/$s.rc"
            echo $(( $(date +%s) - t )) > "sec/$s.dur"
        ) &
    done
    wait
}
run_phase $PHASE1
run_phase $PHASE2

fail=0
for s in $SECTIONS; do
    first=1
    while IFS= read -r line; do
        if [ $first -eq 1 ]; then echo "[$(cat sec/$s.dur 2> /dev/null || echo ?)s] $line"; first=0; else echo "$line"; fi
    done < "sec/$s.out"
    [ "$(cat sec/$s.rc 2> /dev/null || echo 1)" = 0 ] || fail=1
done
echo "[total $(( $(date +%s) - T0 ))s]"
exit $fail
