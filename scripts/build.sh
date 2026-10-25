#!/usr/bin/env bash
# Build an app-cling program.
#   scripts/build.sh <main.clg> <out.exe> [--headless]
# Every .clg (cjson-cling's library, src/app.clg, the main) is transpiled to its own .cpp/.h, compiled
# separately and linked with the shim. --headless links the test shim (no webview, no window).
# Environment: CLING (compiler), CJSON (path of cjson-cling), CXX, CLING_FLAGS, WORK, PAGE (an .html file to
# embed as app_page_html(); empty if unset). src/bridge.js is always embedded as app_bridge_js().
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLING="${CLING:-c:/project/CeeLing/build/cling.exe}"
CJSON="${CJSON:-/c/project/cjson-cling}"
CXX="${CXX:-g++}"
CLING_FLAGS="${CLING_FLAGS---pool-shared}"
MAIN="$1"; OUT="$2"; MODE="${3:-window}"
WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
# generated code includes "../shim/<header>": give the work directory a sibling shim/ with those headers
SHIMDIR="$(cd "$WORK/.." && pwd)/shim"
mkdir -p "$SHIMDIR"
cp "$CJSON"/shim/cjson_num.h "$HERE"/shim/app_shim.h "$HERE"/shim/app_fs.h "$SHIMDIR/"

for f in cjson_item cjson_api cjson_parse cjson_print; do cp "$CJSON/src/$f.clg" "$WORK/"; done
cp "$HERE/src/app.clg" "$WORK/"
MAIN_NAME="$(basename "$MAIN" .clg)"
cp "$MAIN" "$WORK/$MAIN_NAME.clg"

# the page and the bridge script are compiled in as two strings (raw literals), so a program is one file
EMBED="$WORK/embedded.cpp"
{
    echo '#include <string>'
    echo 'std::string app_bridge_js() { return std::string(R"CLING_EMBED('
    cat "$HERE/src/bridge.js"
    echo ')CLING_EMBED"); }'
    echo 'std::string app_page_html() { return std::string(R"CLING_EMBED('
    if [ -n "${PAGE:-}" ]; then cat "$PAGE"; fi
    echo ')CLING_EMBED"); }'
} > "$EMBED"

CXXFLAGS="--std=c++17 -O1 -Wno-unknown-pragmas"
OBJS=()
for f in cjson_item cjson_api cjson_parse cjson_print app "$MAIN_NAME"; do
    "$CLING" $CLING_FLAGS --cpp "$WORK/$f.cpp" "$WORK/$f.clg" > "$WORK/$f.log" 2>&1 || { grep -v Detected "$WORK/$f.log"; exit 1; }
    if [ "$f" != "$MAIN_NAME" ]; then
        "$CLING" $CLING_FLAGS --cpp "$WORK/$f.h" "$WORK/$f.clg" >> "$WORK/$f.log" 2>&1 || { grep -v Detected "$WORK/$f.log"; exit 1; }
    fi
    "$CXX" $CXXFLAGS -c "$WORK/$f.cpp" -o "$WORK/$f.o"
    OBJS+=("$WORK/$f.o")
done

if [ "$MODE" = "--headless" ]; then
    "$CXX" --std=c++14 -O1 -DAPP_HEADLESS -I "$HERE/shim" -c "$HERE/shim/app_shim.cpp" -o "$WORK/app_shim.o"
    "$CXX" --std=c++17 -O1 -DAPP_HEADLESS -I "$HERE/shim" -c "$HERE/shim/app_fs.cpp" -o "$WORK/app_fs.o"
    "$CXX" -c "$EMBED" -o "$WORK/embedded.o"
    "$CXX" "${OBJS[@]}" "$WORK/app_shim.o" "$WORK/app_fs.o" "$WORK/embedded.o" -pthread -o "$OUT"
else
    INC="-I $HERE/shim -I $HERE/third_party/webview/core/include -I $HERE/third_party/webview2/build/native/include"
    "$CXX" --std=c++14 -O1 -Wno-unknown-pragmas $INC -c "$HERE/shim/app_shim.cpp" -o "$WORK/app_shim.o"
    "$CXX" --std=c++17 -O1 -I "$HERE/shim" -c "$HERE/shim/app_fs.cpp" -o "$WORK/app_fs.o"
    "$CXX" -c "$EMBED" -o "$WORK/embedded.o"
    "$CXX" "${OBJS[@]}" "$WORK/app_shim.o" "$WORK/app_fs.o" "$WORK/embedded.o" -static -ladvapi32 -lole32 -lshell32 -lshlwapi -luser32 -lversion -o "$OUT"
fi
