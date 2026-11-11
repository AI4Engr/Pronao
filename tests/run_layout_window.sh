#!/usr/bin/env bash
# Window layout for real (Windows): full screen, always on top, no title bar, fixed size, a place fitted to the monitors, a drag that does nothing without the mouse button,
# and a modal window that disables its owner until it closes (by the application, or by the user pressing its close button). After every instruction the test asks WINDOWS what the
# window looks like (window_poke.exe layout: the rectangle, WS_EX_TOPMOST, WS_CAPTION, WS_THICKFRAME, whether it is enabled, whether it covers its monitor).
# Then two CONTROLS: a shim in which full screen does not change the window, and one in which the owner is not disabled.
# Not tested: dragging and resizing with the mouse (a drag needs the button held down; the test checks only that it does nothing without it), transparency (not offered), several monitors.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/tests/lib_stop.sh"
TITLE="layout test"

story() { # dir  shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run" "$S/ins"
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; return 2; }
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_placement" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/layout_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")" LAYOUT_TEST_DIR="$(cygpath -w "$S/ins")"
    local P="$S/poke.exe" OUT="$S/out.txt" N=0
    (cd "$S/run" && timeout 120 "$S/t.exe" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { local n=0 limit=$(( ${2:-8} * 5 )); until grep -Eq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done; return 0; }
    fail() { echo "FAILED: $1"; stop_program "$S/t.exe"; wait "$pid" 2> /dev/null; return 1; }
    do_() { # operation args... -> the program does it, and says it is done
        N=$((N + 1)); printf '%s %s' "$N" "$*" > "$S/ins/instructions.txt"
        waitfor "^done $N " 10 || return 1
        sleep 0.4
    }
    look() { "$P" layout "$TITLE"; }                 # x y w h topmost caption thick enabled covers
    field() { echo "$1" | cut -d' ' -f"$2"; }

    waitfor '^ready' 30 && "$P" wait "$TITLE" || fail "the program never became ready" || return 1
    local before; before="$(look)"; echo "start: $before"
    [ "$(field "$before" 6)" = 1 ] && [ "$(field "$before" 7)" = 1 ] && [ "$(field "$before" 8)" = 1 ] && [ "$(field "$before" 9)" = 0 ] || fail "the window does not start as an ordinary window" || return 1

    # full screen, and back
    do_ fullscreen 1 || fail "full screen was not done" || return 1
    local fs; fs="$(look)"; echo "full screen: $fs"
    [ "$(field "$fs" 9)" = 1 ] && [ "$(field "$fs" 6)" = 0 ] || fail "the window does not cover its monitor without a frame" || return 1
    do_ fullscreen 0 || fail "leaving full screen was not done" || return 1
    local back; back="$(look)"; echo "back: $back"
    [ "$back" = "$before" ] || fail "the window did not come back as it was" || return 1

    # always on top
    do_ ontop 1 && [ "$(field "$(look)" 5)" = 1 ] || fail "the window is not on top" || return 1
    do_ ontop 0 && [ "$(field "$(look)" 5)" = 0 ] || fail "the window is still on top" || return 1

    # no title bar: the resize border stays
    do_ frameless 1; local fl; fl="$(look)"; echo "frameless: $fl"
    [ "$(field "$fl" 6)" = 0 ] && [ "$(field "$fl" 7)" = 1 ] || fail "frameless: expected no caption and a resize border" || return 1
    do_ frameless 0; [ "$(field "$(look)" 6)" = 1 ] || fail "the title bar did not come back" || return 1

    # a fixed size
    do_ resizable 0; [ "$(field "$(look)" 7)" = 0 ] || fail "the window can still be resized" || return 1
    do_ resizable 1; [ "$(field "$(look)" 7)" = 1 ] || fail "the window cannot be resized again" || return 1

    # a place: as asked, and fitted when it cannot be had
    do_ bounds 150 120 640 480; local bb; bb="$(look)"; echo "bounds: $bb"
    [ "$(field "$bb" 1) $(field "$bb" 2) $(field "$bb" 3) $(field "$bb" 4)" = "150 120 640 480" ] || fail "the window is not where it was put" || return 1
    do_ bounds 90000 90000 640 480; local fit; fit="$(look)"; echo "fitted: $fit"
    "$P" inside "$TITLE" || fail "a place far off the screen was not brought back onto it" || return 1
    do_ bounds 0 0 30000 30000
    "$P" inside "$TITLE" || fail "a window bigger than the screen was not made to fit" || return 1

    # a drag with no mouse button held does nothing
    do_ bounds 150 120 640 480; local pre; pre="$(look)"
    do_ drag; [ "$(look)" = "$pre" ] || fail "a drag started without the mouse button" || return 1

    # a modal window
    do_ modalopen || fail "the modal window was not opened" || return 1
    "$P" wait "layout child" || fail "the modal window never appeared" || return 1
    [ "$(field "$(look)" 8)" = 0 ] || fail "the owner can still be used while the modal window is open" || return 1
    [ "$(field "$("$P" layout "layout child")" 8)" = 1 ] || fail "the modal window itself is disabled" || return 1
    do_ modalclose
    [ "$(field "$(look)" 8)" = 1 ] || fail "the owner stayed disabled after the application closed the modal window" || return 1
    do_ modalopen && "$P" wait "layout child"
    [ "$(field "$(look)" 8)" = 0 ] || fail "the owner is not disabled the second time" || return 1
    "$P" close "layout child"; sleep 1
    [ "$(field "$(look)" 8)" = 1 ] || fail "the owner stayed disabled after the user closed the modal window" || return 1
    echo "the modal window disabled its owner and gave it back, both ways"

    do_ quit; waitfor '^closed$' 15 || fail "the program did not end" || return 1
    wait "$pid"
    [ "$(cat "$S/rc")" = 0 ] || { echo "the program ended with $(cat "$S/rc")"; return 1; }
    return 0
}

story "${TMPDIR:-/tmp}/pronao-layout" "" || { echo "layout test: FAILED"; stop_program "${TMPDIR:-/tmp}/pronao-layout/t.exe"; exit 1; }
echo "layout test: ok"

control() { # name  file  old  new
    local C="${TMPDIR:-/tmp}/pronao-layout-control-$1"
    rm -rf "$C" 2>/dev/null || true; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
    python3 - "$C/shim/$2" "$3" "$4" <<'PYEOF' || { echo "CONTROL SETUP FAILED: $1"; exit 1; }
import sys
p, old, new = sys.argv[1:4]
s = open(p, encoding='utf-8', newline='').read()
assert s.count(old) == 1, (old, s.count(old))
open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
PYEOF
    if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then stop_program "$C/case/t.exe"; echo "CONTROL DID NOT FAIL: $1"; exit 1; fi
    stop_program "$C/case/t.exe"
    echo "control: $1: the test fails, as it must"
}
control "full screen changes nothing" app_shim.cpp 'SetWindowPos(hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);' ''
control "the owner is not disabled" app_shim.cpp 'EnableWindow(ownerWindow, FALSE);' ''
