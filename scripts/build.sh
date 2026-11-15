#!/usr/bin/env bash
# Build a Pronao program.
#   scripts/build.sh <main.clg or main.cpp> <out.exe> [--headless]
# A main.cpp is plain C++ that includes the generated headers (app.h, core.h, cjson_item.h ...) and calls the framework directly; it is compiled with -I <work dir>.
# Every .clg (cjson-cling's library, src/*.clg, the main) is transpiled to its own .cpp/.h, compiled separately and linked
# with the native layer in shim/ (shim/win or shim/posix for the OS). --headless links the test doubles in tests/doubles
# instead of the window and the dialogs (no webview, no window).
# Environment: CLING (compiler), CJSON (path of cjson-cling), CXX (all found automatically when the projects sit side by side), CLING_FLAGS, WORK, PAGE (an .html file to embed as
# app_page_html(); empty if unset). src/bridge.js is always embedded as app_bridge_js().
# RELEASE=1: the window ignores the debug argument of open (no developer tools, no context menu) and the program refuses a development
# server (APP_DEV_URL): compiled out, not a runtime switch. It also strips the symbols (STRIP=1 does that alone).
# GUI=1 (and RELEASE=1, unless GUI=0): a program for people, not for a console: it is linked as a Windows (not console) program, so double-clicking it opens no black
# console window. Its output (std::cout) goes nowhere then, which is why the tests use the console build.
# APP_VERSION=1.2.3 (APP_NAME, APP_COMPANY, APP_DESCRIPTION, APP_COPYRIGHT): the version resource of the exe, shown by Explorer under Properties > Details.
# ICON=<file.ico>: the program's icon (Windows): compiled into the exe as resource 1, so Explorer shows it, and the window (and so the taskbar) uses it. Without
# it the system's default icon is shown. scripts/make_icon.py makes a placeholder. On Linux ICON is a .png and only the window uses it (see below in the script).
# SIGN_COMMAND='signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /sha1 THUMBPRINT %1' (RELEASE=1 only): run as the very last step, on the finished exe, with %1
# replaced by its path (as one argument, whatever characters the path has; write %1 bare, not in quotes). Any signing tool works: signtool, relic, artifact-signing-cli, osslsigncode,
# a script. SIGN_VERIFY_COMMAND (same %1) runs after it, for example 'signtool verify /pa %1'. If either fails, or the signing command leaves the exe unchanged, the build FAILS and the
# unsigned exe is deleted, so no exe that looks like a release is left behind. The commands are not printed (they may hold a key or a password); their own output is shown.
# JOBS (parallel compiles, default: the cores), OBJ_CACHE (a folder of compiled objects shared by builds), OPT (optimisation level, default -O1).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/scripts/env.sh"      # CLING, CJSON, CXX: from the environment, else the side-by-side layout (see env.sh)
CLING_FLAGS="${CLING_FLAGS---pool-shared}"
MAIN="$1"; OUT="$2"; MODE="${3:-window}"
# the signing hook is checked BEFORE anything is built: a mistake in it must not cost a whole build
if [ -n "${SIGN_COMMAND:-}${SIGN_VERIFY_COMMAND:-}" ]; then
    [ "$MODE" != "--headless" ] || { echo "build.sh: SIGN_COMMAND is for a program for people; a --headless (test) build is not signed" >&2; exit 1; }
    [ -n "${RELEASE:-}" ] || { echo "build.sh: SIGN_COMMAND needs RELEASE=1 (a build with developer tools must not be signed as if it were the release)" >&2; exit 1; }
    for v in SIGN_COMMAND SIGN_VERIFY_COMMAND; do
        if [ -n "${!v:-}" ]; then
            case "${!v}" in *%1*) ;; *) echo "build.sh: $v must contain %1, the place where the path of the exe goes" >&2; exit 1 ;; esac
        fi
    done
    [ -n "${SIGN_COMMAND:-}" ] || { echo "build.sh: SIGN_VERIFY_COMMAND without SIGN_COMMAND: there is nothing to verify" >&2; exit 1; }
