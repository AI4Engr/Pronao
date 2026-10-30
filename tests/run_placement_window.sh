#!/usr/bin/env bash
# Window places for real: a window is moved, resized, maximized and restored from outside (tests/window_poke.cpp), the program is started again and the
# window must come back where it was; a place that no monitor has any more must be brought back onto one. Then a CONTROL: the same story against a shim that
# never applies a remembered place; the window opens at its default and the test must fail.
# What it does not do: a physical mouse drag (the end of a drag is sent as the message it produces), more than the monitors this machine has.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TITLE="placement test"

story() { # dir  shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run" "$S/roaming" "$S/home"
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; return 2; }
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_placement" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/placement_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export APPDATA="$(cygpath -w "$S/roaming")" HOME="$S/home" XDG_CONFIG_HOME="$S/home/.config"
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
    local FILE="$S/roaming/app-cling-placement-test/windows.json"
    local P="$S/poke.exe"

    session() { # label  command...   runs the program, waits for its page, runs the commands with the window up, closes it, waits for the end
        local label="$1"; shift
        (cd "$S/run" && timeout 60 "$S/t.exe" > "$S/$label.out" 2>&1; echo $? > "$S/$label.rc") &
        local pid=$!
        local waited=0
        until grep -q '^ready' "$S/$label.out" 2> /dev/null && "$P" wait "$TITLE"; do
            sleep 0.2; waited=$((waited + 1))
            [ "$waited" -gt 150 ] && { echo "$label: the program never became ready"; cat "$S/$label.out"; kill "$pid" 2> /dev/null; return 1; }
        done
        local steps=0
        "$@" || steps=1                                   # (the window is closed and waited for even when a step failed, then the failure is reported)
        "$P" close "$TITLE"
        wait "$pid"
        [ "$steps" = 0 ] && [ "$(cat "$S/$label.rc" 2> /dev/null)" = 0 ]
    }
    waitfile() { # pattern -> waits up to 5 s for it to appear in the file
        local n=0
        until grep -Eq "$1" "$FILE" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt 25 ] && { echo "the file never had $1: $(cat "$FILE" 2> /dev/null | tr -d '\n ')"; return 1; }; done
    }

    # 1: the user moves and resizes the window, maximizes it, restores it and moves it again
    first_steps() {
        "$P" move "$TITLE" 120 90 700 500 && waitfile '"x":[[:space:]]*120' || return 1
        "$P" maximize "$TITLE" && waitfile '"max":[[:space:]]*true' || return 1
        grep -Eq '"w":[[:space:]]*700' "$FILE" || { echo "maximizing lost the restored size: $(tr -d '\n ' < "$FILE")"; return 1; }
        "$P" restore "$TITLE" && waitfile '"max":[[:space:]]*false' || return 1
        "$P" move "$TITLE" 200 150 640 480 && waitfile '"x":[[:space:]]*200' && waitfile '"h":[[:space:]]*480'
    }
    session first first_steps || return 1
    echo "saved: $(tr -d '\n ' < "$FILE")"

    # 2: started again, it opens where it was
    check_there() { local got; got="$("$P" find "$TITLE")"; echo "reopened at: $got"; [ "$got" = "200 150 640 480 0" ]; }
    session second check_there || return 1

    # 3: it was maximized when closed; it comes back maximized, and restoring gives the size it had
    maximize_then_leave() { "$P" maximize "$TITLE" && waitfile '"max":[[:space:]]*true'; }
    session third maximize_then_leave || return 1
    check_maximized() { local got; got="$("$P" find "$TITLE")"; echo "reopened maximized: $got"; [ "${got##* }" = 1 ] && [ "${got% *}" = "200 150 640 480" ]; }
    session fourth check_maximized || return 1

    # 4: a place on no monitor at all is brought back onto one
    printf '{"main": {"x": 30000, "y": 30000, "w": 640, "h": 480, "max": false}}' > "$FILE"
    check_inside() { local got; got="$("$P" find "$TITLE")"; echo "from nowhere to: $got"; "$P" inside "$TITLE"; }
    session fifth check_inside || return 1
    # ... and one that is too big is made to fit
    printf '{"main": {"x": 0, "y": 0, "w": 30000, "h": 30000, "max": false}}' > "$FILE"
    session sixth check_inside || return 1
    return 0
}

story "${TMPDIR:-/tmp}/app-cling-placement" "" || { echo "placement test: FAILED"; exit 1; }
echo "placement test: ok"

C="${TMPDIR:-/tmp}/app-cling-placement-control"
rm -rf "$C" 2>/dev/null || true; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/app_shim.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);"
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "/* the remembered place is not applied */"))
PYEOF
if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the place to be applied"; exit 1; fi
echo "control: without applying the remembered place the test fails, as it must"
