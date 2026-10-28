#!/usr/bin/env bash
# A real window on a development server (python's http.server on a free local port) and the same program built with RELEASE=1, which must
# refuse to load any server. Needs python3 and the WebView2 runtime.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/app-cling-dev"
rm -rf "$W" 2>/dev/null || true; mkdir -p "$W/work" "$W/work-release" "$W/run"
PORT="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$HERE/tests/dev_site" > "$W/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/dev_window.clg" "$W/t.exe" || { echo "BUILD FAILED"; exit 1; }
RELEASE=1 WORK="$W/work-release" bash "$HERE/scripts/build.sh" "$HERE/tests/dev_window.clg" "$W/release.exe" || { echo "RELEASE BUILD FAILED"; exit 1; }
for i in $(seq 1 50); do curl -s -o /dev/null "http://127.0.0.1:$PORT/" && break; sleep 0.1; done
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$W/profile-$$")"
export APP_DEV_URL="http://127.0.0.1:$PORT/"
OUT="$(cd "$W/run" && timeout 45 "$W/t.exe" 2>&1)"; RC=$?
echo "$OUT"
[ "$RC" -eq 124 ] && { echo "dev window: TIMED OUT"; exit 1; }
{ [ "$RC" -eq 0 ] && printf '%s\n' "$OUT" | grep -q '^selftest ok$'; } || exit 1
# a release build refuses the same address
ROUT="$(cd "$W/run" && timeout 20 "$W/release.exe" 2>&1)"; RRC=$?
echo "release: $ROUT (exit $RRC)"
[ "$RRC" -eq 3 ] && printf '%s\n' "$ROUT" | grep -q '^dev: refused$'
