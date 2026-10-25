#!/usr/bin/env bash
# Sanitizer runs for the headless build, in WSL (the MinGW toolchain on the dev machine has no sanitizer runtime).
#   tests/run_sanitizers.sh
# 1. ASan + UBSan over the CeeLing end-to-end tests (built WITHOUT --pool-shared: the pool would hide a
#    use-after-free) and over the queue stress test.
# 2. ThreadSanitizer over the queue stress test.
# 3. A NEGATIVE CONTROL: the same TSan run against a shim whose inject path has its mutex removed. It MUST report
#    a data race; if it does not, the TSan run above proves nothing and this script fails.
# Only the shim and the CeeLing layer are covered; webview and the browser engine are not built here.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="/c/apps/msys64/mingw64/bin:$PATH"
CLING="${CLING:-c:/project/CeeLing/build/cling.exe}"
CJSON="${CJSON:-/c/project/cjson-cling}"
BASE="${BASE:-/c/tmp/app-cling-san}"
rm -rf "$BASE"; mkdir -p "$BASE/work" "$BASE/shim" "$BASE/shim_nolock"
cp "$CJSON"/shim/cjson_num.h "$HERE"/shim/app_shim.h "$BASE/shim/"
for f in cjson_item cjson_api cjson_parse cjson_print; do cp "$CJSON/src/$f.clg" "$BASE/work/"; done
cp "$HERE/src/app.clg" "$HERE/tests/headless_main.clg" "$BASE/work/"
for f in cjson_item cjson_api cjson_parse cjson_print app headless_main; do
    "$CLING" --cpp "$BASE/work/$f.cpp" "$BASE/work/$f.clg" > "$BASE/work/$f.log" 2>&1 || { grep -v Detected "$BASE/work/$f.log"; echo "TRANSPILE FAILED: $f"; exit 1; }
    if [ "$f" != "headless_main" ]; then
        "$CLING" --cpp "$BASE/work/$f.h" "$BASE/work/$f.clg" >> "$BASE/work/$f.log" 2>&1 || { echo "TRANSPILE FAILED (header): $f"; exit 1; }
    fi
done
{
    echo '#include <string>'
    echo 'std::string app_bridge_js() { return ""; }'
    echo 'std::string app_page_html() { return ""; }'
} > "$BASE/work/embedded.cpp"
cp "$HERE/shim/app_shim.cpp" "$BASE/shim/"
cp "$HERE/tests/queue_stress.cpp" "$HERE/tests/wsl_run.sh" "$BASE/"
# the negative control's shim: drop the lock in app_test_inject (the producer side of the queue)
awk '/^int app_test_inject/ {infn=1} infn && /std::lock_guard<std::mutex> lock\(app->m\);/ {infn=0; next} {print}' \
    "$HERE/shim/app_shim.cpp" > "$BASE/shim_nolock/app_shim.cpp"
cp "$HERE/shim/app_shim.h" "$BASE/shim_nolock/"
if cmp -s "$HERE/shim/app_shim.cpp" "$BASE/shim_nolock/app_shim.cpp"; then echo "NEGATIVE CONTROL SETUP FAILED (nothing removed)"; exit 1; fi

MSYS_NO_PATHCONV=1 wsl -d Ubuntu-22.04 -- bash /mnt/c/tmp/app-cling-san/wsl_run.sh /mnt/c/tmp/app-cling-san
rc=$?
if [ $rc -eq 0 ]; then echo "sanitizers: ALL OK"; else echo "sanitizers: FAILED"; fi
exit $rc
