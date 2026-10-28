#!/usr/bin/env bash
# The WSL half of tests/run_sanitizers.sh: compiles and runs everything under the sanitizers.
#   wsl_run.sh <directory prepared by run_sanitizers.sh>
set -uo pipefail
cd "$1"
fail=0
EXPECTED=224
EXPECTED_FILES=123
EXPECTED_PROGRAMS=15
ASAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
TSAN="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
LIBS="cjson_item cjson_api cjson_parse cjson_print text core files assets app embedded"
EXPECTED_SITE=29

# the native layer and the test doubles (POSIX files), compiled from <repo dir> with <flags> into <prefix>*.o
NATIVE_SRC="shim/watch.cpp shim/assets.cpp shim/os.cpp shim/posix/fs.cpp shim/posix/proc.cpp shim/posix/watch.cpp tests/doubles/headless_shim.cpp tests/doubles/test_support.cpp"
native_objs() { # repo flags prefix
    local o="" f
    for f in $NATIVE_SRC; do
        g++ --std=c++17 $2 -c "$1/$f" -o "$3$(echo $f | tr '/' '_').o" || return 1
        o="$o $3$(echo $f | tr '/' '_').o"
    done
    # the embedded web files: none, unless ASSETS_DIR holds a packed folder
    local a="${ASSETS_DIR:-$1}"
    g++ --std=c++17 $2 -c "$a/assets_embedded.cpp" -o "${3}embedded.o" || return 1
    o="$o ${3}embedded.o"
    [ -f "$a/assets_blob.o" ] && o="$o $a/assets_blob.o"
    echo "$o"
}

# build a CeeLing program (<main>) from <variant dir> and <repo dir> with <flags> into <exe>
build_program() { # variant repo flags exe main
    local objs="" f natives
    for f in $LIBS $5; do
        g++ --std=c++17 $3 -pthread -Wno-unknown-pragmas -c "$1/$f.cpp" -o "$1/$f.$4.o" || return 1
        objs="$objs $1/$f.$4.o"
    done
    natives=$(native_objs "$2" "$3" "$1/native.$4.") || return 1
    g++ $3 $objs $natives -pthread -o "$4" || return 1
}
# TSan needs ASLR off on some WSL kernels (its shadow-memory mapping fails otherwise)
run_tsan() { # exe out
    APP_PROC_ECHO="$PWD/proc_helper" TSAN_OPTIONS=halt_on_error=0 setarch "$(uname -m)" -R "./$1" > "$2" 2>&1
}
# a C++ test (<source>) linked with the native layer from <repo dir>
build_cpp_test() { # source repo flags exe
    local natives
    natives=$(native_objs "$2" "$3" "$4.") || return 1
    g++ --std=c++17 $3 -I "$2/tests" "$2/tests/$1" $natives -pthread -o "$4" || return 1
}

# the program the process tests run (every section below that needs it finds it through APP_PROC_ECHO)
g++ --std=c++17 -O1 repo/tests/proc_helper.cpp -o proc_helper || { echo "HELPER BUILD FAILED"; exit 1; }

for m in "headless_main $EXPECTED" "files_main $EXPECTED_FILES" "programs_main $EXPECTED_PROGRAMS"; do
    set -- $m
    echo "== ASan+UBSan: $1"
    build_program work repo "$ASAN" $1.asan $1 || { echo "BUILD FAILED"; exit 1; }
    APP_PROC_ECHO="$PWD/proc_helper" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./$1.asan > $1.asan.out 2>&1
    rc=$?; pass=$(grep -c '^PASS ' $1.asan.out); bad=$(grep -c '^FAIL ' $1.asan.out)
    echo "   exit $rc, $pass passed, $bad failed"
    if [ $rc -ne 0 ] || [ "$pass" -ne "$2" ] || [ "$bad" -ne 0 ]; then tail -30 $1.asan.out; fail=1; fi
done

echo "== ASan+UBSan: the program's own web files (packed here, looked up as the window does)"
bash repo/scripts/pack_assets.sh repo/tests/site site_assets || { echo "PACK FAILED"; exit 1; }
ASSETS_DIR="$PWD/site_assets" build_program work repo "$ASAN" site_main.asan site_main || { echo "BUILD FAILED"; exit 1; }
APP_SITE_DIR="$PWD/repo/tests/site" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./site_main.asan > site_main.asan.out 2>&1
rc=$?; pass=$(grep -c '^PASS ' site_main.asan.out); bad=$(grep -c '^FAIL ' site_main.asan.out)
echo "   exit $rc, $pass passed, $bad failed"
if [ $rc -ne 0 ] || [ "$pass" -ne $EXPECTED_SITE ] || [ "$bad" -ne 0 ]; then tail -30 site_main.asan.out; fail=1; fi