fi
WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PLAT=win ;; *) PLAT=posix ;; esac
. "$HERE/scripts/modules.sh"
MODULES="$APP_MODULES"
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
CPP_MAIN=""; case "$MAIN" in *.cpp) CPP_MAIN=1 ;; esac
if [ -n "$CPP_MAIN" ]; then
    MAIN_NAME=""            # (a C++ main is compiled as it is, below: nothing to transpile)
else
    MAIN_NAME="$(basename "$MAIN" .clg)"
    sync "$MAIN" "$WORK/$MAIN_NAME.clg"
fi

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
STRIP_FLAG=""; [ -n "${RELEASE:-}${STRIP:-}" ] && STRIP_FLAG="-s"
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
# The modules of src/ are compiled one by one, and CeeLing chooses unique_ptr or shared_ptr per file: what one module needs shared from a class that another one declares is
# worked out first (scripts/pointer_demands.sh) and every file, the program's own included, is then compiled knowing it. Needs a compiler with --ptr-demands.
JOBS="$JOBS" bash "$HERE/scripts/pointer_demands.sh" "$CLING" "$WORK" "$CLING_FLAGS" "$MODULES" || exit 1
CLING_FLAGS="$CLING_FLAGS --ptr-demands $WORK/demands.txt"
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
if [ -n "$CPP_MAIN" ]; then
    OBJS+=("$WORK/main_cpp.o")
    spawn cc "$MAIN" "$WORK/main_cpp.o" $CXXFLAGS -I "$WORK"
fi

# the native layer: shared files, the OS's files, and either the window or the test doubles
SHIM="${SHIM:-$HERE/shim}"      # (tests/negative_controls.sh builds against a doctored copy)
DOUBLES="${DOUBLES:-$HERE/tests/doubles}"
# a native source depends on the headers beside it: they are part of its cache key
# (shim/posix has no .h: the failing cat of an empty glob must not end the script under pipefail)
NKEY="$( { cat "$SHIM"/*.h "$SHIM/$PLAT"/*.h "$DOUBLES"/*.h 2>/dev/null || true; } | md5sum | cut -d' ' -f1)"
# ASSETS=<folder>: the web files compiled into the program (served from memory; see scripts/pack_assets.sh)
bash "$HERE/scripts/pack_assets.sh" "${ASSETS:--}" "$WORK/assets" || exit 1
NATIVE=("$SHIM/watch.cpp" "$SHIM/assets.cpp" "$WORK/assets/assets_embedded.cpp" "$SHIM/os.cpp" "$SHIM/$PLAT/fs.cpp" "$SHIM/$PLAT/proc.cpp" "$SHIM/$PLAT/watch.cpp")
if [ "$MODE" = "--headless" ]; then
    NATIVE+=("$DOUBLES/headless_shim.cpp" "$DOUBLES/test_support.cpp" "$DOUBLES/instance_double.cpp" "$DOUBLES/http_double.cpp")
    LINK="-pthread -lz"
