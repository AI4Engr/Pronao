#!/usr/bin/env bash
# Builds __NAME__:   bash build.sh [--release] [--headless]      -> out/__NAME__(.exe)
#   --release   no developer tools, no development server (the choice is compiled in)
#   --headless  no window (test doubles instead): for checking that the program transpiles and compiles
# Where pronao lives: PRONAO in the environment, else the folder this project was created from.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRONAO="${PRONAO:-__PRONAO__}"
[ -f "$PRONAO/scripts/build.sh" ] || { echo "pronao was not found at $PRONAO (set PRONAO=<its folder>)" >&2; exit 1; }
MODE=window
for a in "$@"; do
    case "$a" in
        --release) export RELEASE=1 ;;          # also: no console window when the program is started by double-click (GUI=0 keeps it)
        --headless) MODE=--headless ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac
mkdir -p "$HERE/out"
# what Explorer shows under Properties > Details of the exe, and what a virus scanner or SmartScreen reads about a program with no signature: fill these in
export APP_NAME="__NAME__" APP_VERSION="${APP_VERSION:-0.1.0}" APP_COMPANY="${APP_COMPANY:-}" APP_DESCRIPTION="${APP_DESCRIPTION:-__NAME__}"
# the program's icon: replace icon.ico (Windows, several sizes) and icon.png (Linux, 48 x 48 or more) with your own
if [ -n "$EXE" ]; then [ -f "$HERE/icon.ico" ] && export ICON="$HERE/icon.ico"; else [ -f "$HERE/icon.png" ] && export ICON="$HERE/icon.png"; fi
PAGE="$HERE/index.html" WORK="${WORK:-$HERE/out/work}" bash "$PRONAO/scripts/build.sh" "$HERE/main.clg" "$HERE/out/__NAME__$EXE" $MODE
echo "built out/__NAME__$EXE"
