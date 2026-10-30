#!/usr/bin/env bash
# Build an app-cling program.
#   scripts/build.sh <main.clg> <out.exe> [--headless]
# Every .clg (cjson-cling's library, src/*.clg, the main) is transpiled to its own .cpp/.h, compiled separately and linked
# with the native layer in shim/ (shim/win or shim/posix for the OS). --headless links the test doubles in tests/doubles
# instead of the window and the dialogs (no webview, no window).
# Environment: CLING (compiler), CJSON (path of cjson-cling), CXX (all found automatically when the projects sit side by side), CLING_FLAGS, WORK, PAGE (an .html file to embed as
# app_page_html(); empty if unset). src/bridge.js is always embedded as app_bridge_js().
# RELEASE=1: the window ignores the debug argument of open (no developer tools, no context menu) and the program refuses a development
# server (APP_DEV_URL): compiled out, not a runtime switch. It also strips the symbols (STRIP=1 does that alone).
# ICON=<file.ico>: the program's icon (Windows): compiled into the exe as resource 1, so Explorer shows it, and the window (and so the taskbar) uses it. Without
# it the system's default icon is shown. scripts/make_icon.py makes a placeholder.
# JOBS (parallel compiles, default: the cores), OBJ_CACHE (a folder of compiled objects shared by builds), OPT (optimisation level, default -O1).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/scripts/env.sh"      # CLING, CJSON, CXX: from the environment, else the side-by-side layout (see env.sh)
CLING_FLAGS="${CLING_FLAGS---pool-shared}"
MAIN="$1"; OUT="$2"; MODE="${3:-window}"
WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PLAT=win ;; *) PLAT=posix ;; esac
MODULES="text core files assets settings links placement app"
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
    echo 'std::string app_extra_js() { return std::string(R"CLING_EMBED('
    for x in ${EXTRA_JS:-}; do cat "$x"; echo; done
    echo ')CLING_EMBED"); }'
} > "$EMBED"

# -ffunction-sections / -fdata-sections + --gc-sections drop what nothing uses; RELEASE=1 (or STRIP=1) also strips the symbols
# (smaller, but no backtraces or debugger names: keep a non-release build for that)
SECTIONS="-ffunction-sections -fdata-sections"
GC="-Wl,--gc-sections"
STRIP_FLAG="${RELEASE:+-s}${STRIP:+-s}"
OPT="${OPT:--O1}"        # (the headless test runner uses -O0: it compiles more than twice as fast, and the sanitizer runs cover -O1)
CXXFLAGS="--std=c++17 $OPT -Wno-unknown-pragmas $SECTIONS ${RELEASE:+-DAPP_RELEASE}"
# Speed: the modules are independent once their headers exist, so they are transpiled in parallel, then compiled in parallel (JOBS at a time,
# default: the number of cores), and a compiled object is kept in OBJ_CACHE (if set) under a key made of the source, every generated header and
# the flags, so a second program, or a doctored copy of one file (tests/negative_controls.sh), recompiles only what actually differs.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
FAILED=0
pids=()
spawn() {
    "$@" &
    pids+=($!)
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n || FAILED=1; done
}
finish() {
    local p
    for p in "${pids[@]}"; do wait "$p" || FAILED=1; done
    pids=()
    [ "$FAILED" = 0 ] || { echo "build failed" >&2; exit 1; }
}
transpile() { # module
    "$CLING" $CLING_FLAGS --cpp "$WORK/$1.cpp" "$WORK/$1.clg" > "$WORK/$1.log" 2>&1 || { grep -v Detected "$WORK/$1.log"; return 1; }
    if [ "$1" != "$MAIN_NAME" ]; then
        "$CLING" $CLING_FLAGS --cpp "$WORK/$1.h" "$WORK/$1.clg" >> "$WORK/$1.log" 2>&1 || { grep -v Detected "$WORK/$1.log"; return 1; }
    fi
}
ALL_MODULES="cjson_item cjson_api cjson_parse cjson_print $MODULES $EXTRA_NAMES $MAIN_NAME"
for f in $ALL_MODULES; do spawn transpile "$f"; done
finish
HKEY="$(cat "$WORK"/*.h | md5sum | cut -d' ' -f1)"
cc() { # source object flags...
    local src="$1" obj="$2" key=""
    shift 2
    if [ -n "${OBJ_CACHE:-}" ]; then
        mkdir -p "$OBJ_CACHE"
        key="$({ cat "$src"; echo "$HKEY ${NKEY:-} $CXX $*"; } | md5sum | cut -d' ' -f1)"
        if [ -f "$OBJ_CACHE/$key.o" ] && cp "$OBJ_CACHE/$key.o" "$obj" 2> /dev/null; then return 0; fi     # (a cached object being replaced by another build at this moment is simply compiled again)
    fi
    "$CXX" "$@" -c "$src" -o "$obj" || return 1
    if [ -n "$key" ]; then cp "$obj" "$OBJ_CACHE/$key.$$.tmp" && mv -f "$OBJ_CACHE/$key.$$.tmp" "$OBJ_CACHE/$key.o"; fi
}
OBJS=()
for f in $ALL_MODULES; do
    OBJS+=("$WORK/$f.o")
    spawn cc "$WORK/$f.cpp" "$WORK/$f.o" $CXXFLAGS
