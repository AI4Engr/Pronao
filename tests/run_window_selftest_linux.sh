#!/usr/bin/env bash
# Linux: builds tests/window_selftest.clg and tests/site_window.clg against GTK3 + WebKitGTK and runs them in a real window (a real X11 display, or WSLg with
# GDK_BACKEND=x11). Needs the packages scripts/build.sh names, xdotool, and a window manager. Passes only on exit code 0 AND "selftest ok" for both.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory      # (memory: no dconf warnings on a machine without a session bus)
W="${TMPDIR:-/tmp}/pronao-window-linux"
rm -rf "$W"; mkdir -p "$W/work" "$W/run/files"
PAGE="$HERE/tests/selftest.html" WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/window_selftest.clg" "$W/t" || { echo "BUILD FAILED"; exit 1; }
bash "$HERE/tests/close_dialogs.sh" "pronao selftest ready" "pronao selftest close" "pronao selftest message" "pronao selftest open" > "$W/dialogs.log" 2>&1 &
CLOSER=$!
OUT="$(cd "$W/run" && timeout 60 "$W/t" 2>&1)"; RC=$?
wait $CLOSER; CLOSERC=$?
echo "$OUT"
[ "$CLOSERC" -eq 0 ] || { echo "dialog helper: $(cat "$W/dialogs.log")"; exit 1; }
[ "$RC" -eq 124 ] && { echo "window selftest: TIMED OUT"; exit 1; }
{ [ "$RC" -eq 0 ] && printf '%s\n' "$OUT" | grep -q '^selftest ok$'; } || exit 1

S="${TMPDIR:-/tmp}/pronao-site-linux"
rm -rf "$S"; mkdir -p "$S/work" "$S/run"
ASSETS="$HERE/tests/site_window" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/site_window.clg" "$S/t" || { echo "SITE BUILD FAILED"; exit 1; }
SOUT="$(cd "$S/run" && timeout 60 "$S/t" 2>&1)"; SRC=$?
echo "$SOUT"
[ "$SRC" -eq 0 ] && printf '%s\n' "$SOUT" | grep -q '^selftest ok$'
