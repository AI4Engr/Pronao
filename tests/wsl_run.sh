#!/usr/bin/env bash
# The WSL half of tests/run_sanitizers.sh: compiles and runs everything under the sanitizers.
#   wsl_run.sh <directory prepared by run_sanitizers.sh>
set -uo pipefail
cd "$1"
fail=0
ASAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1"
TSAN="-fsanitize=thread -fno-omit-frame-pointer -g -O1"

echo "== ASan+UBSan: CeeLing end-to-end tests"
objs=""
for f in cjson_item cjson_api cjson_parse cjson_print app headless_main embedded; do
    g++ --std=c++17 $ASAN -Wno-unknown-pragmas -c work/$f.cpp -o work/$f.asan.o || { echo "COMPILE FAILED $f"; exit 1; }
    objs="$objs work/$f.asan.o"
done
g++ --std=c++14 $ASAN -DAPP_HEADLESS -I shim -c shim/app_shim.cpp -o work/shim.asan.o || exit 1
g++ $ASAN $objs work/shim.asan.o -pthread -o headless.asan || exit 1
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 ./headless.asan > headless.asan.out 2>&1
rc=$?; pass=$(grep -c '^PASS ' headless.asan.out); bad=$(grep -c '^FAIL ' headless.asan.out)
echo "   exit $rc, $pass passed, $bad failed"
if [ $rc -ne 0 ] || [ "$pass" -ne 19 ] || [ "$bad" -ne 0 ]; then tail -30 headless.asan.out; fail=1; fi

echo "== ASan+UBSan: queue stress"
g++ --std=c++14 $ASAN -DAPP_HEADLESS -I shim queue_stress.cpp shim/app_shim.cpp -pthread -o stress.asan || exit 1
ASAN_OPTIONS=detect_leaks=1 ./stress.asan > stress.asan.out 2>&1; rc=$?
echo "   exit $rc: $(tail -1 stress.asan.out)"
[ $rc -eq 0 ] || { tail -30 stress.asan.out; fail=1; }

# TSan needs ASLR off on some WSL kernels (its shadow-memory mapping fails otherwise)
echo "== TSan: queue stress"
g++ --std=c++14 $TSAN -DAPP_HEADLESS -I shim queue_stress.cpp shim/app_shim.cpp -pthread -o stress.tsan || exit 1
TSAN_OPTIONS=halt_on_error=0 setarch "$(uname -m)" -R ./stress.tsan > stress.tsan.out 2>&1; rc=$?
races=$(grep -c 'WARNING: ThreadSanitizer' stress.tsan.out)
echo "   exit $rc, $races TSan reports: $(grep 'queue stress' stress.tsan.out | tail -1)"
if [ $rc -ne 0 ] || [ "$races" -ne 0 ]; then head -40 stress.tsan.out; fail=1; fi

echo "== TSan negative control: the producer-side lock removed (must report a race)"
g++ --std=c++14 $TSAN -DAPP_HEADLESS -I shim_nolock queue_stress.cpp shim_nolock/app_shim.cpp -pthread -o stress.nolock || exit 1
TSAN_OPTIONS=halt_on_error=0 setarch "$(uname -m)" -R ./stress.nolock > stress.nolock.out 2>&1
races=$(grep -c 'WARNING: ThreadSanitizer' stress.nolock.out)
echo "   $races TSan reports"
if [ "$races" -eq 0 ]; then echo "   NEGATIVE CONTROL DID NOT FAIL -- the TSan run above proves nothing"; fail=1; fi
exit $fail
