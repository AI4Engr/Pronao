#!/usr/bin/env bash
# The window's menu bar for real (Windows): the bar must be there with the right items, choosing an item must reach the program (and the page, when asked), a
# shortcut must work while the page holds the keyboard and be swallowed so the page does not get it, a greyed item must do nothing, and a tick must show.
# The test chooses items and presses keys from outside (tests/window_poke.cpp: WM_COMMAND for an item, SendInput for keys). Then two CONTROLS: against a shim that
# does not route menu commands, and against one that does not hook shortcuts; each must make the test fail.
# What it does not do: open the menu with the mouse and look at it (appearance, keyboard navigation inside the menu, high-DPI drawing) -- see README.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TITLE="menu test"

story() { # dir  shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run"
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; return 2; }
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_menu" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/menu_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
    local P="$S/poke.exe" OUT="$S/out.txt"
    (cd "$S/run" && timeout 90 "$S/t.exe" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { # pattern [seconds] -> waits for the line in the program's output
        local n=0 limit=$(( ${2:-8} * 5 ))
        until grep -Eq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done
        return 0                       # (without this the status would be that of the last test in the loop: false)
    }
    count() { grep -c "^click $1\$" "$OUT" 2> /dev/null; }
    fail() { echo "FAILED: $1"; "$P" close "$TITLE" 2> /dev/null; kill "$pid" 2> /dev/null; wait "$pid" 2> /dev/null; return 1; }

    waitfor '^ready' 30 && "$P" wait "$TITLE" || fail "the program never became ready" || return 1
    echo "page: $(grep '^ready' "$OUT" | head -1)"

    # 1: the bar
    local bar; bar="$("$P" menu "$TITLE")"; echo "bar: $bar"
    [ "$bar" = "File: Open|Save|-|Disable save|Quit ; View: Grid" ] || fail "the menu bar is not what was declared" || return 1

    # 1b: the menu bar takes room from the window, and the page is made to fit what is left (it is neither cut off nor left with an empty strip)
    waitfor '^sized ' 10 || fail "the page never said how big it is" || return 1
    local client sized_w sized_h cw ch
    client="$("$P" client "$TITLE")"; cw="${client% *}"; ch="${client#* }"
    read -r _ sized_w sized_h < <(grep '^sized ' "$OUT" | tail -1 | tr -d '\r')
    echo "client area: $cw x $ch; the page: $sized_w x $sized_h"
    [ "$(( cw - sized_w ))" -le 2 ] && [ "$(( sized_w - cw ))" -le 2 ] && [ "$(( ch - sized_h ))" -le 2 ] && [ "$(( sized_h - ch ))" -le 2 ] || fail "the page does not fill the client area under the menu bar" || return 1

    # 2: choosing an item reaches the program, and the page
    "$P" click "$TITLE" "Save" && waitfor '^click file.save$' && waitfor '^page saw file.save$' || fail "choosing Save did not reach the program and the page" || return 1

    # 3: a shortcut while the page holds the keyboard
    "$P" key "$TITLE" ctrl+o && waitfor '^click file.open$' || fail "Ctrl+O did not choose Open" || return 1
    [ "$(count file.open)" = 1 ] || fail "Ctrl+O chose Open $(count file.open) times" || return 1

    # 4: a function key, and a tick
    "$P" key "$TITLE" f5 && waitfor '^click view.grid$' || fail "F5 did not choose Grid" || return 1
    sleep 0.5
    bar="$("$P" menu "$TITLE")"; echo "bar after F5: $bar"
    case "$bar" in *"Grid*") ;; *) fail "Grid is not ticked after F5" || return 1 ;; esac
    "$P" key "$TITLE" f5 && sleep 0.8
    bar="$("$P" menu "$TITLE")"
    case "$bar" in *"Grid*") fail "Grid is still ticked after the second F5" || return 1 ;; esac

    # 5: a greyed item does nothing, with the mouse or the keyboard
    "$P" click "$TITLE" "Disable save" && waitfor '^click file.nosave$' || fail "Disable save was not chosen" || return 1
    sleep 0.5
    bar="$("$P" menu "$TITLE")"; echo "bar after disabling Save: $bar"
    case "$bar" in *"Save~"*) ;; *) fail "Save is not greyed" || return 1 ;; esac
    local before; before="$(count file.save)"
    "$P" key "$TITLE" ctrl+s; sleep 1
    [ "$(count file.save)" = "$before" ] || fail "Ctrl+S chose Save although it is greyed" || return 1

    # 5b: a right click in the page asks for the menu the application registered; the first item is chosen with the keyboard
    "$P" rightclick "$TITLE"; sleep 1.2
    "$P" popupexists || fail "a right click in the page did not open the registered menu" || return 1
    "$P" menukeys; waitfor '^popup chosen ctx.copy$' && waitfor '^click ctx.copy$' || fail "the item chosen in the page's menu did not reach the page and the program" || return 1

    # 6: a key that is not a shortcut is left alone, and Quit ends the program
    "$P" key "$TITLE" a; sleep 0.5
    "$P" click "$TITLE" "Quit" && waitfor '^closed$' 15 || fail "Quit did not end the program" || return 1
    wait "$pid"
    [ "$(cat "$S/rc")" = 0 ] || { echo "the program ended with $(cat "$S/rc")"; return 1; }
    local clicks; clicks="$(grep '^click ' "$OUT" | tr '\n' ' ')"; echo "clicks: $clicks"
    [ "$clicks" = "click file.save click file.open click view.grid click view.grid click file.nosave click ctx.copy click file.quit " ] || { echo "the clicks are not the ones expected"; return 1; }
    return 0
}

story "${TMPDIR:-/tmp}/app-cling-menu" "" || { echo "menu test: FAILED"; exit 1; }
echo "menu test: ok"

control() { # name  file  old  new
    local C="${TMPDIR:-/tmp}/app-cling-menu-control-$1"
    rm -rf "$C" 2>/dev/null || true; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
    python3 - "$C/shim/$2" "$3" "$4" <<'PYEOF' || { echo "CONTROL SETUP FAILED: $1"; exit 1; }
import sys
p, old, new = sys.argv[1:4]
s = open(p, encoding='utf-8', newline='').read()
assert s.count(old) == 1, (old, s.count(old))
open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
PYEOF
    if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: $1"; exit 1; fi
    echo "control: $1: the test fails, as it must"
}
control "menu commands are not routed" app_shim.cpp 'app_menu_command(a->nav->handle, LOWORD(wp))' 'false'
control "shortcuts are not hooked" win/nav.cpp 'return app_menu_shortcut(handle, vk, mods, repeat);' 'return false;'