else
    INC="-I $HERE/third_party/webview/core/include -I $HERE/third_party/webview2/build/native/include"
    GTKLIBS=""
    if [ "$PLAT" = posix ]; then
        # the window is GTK 3 with WebKitGTK (the webview library's Linux backend): 4.1 if it is installed, else 4.0
        WEBKIT=""; for v in webkit2gtk-4.1 webkit2gtk-4.0; do pkg-config --exists "$v" 2>/dev/null && { WEBKIT="$v"; break; }; done
        [ -n "$WEBKIT" ] && pkg-config --exists gtk+-3.0 || { echo "build.sh: a window on Linux needs GTK 3 and WebKitGTK: sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev (or -4.0-dev)" >&2; exit 1; }
        INC="$INC $(pkg-config --cflags gtk+-3.0 "$WEBKIT")"
        GTKLIBS="$(pkg-config --libs gtk+-3.0 "$WEBKIT")"
    fi
    spawn cc "$SHIM/app_shim.cpp" "$WORK/app_shim.o" --std=c++14 -O1 -Wno-unknown-pragmas $SECTIONS ${RELEASE:+-DAPP_RELEASE} $INC
    OBJS+=("$WORK/app_shim.o")
    NATIVE+=("$SHIM/$PLAT/dialog.cpp" "$SHIM/$PLAT/shell.cpp" "$SHIM/$PLAT/instance.cpp" "$SHIM/$PLAT/menu.cpp" "$SHIM/$PLAT/runtime.cpp")
    [ -f "$SHIM/$PLAT/nav.cpp" ] && NATIVE+=("$SHIM/$PLAT/nav.cpp")
    [ -f "$SHIM/$PLAT/tray.cpp" ] && NATIVE+=("$SHIM/$PLAT/tray.cpp")
    [ -f "$SHIM/$PLAT/theme.cpp" ] && NATIVE+=("$SHIM/$PLAT/theme.cpp")
    [ -f "$SHIM/$PLAT/drop.cpp" ] && NATIVE+=("$SHIM/$PLAT/drop.cpp")
    NATIVE+=("$SHIM/$PLAT/http.cpp")
    [ -f "$SHIM/$PLAT/assets.cpp" ] && NATIVE+=("$SHIM/$PLAT/assets.cpp")
    [ -f "$SHIM/$PLAT/window.cpp" ] && NATIVE+=("$SHIM/$PLAT/window.cpp")
    [ -f "$SHIM/$PLAT/gtkmenu.cpp" ] && NATIVE+=("$SHIM/$PLAT/gtkmenu.cpp")
    [ -f "$SHIM/$PLAT/notify.cpp" ] && NATIVE+=("$SHIM/$PLAT/notify.cpp")
    if [ "$PLAT" = win ]; then
        LINK="-static -lz -lwinhttp -ladvapi32 -lole32 -lshell32 -lshlwapi -luser32 -lversion -luuid"
        if [ "${GUI:-${RELEASE:+1}}" = 1 ]; then LINK="-mwindows $LINK"; fi
    else
        LINK="-pthread -lz -ldl $GTKLIBS"
    fi
fi
for n in "${NATIVE[@]}"; do
    o="$WORK/native_$(basename "$(dirname "$n")")_$(basename "$n" .cpp).o"
    spawn cc "$n" "$o" $CXXFLAGS ${INC:-}
    OBJS+=("$o")
done
spawn cc "$EMBED" "$WORK/embedded.o"
finish
[ -f "$WORK/assets/assets_blob.o" ] && OBJS+=("$WORK/assets/assets_blob.o")
if [ -n "${ICON:-}" ] && [ "$MODE" != "--headless" ] && [ "$PLAT" != win ]; then
    # Linux: ICON is a .png (at least 48x48 is best), compiled in as a byte array; the window uses it (the title bar, the task switcher, the taskbar of most desktops).
    # A desktop shows the icon of an INSTALLED program from its .desktop file, not from the executable: that file is the packager's (docs/manual/10-distribution.md).
    [ -f "$ICON" ] || { echo "ICON $ICON does not exist" >&2; exit 1; }
    [ "$(head -c8 "$ICON" | od -An -tx1 | tr -d ' 
')" = "89504e470d0a1a0a" ] || { echo "ICON $ICON is not a .png file (the Linux build takes a PNG)" >&2; exit 1; }
    {
        echo 'extern const unsigned char app_icon_png[] = {'
        od -An -v -tu1 "$ICON" | tr -s ' 
' ',' | sed 's/^,//'
        echo '0};'
        echo "extern const unsigned long app_icon_png_size = $(stat -c %s "$ICON");"
    } > "$WORK/icon_data.cpp"
    "$CXX" -c "$WORK/icon_data.cpp" -o "$WORK/icon_data.o" || { echo "the icon could not be compiled in" >&2; exit 1; }
    OBJS+=("$WORK/icon_data.o")
fi
if { [ -n "${ICON:-}" ] || [ -n "${APP_VERSION:-}" ]; } && [ "$MODE" != "--headless" ] && [ "$PLAT" = win ]; then
    : > "$WORK/icon.rc"
    if [ -n "${ICON:-}" ]; then
        [ -f "$ICON" ] || { echo "ICON $ICON does not exist" >&2; exit 1; }
        [ "$(head -c4 "$ICON" | od -An -tx1 | tr -d ' \n')" = "00000100" ] || { echo "ICON $ICON is not an .ico file" >&2; exit 1; }
        ICON_FULL="$(cygpath -m "$(cd "$(dirname "$ICON")" && pwd)/$(basename "$ICON")")"
        printf '1 ICON "%s"\n' "$ICON_FULL" >> "$WORK/icon.rc"
    fi
    if [ -n "${APP_VERSION:-}" ]; then
        # the version resource: what Explorer shows under Properties > Details (and what SmartScreen and virus scanners read about a program with no signature)
        case "$APP_VERSION" in *[!0-9.]*|""|.*|*.|*..*) echo "APP_VERSION must be numbers and dots, like 1.2.3 (got $APP_VERSION)" >&2; exit 1 ;; esac
        IFS=. read -r V1 V2 V3 V4 <<< "$APP_VERSION"
        VCOMMA="${V1:-0},${V2:-0},${V3:-0},${V4:-0}"
        vtext() { # NAME -> the value, which may hold letters, digits, spaces and . , - _ ( ) only (nothing that could end the string or the line in the .rc)
            case "$2" in *[!A-Za-z0-9\ .,_\(\)-]*) echo "$1 may contain only letters, digits, spaces and . , _ - ( )" >&2; exit 1 ;; esac
            printf '%s' "$2"
        }
        P_NAME="$(vtext APP_NAME "${APP_NAME:-$(basename "$OUT" .exe)}")"
        P_COMPANY="$(vtext APP_COMPANY "${APP_COMPANY:-}")"
        P_DESC="$(vtext APP_DESCRIPTION "${APP_DESCRIPTION:-$P_NAME}")"
        P_COPY="$(vtext APP_COPYRIGHT "${APP_COPYRIGHT:-}")"
        cat >> "$WORK/icon.rc" <<RCEOF
