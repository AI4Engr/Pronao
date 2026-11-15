#!/usr/bin/env bash
# One command from a fresh clone to a working build: checks the tools, clones CeeLing and cJSON-cling next to this
# repository when they are missing, builds the CeeLing compiler, downloads webview (and, on Windows, the WebView2 SDK).
#   bash scripts/setup.sh
# CEELING_URL / CJSON_URL override where the two repositories are cloned from. Nothing is installed system-wide, and
# a compiler, cmake, git, curl or Python that is missing is reported, not fetched.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARENT="$(dirname "$HERE")"
CEELING_URL="${CEELING_URL:-https://github.com/AI4Engr/CeeLing.git}"
CJSON_URL="${CJSON_URL:-https://github.com/AI4Engr/cJSON-cling.git}"
missing=0
need() { command -v "$1" >/dev/null 2>&1 || { echo "setup: '$1' is not on PATH. $2" >&2; missing=1; }; }
need git "Install Git."
need cmake "Install CMake (on Windows: pacman -S mingw-w64-x86_64-cmake in MSYS2)."
need "${CXX:-g++}" "On Windows install MinGW-w64 (MSYS2) and put its bin folder on PATH."
need curl "fetch.sh downloads with curl."
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        command -v python3 >/dev/null 2>&1 || command -v python >/dev/null 2>&1 || { echo "setup: Python 3 is not on PATH (fetch.sh needs it to unpack the WebView2 package)." >&2; missing=1; } ;;
    *)
        need pkg-config "sudo apt install pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev (the window is GTK 3 + WebKitGTK)."
        if command -v pkg-config >/dev/null 2>&1; then
            { pkg-config --exists gtk+-3.0 && { pkg-config --exists webkit2gtk-4.1 || pkg-config --exists webkit2gtk-4.0; }; } || { echo "setup: GTK 3 and WebKitGTK development files are missing: sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev" >&2; missing=1; }
        fi ;;
esac
[ "$missing" -eq 0 ] || exit 1

for pair in "CeeLing:$CEELING_URL" "cJSON-cling:$CJSON_URL"; do
    name="${pair%%:*}"; url="${pair#*:}"
    lower="$(echo "$name" | tr 'A-Z' 'a-z')"
    if [ -d "$PARENT/$name" ] || [ -d "$PARENT/$lower" ]; then
        echo "setup: $name found next to this repository"
    else
        echo "setup: cloning $name from $url"
        git clone --quiet --recurse-submodules "$url" "$PARENT/$name"
    fi
done

CEELING="$PARENT/CeeLing"; [ -d "$CEELING" ] || CEELING="$PARENT/ceeling"
if [ ! -f "$CEELING/build/cling.exe" ] && [ ! -f "$CEELING/build/cling" ]; then
    echo "setup: building the CeeLing compiler (a few minutes)"
    cmake -S "$CEELING" -B "$CEELING/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$CEELING/build" --parallel --target cling
else
    echo "setup: the CeeLing compiler is already built"
fi

bash "$HERE/scripts/fetch.sh"
. "$HERE/scripts/env.sh"
echo "setup: ready. CLING=$CLING CJSON=$CJSON"
echo "setup: next:  bash pronao doctor     then:  bash pronao create ../my-app && cd ../my-app && bash ../$(basename "$HERE")/pronao build"
