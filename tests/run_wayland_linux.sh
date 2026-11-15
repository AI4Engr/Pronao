#!/usr/bin/env bash
# The window layer under WAYLAND, not X11: weston is started headless on a private socket, GDK_BACKEND=wayland, no X display at all. Checked, without anyone clicking
# (xdotool does not exist under Wayland, so no input is sent): the site self-test of tests/site_window.clg (window, pronao:// pages served from memory, navigation guard, commands)
# prints "selftest ok"; the menu bar is built and the page is told its size under it (600 wide, shorter than the 400 high window). Then a CONTROL: the same program pointed at a
# compositor that is not there must not open a window.
# What it cannot do: type, click, drag, and read a window's position (a Wayland program cannot), so the dialogs, shortcuts, drops and placement are not tested here.
# Needs weston (apt install weston). It does not touch the display you are using.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v weston > /dev/null || { echo "wayland test: weston is not installed (apt install weston)"; exit 1; }
W="${TMPDIR:-/tmp}/pronao-wayland-linux"
rm -rf "$W"; mkdir -p "$W/work1" "$W/work2" "$W/run" "$W/xdg"; chmod 700 "$W/xdg"
ASSETS="$HERE/tests/site_window" WORK="$W/work1" bash "$HERE/scripts/build.sh" "$HERE/tests/site_window.clg" "$W/site" > "$W/b1.log" 2>&1 || { cat "$W/b1.log"; echo "BUILD FAILED"; exit 1; }
ASSETS="$HERE/tests/site_menu" WORK="$W/work2" bash "$HERE/scripts/build.sh" "$HERE/tests/menu_window.clg" "$W/menu" > "$W/b2.log" 2>&1 || { cat "$W/b2.log"; echo "BUILD FAILED"; exit 1; }
export XDG_RUNTIME_DIR="$W/xdg" GSETTINGS_BACKEND=memory GDK_BACKEND=wayland WEBKIT_DISABLE_COMPOSITING_MODE=1 WEBKIT_DISABLE_DMABUF_RENDERER=1
unset DISPLAY
weston --backend=headless --socket=pronao-wl --width=1280 --height=800 > "$W/weston.log" 2>&1 &
WPID=$!
trap 'kill "$WPID" 2> /dev/null' EXIT
n=0; until [ -S "$XDG_RUNTIME_DIR/pronao-wl" ]; do sleep 0.2; n=$((n + 1)); [ "$n" -gt 50 ] && { echo "weston did not start: $(tail -3 "$W/weston.log")"; exit 1; }; done
export WAYLAND_DISPLAY=pronao-wl
fail() { echo "FAILED: $1"; exit 1; }

OUT="$(cd "$W/run" && timeout 60 "$W/site" 2>&1)"; RC=$?
printf '%s\n' "$OUT" | grep -aq '^selftest ok$' && [ "$RC" -eq 0 ] || fail "the site self-test under Wayland: $(printf '%s\n' "$OUT" | grep -av 'libEGL\|MESA\|dconf\|^$' | tail -3)"
echo "the site self-test passes under Wayland (window, pages from memory, navigation guard, commands)"

(cd "$W/run" && timeout 30 "$W/menu" > "$W/menu.out" 2>&1 &)
n=0; until grep -aq '^sized ' "$W/menu.out" 2> /dev/null; do sleep 0.3; n=$((n + 1)); [ "$n" -gt 60 ] && fail "the menu program never reported its size under Wayland"; done
read -r _ w h < <(grep -a '^sized ' "$W/menu.out" | tail -1 | tr -d '\r')
[ "$w" = 600 ] && [ "$h" -ge 330 ] && [ "$h" -lt 400 ] || fail "the page is $w x $h under the menu bar in a 600 x 400 window"
echo "the menu bar is built and the page is $w x $h under it"

# CONTROL: no compositor at that name
if (cd "$W/run" && WAYLAND_DISPLAY=nonexistent-socket timeout 20 "$W/site" 2>&1 | grep -aq '^selftest ok$'); then echo "CONTROL DID NOT FAIL: the program ran with no compositor"; exit 1; fi
echo "control: with no compositor the program does not run, as it must"
echo "wayland test: ok"
