#!/usr/bin/env bash
# Builds tests/window_selftest.clg against the real webview and runs it in a real window.
# Needs third_party/ (scripts/fetch.sh) and the WebView2 runtime. Passes only on exit code 0 AND "selftest ok".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/pronao-window"
rm -rf "$W" 2>/dev/null || true; mkdir -p "$W/work"
PAGE="$HERE/tests/selftest.html" WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/window_selftest.clg" "$W/t.exe" || { echo "BUILD FAILED"; exit 1; }
[ -f "$W/t.exe" ] || { echo "BUILD FAILED (no binary)"; exit 1; }
mkdir -p "$W/run/files"
# a private browser profile per run: a previous run's browser processes may still hold the default one for a few seconds
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$W/profile-$$")"
# the page opens a real message box and a real Open dialog; this closes them the way a user would
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/tests/close_dialogs.ps1")" "main:pronao selftest ready" "no:pronao selftest close" "pronao selftest message" "pronao selftest open" > "$W/dialogs.log" 2>&1 &
CLOSER=$!
OUT="$(cd "$W/run" && timeout 45 "$W/t.exe" 2>&1)"; RC=$?
wait $CLOSER; CLOSERC=$?
echo "$OUT"
[ "$CLOSERC" -eq 0 ] || { echo "dialog helper: $(cat "$W/dialogs.log")"; exit 1; }
[ "$RC" -eq 124 ] && { echo "window selftest: TIMED OUT"; exit 1; }
[ "$RC" -eq 0 ] && printf '%s\n' "$OUT" | grep -q '^selftest ok$'

# the same window, but with the page served by the program from memory (several files)
S="${TMPDIR:-/tmp}/pronao-site"
rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run"
ASSETS="$HERE/tests/site_window" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/site_window.clg" "$S/t.exe" || { echo "SITE BUILD FAILED"; exit 1; }
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"      # (a fresh profile: the last run's browser may still be shutting down)
SOUT="$(cd "$S/run" && timeout 60 "$S/t.exe" 2>&1)"; SRC=$?
echo "$SOUT"
[ "$SRC" -eq 0 ] && printf '%s\n' "$SOUT" | grep -q '^selftest ok$'
