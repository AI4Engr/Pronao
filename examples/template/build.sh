#!/usr/bin/env bash
# Builds __NAME__:   bash build.sh [--release] [--headless]      -> out/__NAME__(.exe)
#   --release   no developer tools, no development server (the choice is compiled in)
#   --headless  no window (test doubles instead): for checking that the program transpiles and compiles
# Where app-cling lives: APP_CLING in the environment, else the folder this project was created from.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_CLING="${APP_CLING:-__APP_CLING__}"
[ -f "$APP_CLING/scripts/build.sh" ] || { echo "app-cling was not found at $APP_CLING (set APP_CLING=<its folder>)" >&2; exit 1; }
MODE=window
for a in "$@"; do
    case "$a" in
        --release) export RELEASE=1 ;;
        --headless) MODE=--headless ;;
        *) echo "unknown option $a" >&2; exit 2 ;;
    esac
done
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac
mkdir -p "$HERE/out"
PAGE="$HERE/index.html" WORK="${WORK:-$HERE/out/work}" bash "$APP_CLING/scripts/build.sh" "$HERE/main.clg" "$HERE/out/__NAME__$EXE" $MODE
echo "built out/__NAME__$EXE"
