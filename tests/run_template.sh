#!/usr/bin/env bash
# A new project made by scripts/new_project.sh must build (headless, window, release) and the window build must start and stay up.
#   tests/run_template.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/pronao-template"
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
    [ -f icon.ico ] || { echo "the new project has no icon.ico"; exit 1; }
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$W/poke.exe" -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 > /dev/null 2>&1 || { echo "POKE BUILD FAILED"; exit 1; }
    "$W/poke.exe" exeicon "$(cygpath -w "$PWD/out/my-app$EXE")" > /dev/null || { echo "the program has no icon of its own"; exit 1; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$W/profile-$$")"
    (cd out && timeout 6 "./window$EXE" > run.log 2>&1); RC=$?
    [ "$RC" -eq 124 ] || { cat out/run.log; echo "the window program did not stay up (exit $RC)"; exit 1; }
fi
if [ -z "$EXE" ]; then
    # Linux: the window build must build with the project's icon.png (it used to be given icon.ico, which the Linux build refuses), also as a release; and, with a display, stay up
    [ -f icon.png ] && [ "$(head -c8 icon.png | od -An -tx1 | tr -d ' 
')" = "89504e470d0a1a0a" ] || { echo "the new project has no icon.png"; exit 1; }
    bash build.sh > build-window.log 2>&1 || { cat build-window.log; echo "WINDOW BUILD FAILED"; exit 1; }
    bash build.sh --release > build-release.log 2>&1 || { cat build-release.log; echo "RELEASE BUILD FAILED"; exit 1; }
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        export GDK_BACKEND="${GDK_BACKEND:-x11}" GSETTINGS_BACKEND=memory
        (cd out && timeout 6 ./my-app > run.log 2>&1); RC=$?
        [ "$RC" -eq 124 ] || { grep -av 'dconf' out/run.log | tail -5; echo "the window program did not stay up (exit $RC)"; exit 1; }
    else
        echo "(no display: the window program was built, not started)"
    fi
fi
bash build.sh --nonsense > /dev/null 2>&1 && { echo "build.sh accepted an unknown option"; exit 1; }
echo "template: ok"
