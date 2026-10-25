#!/usr/bin/env bash
# Builds tests/window_selftest.clg against the real webview and runs it in a real window.
# Needs third_party/ (scripts/fetch.sh) and the WebView2 runtime. Passes only on exit code 0 AND "selftest ok".
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/app-cling-window"
rm -rf "$W"; mkdir -p "$W/work"
PAGE="$HERE/tests/selftest.html" WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/window_selftest.clg" "$W/t.exe" || { echo "BUILD FAILED"; exit 1; }
[ -f "$W/t.exe" ] || { echo "BUILD FAILED (no binary)"; exit 1; }
OUT="$(timeout 30 "$W/t.exe" 2>&1)"; RC=$?
echo "$OUT"
[ "$RC" -eq 124 ] && { echo "window selftest: TIMED OUT"; exit 1; }
[ "$RC" -eq 0 ] && printf '%s\n' "$OUT" | grep -q '^selftest ok$'