echo "== ASan+UBSan: program launching (real child processes)"
build_cpp_test proc_shim_test.cpp repo "$ASAN" proc.asan || exit 1
APP_PROC_ECHO="$PWD/proc_helper" ASAN_OPTIONS=detect_leaks=1 ./proc.asan > proc.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 proc.asan.out)"
[ $rc -eq 0 ] || { grep -v '^PASS' proc.asan.out | head -30; fail=1; }

echo "== ASan+UBSan: folder watcher (real inotify events)"
build_cpp_test watch_shim_test.cpp repo "$ASAN" watch.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./watch.asan > watch.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 watch.asan.out)"
[ $rc -eq 0 ] || { grep -v '^PASS' watch.asan.out | head -30; fail=1; }

echo "== TSan: folder watcher"
build_cpp_test watch_shim_test.cpp repo "$TSAN" watch.tsan || exit 1
run_tsan watch.tsan watch.tsan.out; rc=$?
races=$(grep -c 'WARNING: ThreadSanitizer' watch.tsan.out)
echo "   exit $rc, $races TSan reports: $(grep 'watch shim test' watch.tsan.out | tail -1)"
if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then grep -v '^PASS' watch.tsan.out | head -40; fail=1; fi

echo "== ASan+UBSan and TSan: the asset table"
g++ --std=c++17 $ASAN -pthread repo/tests/assets_test.cpp -o assets.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./assets.asan > assets.asan.out 2>&1; rc=$?
echo "   ASan exit $rc: $(tail -1 assets.asan.out)"
[ $rc -eq 0 ] || { grep -v '^PASS' assets.asan.out | head -20; fail=1; }
g++ --std=c++17 $TSAN -pthread repo/tests/assets_test.cpp -o assets.tsan || exit 1
run_tsan assets.tsan assets.tsan.out; rc=$?; races=$(grep -c 'WARNING: ThreadSanitizer' assets.tsan.out)
echo "   TSan exit $rc, $races reports"
[ $rc -eq 0 ] && [ "$races" -eq 0 ] || { head -30 assets.tsan.out; fail=1; }

echo "== ASan+UBSan: queue stress"
build_cpp_test queue_stress.cpp repo "$ASAN" stress.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./stress.asan > stress.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 stress.asan.out)"
[ $rc -eq 0 ] || { tail -30 stress.asan.out; fail=1; }

echo "== TSan: queue stress"
build_cpp_test queue_stress.cpp repo "$TSAN" stress.tsan || exit 1
run_tsan stress.tsan stress.tsan.out; rc=$?
races=$(grep -c 'WARNING: ThreadSanitizer' stress.tsan.out)
echo "   exit $rc, $races TSan reports: $(grep 'queue stress' stress.tsan.out | tail -1)"
if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then head -40 stress.tsan.out; fail=1; fi

for variant in work work_pool; do
    echo "== TSan: CeeLing end-to-end tests with worker threads ($variant)"
    build_program $variant repo "$TSAN" headless.tsan.$variant headless_main || { echo "BUILD FAILED"; exit 1; }
    run_tsan headless.tsan.$variant headless.tsan.$variant.out; rc=$?
    races=$(grep -c 'WARNING: ThreadSanitizer' headless.tsan.$variant.out)
    pass=$(grep -c '^PASS ' headless.tsan.$variant.out); bad=$(grep -c '^FAIL ' headless.tsan.$variant.out)
    echo "   exit $rc, $pass passed, $bad failed, $races TSan reports"
    if [ $rc -ne 0 ] || [ "$races" -ne 0 ] || [ "$pass" -ne $EXPECTED ] || [ "$bad" -ne 0 ]; then head -60 headless.tsan.$variant.out; fail=1; fi
done

echo "== TSan negative control 1: the queue's push without its lock (queue stress; must report a race)"
build_cpp_test queue_stress.cpp nolock "$TSAN" stress.nolock || exit 1
run_tsan stress.nolock stress.nolock.out
races=$(grep -c 'WARNING: ThreadSanitizer' stress.nolock.out)
echo "   $races TSan reports"
if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run above proves nothing"; fail=1; fi

echo "== TSan negative control 2: reply side without its lock (CeeLing workers answering concurrently; must report a race)"
build_program work nolock_ret "$TSAN" headless.nolockret headless_main || { echo "BUILD FAILED"; exit 1; }
run_tsan headless.nolockret headless.nolockret.out
races=$(grep -c 'WARNING: ThreadSanitizer' headless.nolockret.out)
echo "   $races TSan reports"
if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run of the CeeLing tests proves nothing"; fail=1; fi
exit $fail
