#!/usr/bin/env bash
# Window places for real on Linux (X11, or WSLg with GDK_BACKEND=x11): the window is moved and resized from outside (xdotool), the place must be saved; started again it must
# open where it was, every time (a place that is saved and applied must not creep: some window managers report a window at another place than they put it); a place on no
# monitor is brought back onto one, and one that is too big is made to fit. Then a CONTROL: a shim that never applies a remembered place.
# Maximize is tested when wmctrl is installed. What it cannot do: a physical mouse drag, Wayland (a program cannot know or set where its window is there).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory

story() { # dir shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S"; mkdir -p "$S/work" "$S/run" "$S/home"
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_placement" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/placement_window.clg" "$S/t" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export HOME="$S/home" XDG_CONFIG_HOME="$S/home/.config"
    local FILE="$S/home/.config/pronao-placement-test/windows.json"
    session() { # label command...: runs the program, waits for its window, runs the command with the window up, closes it
        local label="$1"; shift
        (cd "$S/run" && timeout 60 ../t > "$S/$label.out" 2>&1; echo $? > "$S/$label.rc") &
        local pid=$! n=0 id=""
        until grep -aq '^ready' "$S/$label.out" 2> /dev/null && id="$(xdotool search --onlyvisible --name '^placement test$' | head -1)" && [ -n "$id" ]; do
            sleep 0.2; n=$((n + 1)); [ "$n" -gt 150 ] && { echo "$label: never ready"; kill "$pid" 2> /dev/null; return 1; }
        done
        sleep 0.8
        local steps=0
        WIN="$id" "$@" || steps=1
        bash "$HERE/tests/close_dialogs.sh" "placement test" > /dev/null 2>&1
        wait "$pid"
        [ "$steps" = 0 ] && [ "$(cat "$S/$label.rc" 2> /dev/null)" = 0 ]
    }
    waitfile() { local n=0; until grep -Eq "$1" "$FILE" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt 25 ] && { echo "the file never had $1: $(tr -d '\n\t ' < "$FILE" 2> /dev/null)"; return 1; }; done; return 0; }
    size_of() { xdotool getwindowgeometry "$WIN" | sed -n 's/.*Geometry: //p'; }

    first_steps() {
        xdotool windowmove "$WIN" 120 90; sleep 0.4; xdotool windowsize "$WIN" 700 500; waitfile '"w":[[:space:]]*700' || return 1
        xdotool windowmove "$WIN" 200 150; xdotool windowsize "$WIN" 640 480; waitfile '"w":[[:space:]]*640' && waitfile '"h":[[:space:]]*480'
    }
    session first first_steps || return 1
    local saved; saved="$(tr -d '\n\t ' < "$FILE")"; echo "saved: $saved"
    # started again and again: the same place each time, and the same size
    local round
    for round in 1 2 3; do
        check_same() { local now; now="$(tr -d '\n\t ' < "$FILE")"; local g; g="$(size_of)"; echo "start $round: $g, the file says $now"; [ "$g" = "640x480" ] && xdotool windowsize "$WIN" 641 480 && sleep 1.2 && xdotool windowsize "$WIN" 640 480 && sleep 1.2; }
        session "again$round" check_same || return 1
        [ "$(tr -d '\n\t ' < "$FILE" | sed 's/"x":[0-9-]*,"y":[0-9-]*,//')" = "$(echo "$saved" | sed 's/"x":[0-9-]*,"y":[0-9-]*,//')" ] || { echo "the size changed between starts"; return 1; }
        [ "$(tr -d '\n\t ' < "$FILE" | grep -o '"x":[0-9-]*,"y":[0-9-]*')" = "$(echo "$saved" | grep -o '"x":[0-9-]*,"y":[0-9-]*')" ] || { echo "the place crept: $(tr -d '\n\t ' < "$FILE") (was $saved)"; return 1; }
    done
    echo "three starts in a row: the place did not creep"
    # maximize (needs wmctrl and a window manager that does it): saved as maximized with the restored size kept, opened maximized again, and restoring gives the old place
    if command -v wmctrl > /dev/null 2>&1; then
        do_max() { wmctrl -ir "$WIN" -b add,maximized_vert,maximized_horz; waitfile '"max":[[:space:]]*true' && grep -Eq '"w":[[:space:]]*640' "$FILE"; }
        session max1 do_max || { echo "maximizing was not saved with the restored size: $(tr -d '
	 ' < "$FILE")"; return 1; }
        back_restore() { local g; g="$(size_of)"; echo "reopened maximized: $g"; [ "${g%%x*}" -gt 700 ] && wmctrl -ir "$WIN" -b remove,maximized_vert,maximized_horz && waitfile '"max":[[:space:]]*false' && sleep 1 && [ "$(size_of)" = "640x480" ]; }
        session max2 back_restore || { echo "reopening maximized, or restoring, failed"; return 1; }
        echo "maximized, reopened maximized, restored to the old size"
    else
        echo "(wmctrl is not installed: maximize not tested)"
    fi
    # a place on no monitor is brought back onto one; one that is too big is made to fit
    printf '{"main": {"x": 30000, "y": 30000, "w": 640, "h": 480, "max": false}}' > "$FILE"
    check_inside() { local p; p="$(xdotool getwindowgeometry "$WIN" | sed -n 's/.*Position: \([0-9-]*\),\([0-9-]*\).*/\1 \2/p')"; echo "from nowhere to: $p"; set -- $p; [ "$1" -lt 5000 ] && [ "$2" -lt 5000 ] && [ "$1" -ge -100 ]; }
    session fifth check_inside || return 1
    return 0
}

story "${TMPDIR:-/tmp}/pronao-placement-linux" "" || { echo "placement test: FAILED"; exit 1; }
echo "placement test: ok"

C="${TMPDIR:-/tmp}/pronao-placement-linux-control"
rm -rf "$C"; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/posix/window.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "        gtk_window_move(w, x, y);" + chr(10)
assert s.count(key) == 1
open(p, "w", encoding="utf-8", newline="").write(s.replace(key, ""))
PYEOF
if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the place to be applied"; exit 1; fi
echo "control: without applying the remembered place the test fails, as it must"
