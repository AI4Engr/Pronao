#!/usr/bin/env bash
# Build an app-cling program.
#   scripts/build.sh <main.clg> <out.exe> [--headless]
# Every .clg (cjson-cling's library, src/*.clg, the main) is transpiled to its own .cpp/.h, compiled separately and linked
# with the native layer in shim/ (shim/win or shim/posix for the OS). --headless links the test doubles in tests/doubles
# instead of the window and the dialogs (no webview, no window).
# Environment: CLING (compiler), CJSON (path of cjson-cling), CXX, CLING_FLAGS, WORK, PAGE (an .html file to embed as
# app_page_html(); empty if unset). src/bridge.js is always embedded as app_bridge_js().
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLING="${CLING:-c:/project/CeeLing/build/cling.exe}"
CJSON="${CJSON:-/c/project/cjson-cling}"
CXX="${CXX:-g++}"
CLING_FLAGS="${CLING_FLAGS---pool-shared}"
MAIN="$1"; OUT="$2"; MODE="${3:-window}"
WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PLAT=win ;; *) PLAT=posix ;; esac
MODULES="text core files app"
# EXTRA: more modules (paths to .clg files, space separated) that sit between the library and the main program
EXTRA_NAMES=""
for x in ${EXTRA:-}; do EXTRA_NAMES="$EXTRA_NAMES $(basename "$x" .clg)"; done
# generated code includes "../shim/cjson_num.h": give the work directory a sibling shim/ with it
SHIMDIR="$(cd "$WORK/.." && pwd)/shim"
mkdir -p "$SHIMDIR"
cp "$CJSON"/shim/cjson_num.h "$SHIMDIR/"

sync() { cp "$1" "$2"; }
for f in cjson_item cjson_api cjson_parse cjson_print; do sync "$CJSON/src/$f.clg" "$WORK/$f.clg"; done
SRC="${SRC:-$HERE/src}"      # (tests/negative_controls.sh builds from a doctored copy)
for f in $MODULES; do sync "$SRC/$f.clg" "$WORK/$f.clg"; done
for x in ${EXTRA:-}; do sync "$x" "$WORK/$(basename "$x")"; done
MAIN_NAME="$(basename "$MAIN" .clg)"
sync "$MAIN" "$WORK/$MAIN_NAME.clg"

# the page and the bridge script are compiled in as two strings (raw literals), so a program is one file
EMBED="$WORK/embedded.cpp"
{
    echo '#include <string>'
    echo 'std::string app_bridge_js() { return std::string(R"CLING_EMBED('
    cat "$SRC/bridge.js"
    echo ')CLING_EMBED"); }'
    echo 'std::string app_page_html() { return std::string(R"CLING_EMBED('
    if [ -n "${PAGE:-}" ]; then cat "$PAGE"; fi
    echo ')CLING_EMBED"); }'
} > "$EMBED"

CXXFLAGS="--std=c++17 -O1 -Wno-unknown-pragmas"
OBJS=()
for f in cjson_item cjson_api cjson_parse cjson_print $MODULES $EXTRA_NAMES "$MAIN_NAME"; do
    OBJS+=("$WORK/$f.o")
    "$CLING" $CLING_FLAGS --cpp "$WORK/$f.cpp" "$WORK/$f.clg" > "$WORK/$f.log" 2>&1 || { grep -v Detected "$WORK/$f.log"; exit 1; }
    if [ "$f" != "$MAIN_NAME" ]; then
        "$CLING" $CLING_FLAGS --cpp "$WORK/$f.h" "$WORK/$f.clg" >> "$WORK/$f.log" 2>&1 || { grep -v Detected "$WORK/$f.log"; exit 1; }
    fi
    "$CXX" $CXXFLAGS -c "$WORK/$f.cpp" -o "$WORK/$f.o"
done

# the native layer: shared files, the OS's files, and either the window or the test doubles
SHIM="${SHIM:-$HERE/shim}"      # (tests/negative_controls.sh builds against a doctored copy)
NATIVE=("$SHIM/watch.cpp" "$SHIM/os.cpp" "$SHIM/$PLAT/fs.cpp" "$SHIM/$PLAT/proc.cpp" "$SHIM/$PLAT/watch.cpp")
if [ "$MODE" = "--headless" ]; then
    DOUBLES="${DOUBLES:-$HERE/tests/doubles}"
    NATIVE+=("$DOUBLES/headless_shim.cpp" "$DOUBLES/test_support.cpp")
    LINK="-pthread"
else
    INC="-I $HERE/third_party/webview/core/include -I $HERE/third_party/webview2/build/native/include"
    "$CXX" --std=c++14 -O1 -Wno-unknown-pragmas $INC -c "$SHIM/app_shim.cpp" -o "$WORK/app_shim.o"
    OBJS+=("$WORK/app_shim.o")
    NATIVE+=("$SHIM/$PLAT/dialog.cpp")
    [ -f "$SHIM/$PLAT/nav.cpp" ] && NATIVE+=("$SHIM/$PLAT/nav.cpp")
    LINK="-static -ladvapi32 -lole32 -lshell32 -lshlwapi -luser32 -lversion -luuid"
fi
for n in "${NATIVE[@]}"; do
    o="$WORK/native_$(basename "$(dirname "$n")")_$(basename "$n" .cpp).o"
    "$CXX" $CXXFLAGS ${INC:-} -c "$n" -o "$o"
    OBJS+=("$o")
done
"$CXX" -c "$EMBED" -o "$WORK/embedded.o"
"$CXX" "${OBJS[@]}" "$WORK/embedded.o" $LINK -o "$OUT"
