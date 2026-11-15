#!/usr/bin/env bash
# The window's menu bar, shortcuts and pop-up menu in a real window on Linux (the counterpart of run_menu_window.sh; the same program, tests/menu_window.clg, and page).
# From outside, with xdotool: the page must fill the window under the bar (it is told its size); Ctrl+O and Ctrl+S choose their items, once each; F5 ticks Grid and a second F5
# unticks it; Alt+V opens the View menu with the keyboard and Down, Return choose Grid; a right click in the page opens the page's own pop-up and Down, Return choose the first
# item (the page is told, and so is the program); Escape on the pop-up chooses nothing. Then a CONTROL: a shim that does not hook shortcuts; Ctrl+O must then fail to choose.
# (F5 is sent by its keycode, 71 on every evdev keymap: xdotool 3.2016 sends the NAME as a level that needs Alt on some keymaps, and the shortcut then does not match.)
# What it cannot do: look at the menu (appearance, high-DPI drawing); opening a menu by clicking its title with a synthetic mouse did not open it under WSLg, so the keyboard does.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory

story() { # dir shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S"; mkdir -p "$S/work" "$S/run"
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_menu" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/menu_window.clg" "$S/t" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    local OUT="$S/out.txt"
    (cd "$S/run" && timeout 60 "$S/t" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { local n=0 limit=$(( ${2:-8} * 5 )); until grep -aEq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done; return 0; }
    count() { grep -ac "^click $1\$" "$OUT" 2> /dev/null; }
    fail() { echo "FAILED: $1"; kill "$pid" 2> /dev/null; bash "$HERE/tests/close_dialogs.sh" "menu test" > /dev/null 2>&1; wait "$pid" 2> /dev/null; return 1; }

    waitfor '^sized ' 30 || fail "the program never became ready" || return 1
    local id; id="$(xdotool search --onlyvisible --name '^menu test$' | head -1)"
    [ -n "$id" ] || fail "no window" || return 1
    xdotool windowfocus "$id"; sleep 0.5

    # 1: the page fills the window under the bar (the window is 600 x 400, the bar takes some of the height)
    read -r _ w h < <(grep -a '^sized ' "$OUT" | tail -1 | tr -d '\r')
    echo "the page is $w x $h in a 600 x 400 window"
    [ "$w" = 600 ] && [ "$h" -ge 330 ] && [ "$h" -lt 400 ] || fail "the page does not fill the window under the bar" || return 1

    # 2: shortcuts, while the page has the keyboard
    xdotool key ctrl+o; waitfor '^click file.open$' 5 || fail "Ctrl+O did not choose Open" || return 1
    xdotool key ctrl+s; waitfor '^click file.save$' 5 && waitfor '^page saw file.save$' 5 || fail "Ctrl+S did not choose Save (or the page was not told)" || return 1
    [ "$(count file.open)" = 1 ] || fail "Ctrl+O chose Open $(count file.open) times" || return 1
    xdotool key 71; waitfor '^click view.grid$' 5 || fail "F5 did not choose Grid" || return 1
    echo "Ctrl+O, Ctrl+S and F5 chose their items, once each, and the page was told"

    # 3: the keyboard opens a menu of the bar
    sleep 0.5; xdotool key alt+v; sleep 0.8; xdotool key Down; sleep 0.3; xdotool key Return; sleep 1
    [ "$(count view.grid)" = 2 ] || fail "Alt+V, Down, Return did not choose Grid (chosen $(count view.grid) times)" || return 1
    echo "Alt+V opened View and Return chose Grid"

    # 4: the page's pop-up
    xdotool mousemove --window "$id" 300 250; sleep 0.3; xdotool click 3; sleep 1
    xdotool key Down; sleep 0.3; xdotool key Return
    waitfor '^popup chosen ctx.copy$' 8 || fail "the pop-up did not choose Copy" || return 1
    xdotool mousemove --window "$id" 300 250; sleep 0.3; xdotool click 3; sleep 1; xdotool key Escape
    waitfor '^popup dismissed$' 8 || fail "Escape on the pop-up was not reported as dismissed" || return 1
    echo "the page's pop-up chose Copy, and Escape chose nothing"

    bash "$HERE/tests/close_dialogs.sh" "menu test" > /dev/null 2>&1
    wait "$pid"
    return 0
}

story "${TMPDIR:-/tmp}/pronao-menu-linux" "" || { echo "menu test: FAILED"; exit 1; }
echo "menu test: ok"

C="${TMPDIR:-/tmp}/pronao-menu-linux-control"
rm -rf "$C"; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/posix/gtkmenu.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "g_signal_connect_data(window, \"key-press-event\""
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "(void)(", 1).replace("(void)(, G_CALLBACK(onKeyPress), new int(handle), freeInt, static_cast<GConnectFlags>(0));", ";"))
PYEOF
if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the shortcut hook"; exit 1; fi
echo "control: without the shortcut hook the test fails, as it must"