done

# the native layer: shared files, the OS's files, and either the window or the test doubles
SHIM="${SHIM:-$HERE/shim}"      # (tests/negative_controls.sh builds against a doctored copy)
DOUBLES="${DOUBLES:-$HERE/tests/doubles}"
# a native source depends on the headers beside it: they are part of its cache key
NKEY="$(cat "$SHIM"/*.h "$SHIM/$PLAT"/*.h "$DOUBLES"/*.h 2>/dev/null | md5sum | cut -d' ' -f1)"
# ASSETS=<folder>: the web files compiled into the program (served from memory; see scripts/pack_assets.sh)
bash "$HERE/scripts/pack_assets.sh" "${ASSETS:--}" "$WORK/assets" || exit 1
NATIVE=("$SHIM/watch.cpp" "$SHIM/assets.cpp" "$WORK/assets/assets_embedded.cpp" "$SHIM/os.cpp" "$SHIM/$PLAT/fs.cpp" "$SHIM/$PLAT/proc.cpp" "$SHIM/$PLAT/watch.cpp")
if [ "$MODE" = "--headless" ]; then
    NATIVE+=("$DOUBLES/headless_shim.cpp" "$DOUBLES/test_support.cpp" "$DOUBLES/instance_double.cpp")
    LINK="-pthread"
else
    INC="-I $HERE/third_party/webview/core/include -I $HERE/third_party/webview2/build/native/include"
    spawn cc "$SHIM/app_shim.cpp" "$WORK/app_shim.o" --std=c++14 -O1 -Wno-unknown-pragmas $SECTIONS ${RELEASE:+-DAPP_RELEASE} $INC
    OBJS+=("$WORK/app_shim.o")
    NATIVE+=("$SHIM/$PLAT/dialog.cpp" "$SHIM/$PLAT/shell.cpp" "$SHIM/$PLAT/instance.cpp")
    [ -f "$SHIM/$PLAT/nav.cpp" ] && NATIVE+=("$SHIM/$PLAT/nav.cpp")
    [ -f "$SHIM/$PLAT/assets.cpp" ] && NATIVE+=("$SHIM/$PLAT/assets.cpp")
    LINK="-static -ladvapi32 -lole32 -lshell32 -lshlwapi -luser32 -lversion -luuid"
fi
for n in "${NATIVE[@]}"; do
    o="$WORK/native_$(basename "$(dirname "$n")")_$(basename "$n" .cpp).o"
    spawn cc "$n" "$o" $CXXFLAGS ${INC:-}
    OBJS+=("$o")
done
spawn cc "$EMBED" "$WORK/embedded.o"
finish
[ -f "$WORK/assets/assets_blob.o" ] && OBJS+=("$WORK/assets/assets_blob.o")
if [ -n "${ICON:-}" ] && [ "$MODE" != "--headless" ] && [ "$PLAT" = win ]; then
    [ -f "$ICON" ] || { echo "ICON $ICON does not exist" >&2; exit 1; }
    [ "$(head -c4 "$ICON" | od -An -tx1 | tr -d ' \n')" = "00000100" ] || { echo "ICON $ICON is not an .ico file" >&2; exit 1; }
    ICON_FULL="$(cygpath -m "$(cd "$(dirname "$ICON")" && pwd)/$(basename "$ICON")")"
    printf '1 ICON "%s"\n' "$ICON_FULL" > "$WORK/icon.rc"
    WINDRES="${WINDRES:-$(dirname "$(command -v "$CXX")")/windres}"
    [ -x "$WINDRES" ] || WINDRES=windres
    "$WINDRES" -O coff -i "$WORK/icon.rc" -o "$WORK/icon.o" || { echo "windres failed for $ICON" >&2; exit 1; }
    OBJS+=("$WORK/icon.o")
fi
"$CXX" "${OBJS[@]}" "$WORK/embedded.o" $GC $STRIP_FLAG $LINK -o "$OUT"
