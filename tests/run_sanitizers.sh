#!/usr/bin/env bash
# Sanitizer runs for the headless build, in WSL (the MinGW toolchain on the dev machine has no sanitizer runtime).
#   tests/run_sanitizers.sh
# 1. ASan + UBSan over the CeeLing end-to-end tests (built WITHOUT --pool-shared: the pool would hide a
#    use-after-free) and over the queue stress test.
# 2. ThreadSanitizer over the queue stress test AND over the CeeLing end-to-end tests, which now start real worker
#    threads (spawn), both without and with --pool-shared.
# 3. NEGATIVE CONTROLS: the same TSan runs against shims whose mutex is removed (producer side; reply side). Each
#    MUST report a data race; if one does not, the TSan run it controls proves nothing and this script fails.
# Only the shim and the CeeLing layer are covered; webview and the browser engine are not built here.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="/c/apps/msys64/mingw64/bin:$PATH"
CLING="${CLING:-c:/project/CeeLing/build/cling.exe}"
CJSON="${CJSON:-/c/project/cjson-cling}"
BASE="${BASE:-/c/tmp/app-cling-san}"
rm -rf "$BASE"; mkdir -p "$BASE/shim" "$BASE/shim_nolock" "$BASE/shim_nolock_ret"
cp "$CJSON"/shim/cjson_num.h "$HERE"/shim/app_shim.h "$BASE/shim/"

# transpile everything twice: plain (work) and with the pooled allocator (work_pool)
for variant in work work_pool; do
    flags=""; [ "$variant" = "work_pool" ] && flags="--pool-shared"
    mkdir -p "$BASE/$variant"
    for f in cjson_item cjson_api cjson_parse cjson_print; do cp "$CJSON/src/$f.clg" "$BASE/$variant/"; done
    cp "$HERE/src/app.clg" "$HERE/tests/headless_main.clg" "$BASE/$variant/"
    for f in cjson_item cjson_api cjson_parse cjson_print app headless_main; do
        "$CLING" $flags --cpp "$BASE/$variant/$f.cpp" "$BASE/$variant/$f.clg" > "$BASE/$variant/$f.log" 2>&1 || { grep -v Detected "$BASE/$variant/$f.log"; echo "TRANSPILE FAILED: $variant/$f"; exit 1; }
        if [ "$f" != "headless_main" ]; then
            "$CLING" $flags --cpp "$BASE/$variant/$f.h" "$BASE/$variant/$f.clg" >> "$BASE/$variant/$f.log" 2>&1 || { echo "TRANSPILE FAILED (header): $variant/$f"; exit 1; }
        fi
    done
    {
        echo '#include <string>'
        echo 'std::string app_bridge_js() { return ""; }'
        echo 'std::string app_page_html() { return ""; }'
    } > "$BASE/$variant/embedded.cpp"
done
cp "$HERE/shim/app_shim.cpp" "$BASE/shim/"
cp "$HERE/tests/queue_stress.cpp" "$HERE/tests/wsl_run.sh" "$BASE/"
# negative controls: a shim with the lock removed in one function
dropLock() { # <function name prefix> <out dir>
    awk -v fn="$1" '$0 ~ "^int " fn {infn=1} infn && /std::lock_guard<std::mutex> lock\(app->m\);/ {infn=0; next} {print}' \
        "$HERE/shim/app_shim.cpp" > "$2/app_shim.cpp"
    cp "$HERE/shim/app_shim.h" "$2/"
    if cmp -s "$HERE/shim/app_shim.cpp" "$2/app_shim.cpp"; then echo "NEGATIVE CONTROL SETUP FAILED ($1: nothing removed)"; exit 1; fi
}
dropLock app_test_inject "$BASE/shim_nolock"
dropLock app_return "$BASE/shim_nolock_ret"

MSYS_NO_PATHCONV=1 wsl -d Ubuntu-22.04 -- bash /mnt/c/tmp/app-cling-san/wsl_run.sh /mnt/c/tmp/app-cling-san
rc=$?
if [ $rc -eq 0 ]; then echo "sanitizers: ALL OK"; else echo "sanitizers: FAILED"; fi
exit $rc
