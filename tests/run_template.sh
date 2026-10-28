#!/usr/bin/env bash
# A new project made by scripts/new_project.sh must build (headless, window, release) and the window build must start and stay up.
#   tests/run_template.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/app-cling-template"
rm -rf "$W" 2>/dev/null || true; mkdir -p "$W"
bash "$HERE/scripts/new_project.sh" "$W/my-app" > /dev/null || { echo "new_project FAILED"; exit 1; }
bash "$HERE/scripts/new_project.sh" "$W/my-app" > /dev/null 2>&1 && { echo "new_project overwrote a project that exists"; exit 1; }
bash "$HERE/scripts/new_project.sh" "$W/9bad" > /dev/null 2>&1 && { echo "new_project accepted a name that starts with a digit"; exit 1; }
cd "$W/my-app"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac
bash build.sh --headless > build-headless.log 2>&1 || { cat build-headless.log; echo "HEADLESS BUILD FAILED"; exit 1; }
if [ -n "$EXE" ]; then
    bash build.sh > build-window.log 2>&1 || { cat build-window.log; echo "WINDOW BUILD FAILED"; exit 1; }
    cp "out/my-app$EXE" "out/window$EXE"
    bash build.sh --release > build-release.log 2>&1 || { cat build-release.log; echo "RELEASE BUILD FAILED"; exit 1; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$W/profile-$$")"
    (cd out && timeout 6 "./window$EXE" > run.log 2>&1); RC=$?
    [ "$RC" -eq 124 ] || { cat out/run.log; echo "the window program did not stay up (exit $RC)"; exit 1; }
fi
bash build.sh --nonsense > /dev/null 2>&1 && { echo "build.sh accepted an unknown option"; exit 1; }
echo "template: ok"