1 VERSIONINFO
FILEVERSION $VCOMMA
PRODUCTVERSION $VCOMMA
FILEOS 0x4
FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "$P_COMPANY"
      VALUE "FileDescription", "$P_DESC"
      VALUE "FileVersion", "$APP_VERSION"
      VALUE "InternalName", "$P_NAME"
      VALUE "LegalCopyright", "$P_COPY"
      VALUE "OriginalFilename", "$(basename "$OUT")"
      VALUE "ProductName", "$P_NAME"
      VALUE "ProductVersion", "$APP_VERSION"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RCEOF
    fi
    WINDRES="${WINDRES:-$(dirname "$(command -v "$CXX")")/windres}"
    [ -x "$WINDRES" ] || WINDRES=windres
    "$WINDRES" -O coff -i "$WORK/icon.rc" -o "$WORK/icon.o" || { echo "windres failed for the icon / version resource" >&2; exit 1; }
    OBJS+=("$WORK/icon.o")
fi
"$CXX" "${OBJS[@]}" "$WORK/embedded.o" $GC $STRIP_FLAG $LINK -o "$OUT"

# ---- signing, last: nothing may touch the exe after this (a change breaks the signature) --------------------------------------------------------------------------------------------
if [ -n "${SIGN_COMMAND:-}" ]; then
    # %1 becomes "$1" of a fresh shell that is handed the path: the path is one argument whatever it holds (spaces, ;, $, quotes), never part of the command's text.
    # (A "%1" the user quoted anyway is taken as the same thing.)
    run_with_exe() {
        local cmd="${1//\"%1\"/%1}"
        cmd="${cmd//%1/\"\$1\"}"
        bash -c "$cmd" signing-hook "$OUT"
    }
    before="$(sha256sum "$OUT" | cut -d' ' -f1)"
    if ! run_with_exe "$SIGN_COMMAND"; then
        rm -f "$OUT"; echo "build.sh: the signing command failed; the unsigned $(basename "$OUT") was deleted" >&2; exit 1
    fi
    after="$(sha256sum "$OUT" 2>/dev/null | cut -d' ' -f1)"
    if [ -z "$after" ] || [ "$after" = "$before" ]; then
        rm -f "$OUT"; echo "build.sh: the signing command succeeded but did not change the exe (nothing was signed); the exe was deleted" >&2; exit 1
    fi
    if [ -n "${SIGN_VERIFY_COMMAND:-}" ] && ! run_with_exe "$SIGN_VERIFY_COMMAND"; then
        rm -f "$OUT"; echo "build.sh: the signature did not verify; the exe was deleted" >&2; exit 1
    fi
    echo "signed: $OUT"
fi
