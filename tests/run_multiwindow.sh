#!/usr/bin/env bash
# Two real windows of one program (shim/win + WebView2), both served from memory. Passes on exit code 0 AND "selftest ok". Without the fix in guardProc
# (WM_DESTROY ends that window's own message loop) the first window to close leaves the program hanging and this test times out.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="${TMPDIR:-/tmp}/pronao-multi"
rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run"
ASSETS="$HERE/tests/site_multi" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/multiwindow_window.clg" "$S/t.exe" || { echo "BUILD FAILED"; exit 1; }
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
OUT="$(cd "$S/run" && timeout 60 "$S/t.exe" 2>&1)"; RC=$?
echo "$OUT"
[ "$RC" -eq 124 ] && { echo "multi-window test: TIMED OUT"; exit 1; }
[ "$RC" -eq 0 ] && printf '%s\n' "$OUT" | grep -q '^selftest ok$'

# CONTROL: the same test against a shim whose guardProc does not end a window's own message loop on WM_DESTROY. The webview library does that only
# for the LAST window of the process, so the window the application closed is never reported closed; the test must see that and fail.
C="${TMPDIR:-/tmp}/pronao-multi-control"
rm -rf "$C" 2>/dev/null || true; mkdir -p "$C/work" "$C/run"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/app_shim.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "PostQuitMessage(0);                                            // this window's own message loop ends (tests/run_multiwindow.sh removes this line as its control)"
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "/* WM_DESTROY handling removed */"))
PYEOF
SHIM="$C/shim" ASSETS="$HERE/tests/site_multi" WORK="$C/work" bash "$HERE/scripts/build.sh" "$HERE/tests/multiwindow_window.clg" "$C/t.exe" || { echo "CONTROL BUILD FAILED"; exit 1; }
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$C/profile-$$")"
COUT="$(cd "$C/run" && timeout 60 "$C/t.exe" 2>&1)"; CRC=$?
if [ "$CRC" -eq 0 ] && printf '%s\n' "$COUT" | grep -q '^selftest ok$'; then echo "CONTROL DID NOT FAIL: the multi-window test does not need the WM_DESTROY fix"; exit 1; fi
echo "control: without the WM_DESTROY fix the test fails, as it must"
