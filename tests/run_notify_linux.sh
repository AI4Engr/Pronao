#!/usr/bin/env bash
# System notifications on Linux, for real up to the desktop's service: the program shows one from the application and one the page asked for; what reaches
# org.freedesktop.Notifications is checked (tests/notify_mock.py stands in for the desktop's service on a private session bus: application name, title, text, a default action),
# a click on the last notification (the mock sends ActionInvoked) must reach the program and the page that asked, and with no notification service the program must be told
# that notifications cannot be shown. Then a CONTROL: a shim that does not listen for clicks. Needs dbus-run-session, python3 with PyGObject, and a display.
# What it cannot see: the notification on the screen, which depends on the desktop and its do-not-disturb setting.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory

story() { # dir shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S"; mkdir -p "$S/work" "$S/run"
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_notify" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/notify_window.clg" "$S/t" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export S HERE
    dbus-run-session -- bash -c '
        python3 "$HERE/tests/notify_mock.py" "$S/log" "$S/click" "$S/stop" > "$S/mock.out" 2>&1 &
        sleep 2
        cd "$S/run"; timeout 40 ../t > "$S/out.txt" 2>&1 &
        n=0; until grep -aq "^page notify ok" "$S/out.txt" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt 100 ] && break; done
        sleep 0.5; touch "$S/click"; sleep 3
        id=$(xdotool search --onlyvisible --name "^notify test$" | head -1); bash "$HERE/tests/close_dialogs.sh" "notify test" > /dev/null 2>&1
        wait %2 2> /dev/null; touch "$S/stop"; sleep 0.5' > /dev/null 2>&1
    local OUT="$S/out.txt"
    grep -aq '^notified ok$' "$OUT" || { echo "FAILED: the application's notification was not taken"; return 1; }
    grep -aq '^page notify ok$' "$OUT" || { echo "FAILED: the page's notification was not taken"; return 1; }
    grep -aq '^notify notify test 1 notify test | something happened | actions=1$' "$S/log" || { echo "FAILED: the service did not get the application's notification: $(cat "$S/log")"; return 1; }
    grep -aq '^notify notify test 1 from the page | hello | actions=1$' "$S/log" || { echo "FAILED: the service did not get the page's notification: $(cat "$S/log")"; return 1; }
    echo "the service received both notifications with their text and a default action"
    grep -aq '^notification click from-page$' "$OUT" && grep -aq '^page heard click from-page$' "$OUT" || { echo "FAILED: the click did not reach the program and the page"; return 1; }
    echo "a click reached the program and the page that asked"
    return 0
}

story "${TMPDIR:-/tmp}/pronao-notify-linux" "" || { echo "notify test: FAILED"; exit 1; }

# no notification service at all: the application is told, plainly
N="${TMPDIR:-/tmp}/pronao-notify-linux-none"
rm -rf "$N"; mkdir -p "$N/run"
( cd "$N/run" && DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent timeout 25 "${TMPDIR:-/tmp}/pronao-notify-linux/t" > "$N/out.txt" 2>&1 & )
sleep 12
bash "$HERE/tests/close_dialogs.sh" "notify test" > /dev/null 2>&1
grep -aq 'selftest FAILED: no notifier' "$N/out.txt" || { echo "notify test: with no bus the program did not say it has no notifier: $(grep -av dconf "$N/out.txt" | head -3)"; exit 1; }
echo "with no session bus the program is told there is no notifier"
echo "notify test: ok"

C="${TMPDIR:-/tmp}/pronao-notify-linux-control"
rm -rf "$C"; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/posix/notify.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = 'if (std::string(signal) != "ActionInvoked") return;'
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "return;"))
PYEOF
if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the click to be heard"; exit 1; fi
echo "control: without listening for clicks the test fails, as it must"
