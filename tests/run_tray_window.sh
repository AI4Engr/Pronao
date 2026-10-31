#!/usr/bin/env bash
# The tray icon and the window that hides instead of closing, for real (Windows): the shell must hold the icon; the close button must hide the window and leave the
# program running; clicking the icon must bring the window back; the right click must open the menu; choosing "Quit" must end the program.
# The test acts from outside (tests/window_poke.cpp): it posts the message the shell sends the icon's window for a click, and the command a menu item sends -- it does not
# move the mouse onto the real icon (Windows 11 keeps those in a shell-owned flyout). Then two CONTROLS: against a shim in which hiding does nothing, and one in which
# the tray menu's items reach nobody; each must make the test fail.
# What it does not do: look at the icon (its picture, its tooltip, where it sits), a click with a real mouse, the shell being restarted.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/tests/lib_stop.sh"
TITLE="tray test"

story() { # dir  shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run"
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; return 2; }
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_placement" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/tray_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
    local P="$S/poke.exe" OUT="$S/out.txt"
    (cd "$S/run" && timeout 90 "$S/t.exe" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { # pattern [seconds]
        local n=0 limit=$(( ${2:-8} * 5 ))
        until grep -Eq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done
        return 0
    }
    waitstate() { # state [seconds] -> the window of the test is visible / hidden
        local n=0 limit=$(( ${2:-5} * 5 ))
        until [ "$("$P" state "$TITLE" 2> /dev/null)" = "$1" ]; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "the window is $("$P" state "$TITLE" 2> /dev/null), not $1"; return 1; }; done
        return 0
    }
    fail() { echo "FAILED: $1"; "$P" traycommand 3 2> /dev/null; sleep 1; stop_program "$S/t.exe"; wait "$pid" 2> /dev/null; return 1; }

    waitfor '^ready' 30 && waitstate visible 20 || fail "the program never became ready" || return 1

    # 1: the shell holds the icon
    "$P" trayexists || fail "the shell does not hold the tray icon" || return 1
    echo "the shell holds the tray icon"

    # 2: the close button hides the window; the program goes on
    "$P" close "$TITLE"; waitstate hidden || fail "the close button did not hide the window" || return 1
    sleep 1; kill -0 "$pid" 2> /dev/null || fail "the program ended when its window was hidden" || return 1
    "$P" trayexists || fail "the tray icon went with the window" || return 1
    echo "closing hid the window and the program went on"

    # 3: a left click on the icon brings it back
    "$P" traycall left && waitfor '^tray click$' && waitstate visible || fail "a click on the icon did not bring the window back" || return 1
    echo "a click on the icon brought the window back"

    # 4: the right click opens the tray menu
    "$P" traycall right; sleep 1
    "$P" popupexists || fail "the right click did not open a menu" || return 1
    "$P" dismiss; sleep 1
    if "$P" popupexists; then fail "the menu stayed open after it was dismissed"; return 1; fi
    echo "the right click opened the menu"

    # 5: items of the tray menu
    "$P" close "$TITLE"; waitstate hidden || fail "hide a second time" || return 1
    "$P" traycommand 0 && waitfor '^menu tray.show$' && waitstate visible || fail "Show window did not bring the window back" || return 1
    "$P" traycommand 1 && waitfor '^menu tray.mute$' || fail "Mute was not chosen" || return 1

    # 5b: a notification from the tray icon, and a click on it
    "$P" traycommand 2 && waitfor '^notified ok$' || fail "the system did not take the notification" || return 1
    "$P" traycall balloon && waitfor '^notification click note-1$' || fail "a click on the notification did not reach the program" || return 1
    "$P" trayexists || fail "the tray icon went away with its notification" || return 1
    echo "a notification was taken and its click reached the program"

    # 6: Quit ends the program, whatever the window is doing
    "$P" close "$TITLE"; waitstate hidden || fail "hide a third time" || return 1
    "$P" traycommand 3 && waitfor '^closed$' 15 || fail "Quit did not end the program" || return 1
    wait "$pid"
    [ "$(cat "$S/rc")" = 0 ] || { echo "the program ended with $(cat "$S/rc")"; return 1; }
    [ "$("$P" state "$TITLE" 2> /dev/null)" = "none" ] || { echo "a window is left"; return 1; }
    "$P" trayexists && { echo "a tray window is left"; return 1; }
    echo "events: $(grep -E '^(tray|menu|notif) ' "$OUT" | tr '\n' ' ')"
    return 0
}

story "${TMPDIR:-/tmp}/app-cling-tray" "" || { echo "tray test: FAILED"; exit 1; }
echo "tray test: ok"

control() { # name  file  old  new
    local C="${TMPDIR:-/tmp}/app-cling-tray-control-$1"
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
control "hiding does nothing" app_shim.cpp 'else if (state == 5) ShowWindow(hwnd, SW_HIDE);' 'else if (state == 5) {}'
control "tray menu items reach nobody" win/tray.cpp 'if (id) tell(t->handle, "menu", *id);' ''
