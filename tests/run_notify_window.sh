#!/usr/bin/env bash
# System notifications for real (Windows): a program with no tray menu shows a notification from the application and one the page asked for. The notification icon must be in the
# notification area while a notification is up, a click on the notification must reach the program (and the page that asked), the icon must be gone after a click, and a notification
# nobody clicks must take its icon away by itself.
# The test posts the message the shell sends for a click on a balloon, and asks the shell whether it holds the icon. It cannot see the toast: whether it is SHOWN depends on Focus
# Assist, the user's settings and what is on the screen. Then two CONTROLS: clicks that reach nobody, and an icon that is never taken away.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/tests/lib_stop.sh"
TITLE="notify test"

story() { # dir  shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run"
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; return 2; }
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_notify" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/notify_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
    local P="$S/poke.exe" OUT="$S/out.txt"
    (cd "$S/run" && timeout 90 "$S/t.exe" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { # pattern [seconds]
        local n=0 limit=$(( ${2:-8} * 5 ))
        until grep -Eq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done
        return 0
    }
    gone() { "$P" trayabsent "$1"; }                 # seconds -> the shell no longer holds the icon
    fail() { echo "FAILED: $1"; "$P" close "$TITLE" 2> /dev/null; stop_program "$S/t.exe"; wait "$pid" 2> /dev/null; return 1; }

    # a watcher is started BEFORE each notification and polls every 15 ms: the shell may dismiss a balloon at once (Focus Assist, the user's settings), so the icon can be up for a
    # tenth of a second only; checking now and then from here would miss it
    "$P" traywait 40 & local w1=$!
    waitfor '^notified ok$' 30 || fail "the system did not take the application's notification" || return 1
    wait "$w1" || fail "the icon was never in the notification area while the application's notification was up" || return 1
    echo "the application's notification was taken and its icon was up"

    "$P" traycall balloon && waitfor '^notification click from-app$' || fail "a click on the notification did not reach the program" || return 1
    gone 3 || fail "the icon stayed after the notification was clicked" || return 1
    echo "a click reached the program and the icon went"

    "$P" traywait 20 & local w2=$!
    waitfor '^page notify ok$' 10 || fail "the page's notification was not taken" || return 1
    wait "$w2" || fail "the icon was never up for the page's notification" || return 1
    "$P" traycall balloon && waitfor '^notification click from-page$' && waitfor '^page heard click from-page$' || fail "a click on the page's notification did not reach the program and the page" || return 1
    echo "a click on the page's notification reached the program and the page"

    "$P" traywait 25 & local w3=$!
    local waited=0
    until [ "$(grep -c '^notified ok$' "$OUT")" -ge 2 ]; do sleep 0.2; waited=$((waited + 1)); [ "$waited" -gt 75 ] && break; done
    [ "$(grep -c '^notified ok$' "$OUT")" -ge 2 ] || fail "the third notification was not taken" || return 1
    wait "$w3" || fail "the icon was never up for the third notification" || return 1
    gone 25 || fail "an icon nobody clicked stayed in the notification area" || return 1
    echo "an unclicked notification took its icon away by itself"

    "$P" close "$TITLE"
    wait "$pid"
    [ "$(cat "$S/rc")" = 0 ] || { echo "the program ended with $(cat "$S/rc")"; return 1; }
    "$P" trayexists && { echo "an icon is left"; return 1; }
    return 0
}

story "${TMPDIR:-/tmp}/app-cling-notify" "" || { echo "notify test: FAILED"; exit 1; }
echo "notify test: ok"

control() { # name  file  old  new
    local C="${TMPDIR:-/tmp}/app-cling-notify-control-$1"
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
control "clicks reach nobody" win/tray.cpp 'if (!id.empty()) tell(t->handle, "notify", id);' ''
control "the icon is never taken away" win/tray.cpp 'void removeIcon(Tray* t) {
    if (!t->shown) return;' 'void removeIcon(Tray* t) {
    return;
    if (!t->shown) return;'
