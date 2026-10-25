#!/usr/bin/env bash
# The WSL half of tests/run_sanitizers.sh: compiles and runs everything under the sanitizers.
#   wsl_run.sh <directory prepared by run_sanitizers.sh>
set -uo pipefail
cd "$1"
fail=0
EXPECTED=147
ASAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
TSAN="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
LIBS="cjson_item cjson_api cjson_parse cjson_print app headless_main embedded"

# build the CeeLing end-to-end program from <variant dir> and <shim dir> with <flags> into <exe>
build_headless() { # variant shimdir flags exe
    local objs="" f
    for f in $LIBS; do
        g++ --std=c++17 $3 -pthread -Wno-unknown-pragmas -c "$1/$f.cpp" -o "$1/$f.$4.o" || return 1
        objs="$objs $1/$f.$4.o"
    done
    g++ --std=c++14 $3 -DAPP_HEADLESS -I "$2" -c "$2/app_shim.cpp" -o "$1/shim.$4.o" || return 1
    g++ --std=c++17 $3 -DAPP_HEADLESS -I "$2" -c "$2/app_fs.cpp" -o "$1/fs.$4.o" || return 1
    g++ --std=c++17 $3 -DAPP_HEADLESS -I "$2" -c "$2/app_dialog.cpp" -o "$1/dialog.$4.o" || return 1
    g++ --std=c++17 $3 -DAPP_HEADLESS -I "$2" -c "$2/app_proc.cpp" -o "$1/proc.$4.o" || return 1
    g++ --std=c++17 $3 -DAPP_HEADLESS -I "$2" -c "$2/app_watch.cpp" -o "$1/watch.$4.o" || return 1
    g++ $3 $objs "$1/shim.$4.o" "$1/fs.$4.o" "$1/dialog.$4.o" "$1/proc.$4.o" "$1/watch.$4.o" -pthread -o "$4" || return 1
}
# TSan needs ASLR off on some WSL kernels (its shadow-memory mapping fails otherwise)
run_tsan() { # exe out
    APP_PROC_ECHO="$PWD/proc_helper" TSAN_OPTIONS=halt_on_error=0 setarch "$(uname -m)" -R "./$1" > "$2" 2>&1
}

# the program the process tests run (every section below that needs it finds it through APP_PROC_ECHO)
g++ --std=c++17 -O1 proc_helper.cpp -o proc_helper || { echo "HELPER BUILD FAILED"; exit 1; }

echo "== ASan+UBSan: CeeLing end-to-end tests (includes real worker threads)"
build_headless work shim "$ASAN" headless.asan || { echo "BUILD FAILED"; exit 1; }
APP_PROC_ECHO="$PWD/proc_helper" ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./headless.asan > headless.asan.out 2>&1
rc=$?; pass=$(grep -c '^PASS ' headless.asan.out); bad=$(grep -c '^FAIL ' headless.asan.out)
echo "   exit $rc, $pass passed, $bad failed"
if [ $rc -ne 0 ] || [ "$pass" -ne $EXPECTED ] || [ "$bad" -ne 0 ]; then tail -30 headless.asan.out; fail=1; fi

echo "== ASan+UBSan: file access shim (real folders, symlink escapes)"
g++ --std=c++17 $ASAN -DAPP_HEADLESS -I shim fs_shim_test.cpp shim/app_fs.cpp -o fs.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./fs.asan > fs.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 fs.asan.out)"
[ $rc -eq 0 ] || { grep -v '^PASS' fs.asan.out | head -30; fail=1; }

echo "== ASan+UBSan: program launching shim (real child processes)"
g++ --std=c++17 $ASAN -DAPP_HEADLESS -I shim proc_shim_test.cpp shim/app_proc.cpp shim/app_fs.cpp -o proc.asan || exit 1
APP_PROC_ECHO="$PWD/proc_helper" ASAN_OPTIONS=detect_leaks=1 ./proc.asan > proc.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 proc.asan.out)"
[ $rc -eq 0 ] || { grep -v '^PASS' proc.asan.out | head -30; fail=1; }

echo "== ASan+UBSan: folder watcher (real inotify events)"
g++ --std=c++17 $ASAN -DAPP_HEADLESS -I shim watch_shim_test.cpp shim/app_watch.cpp shim/app_fs.cpp shim/app_shim.cpp -pthread -o watch.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./watch.asan > watch.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 watch.asan.out)"
[ $rc -eq 0 ] || { grep -v '^PASS' watch.asan.out | head -30; fail=1; }

echo "== TSan: folder watcher"
g++ --std=c++17 $TSAN -DAPP_HEADLESS -I shim watch_shim_test.cpp shim/app_watch.cpp shim/app_fs.cpp shim/app_shim.cpp -pthread -o watch.tsan || exit 1
run_tsan watch.tsan watch.tsan.out; rc=$?
races=$(grep -c 'WARNING: ThreadSanitizer' watch.tsan.out)
echo "   exit $rc, $races TSan reports: $(grep 'watch shim test' watch.tsan.out | tail -1)"
if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then grep -v '^PASS' watch.tsan.out | head -40; fail=1; fi

echo "== ASan+UBSan: queue stress"
g++ --std=c++17 $ASAN -DAPP_HEADLESS -I shim queue_stress.cpp shim/app_shim.cpp shim/app_watch.cpp shim/app_fs.cpp -pthread -o stress.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./stress.asan > stress.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 stress.asan.out)"
[ $rc -eq 0 ] || { tail -30 stress.asan.out; fail=1; }

echo "== TSan: queue stress"
g++ --std=c++17 $TSAN -DAPP_HEADLESS -I shim queue_stress.cpp shim/app_shim.cpp shim/app_watch.cpp shim/app_fs.cpp -pthread -o stress.tsan || exit 1
run_tsan stress.tsan stress.tsan.out; rc=$?
races=$(grep -c 'WARNING: ThreadSanitizer' stress.tsan.out)
echo "   exit $rc, $races TSan reports: $(grep 'queue stress' stress.tsan.out | tail -1)"
if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then head -40 stress.tsan.out; fail=1; fi

for variant in work work_pool; do
    echo "== TSan: CeeLing end-to-end tests with worker threads ($variant)"
    build_headless $variant shim "$TSAN" headless.tsan.$variant || { echo "BUILD FAILED"; exit 1; }
    run_tsan headless.tsan.$variant headless.tsan.$variant.out; rc=$?
    races=$(grep -c 'WARNING: ThreadSanitizer' headless.tsan.$variant.out)
    pass=$(grep -c '^PASS ' headless.tsan.$variant.out); bad=$(grep -c '^FAIL ' headless.tsan.$variant.out)
    echo "   exit $rc, $pass passed, $bad failed, $races TSan reports"
    if [ $rc -ne 0 ] || [ "$races" -ne 0 ] || [ "$pass" -ne $EXPECTED ] || [ "$bad" -ne 0 ]; then head -60 headless.tsan.$variant.out; fail=1; fi
done

echo "== TSan negative control 1: producer-side lock removed (queue stress; must report a race)"
g++ --std=c++17 $TSAN -DAPP_HEADLESS -I shim_nolock queue_stress.cpp shim_nolock/app_shim.cpp shim_nolock/app_watch.cpp shim_nolock/app_fs.cpp -pthread -o stress.nolock || exit 1
run_tsan stress.nolock stress.nolock.out
races=$(grep -c 'WARNING: ThreadSanitizer' stress.nolock.out)
echo "   $races TSan reports"
if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run above proves nothing"; fail=1; fi

echo "== TSan negative control 2: reply-side lock removed (CeeLing workers answering concurrently; must report a race)"
build_headless work shim_nolock_ret "$TSAN" headless.nolockret || { echo "BUILD FAILED"; exit 1; }
run_tsan headless.nolockret headless.nolockret.out
races=$(grep -c 'WARNING: ThreadSanitizer' headless.nolockret.out)
echo "   $races TSan reports"
if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run of the CeeLing tests proves nothing"; fail=1; fi
exit $fail
