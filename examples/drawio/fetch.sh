#!/usr/bin/env bash
# Downloads draw.io (jgraph/drawio, Apache-2.0) at a PINNED release, checks its SHA-256, keeps only what the desktop editor needs and adds
# the bridge (pronao-electron.js). Nothing of draw.io is committed: the result is examples/drawio/site/ (gitignored).
#   bash examples/drawio/fetch.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="32.6.10"
URL="https://github.com/jgraph/drawio/archive/refs/tags/v$VERSION.tar.gz"
SHA="b83e3d92c44bc18c3829a9b3005c89221acaadc824f2901246f2a071856468a0"
DL="$HERE/_dl"; SITE="$HERE/site"
mkdir -p "$DL"
TGZ="$DL/drawio-$VERSION.tar.gz"
if [ ! -f "$TGZ" ] || [ "$(sha256sum "$TGZ" | cut -d' ' -f1)" != "$SHA" ]; then
    curl -sSL -m 600 -o "$TGZ" "$URL"
    got="$(sha256sum "$TGZ" | cut -d' ' -f1)"
    [ "$got" = "$SHA" ] || { echo "SHA-256 mismatch for $URL: wanted $SHA, got $got" >&2; rm -f "$TGZ"; exit 1; }
fi
rm -rf "$SITE" "$DL/x"; mkdir -p "$DL/x" "$SITE"
tar xzf "$TGZ" -C "$DL/x" "drawio-$VERSION/src/main/webapp" "drawio-$VERSION/LICENSE" "drawio-$VERSION/NOTICE" 2>/dev/null \
    || tar xzf "$TGZ" -C "$DL/x" "drawio-$VERSION/src/main/webapp" "drawio-$VERSION/LICENSE"
W="$DL/x/drawio-$VERSION/src/main/webapp"
cp -r "$W/." "$SITE/"
cp "$DL/x/drawio-$VERSION/LICENSE" "$SITE/LICENSE-drawio.txt"

# What a desktop editor does not need: the server side, the embed and viewer builds (the editor build already contains what it uses), the sources the
# minified build was made from (but two files), source maps, the web app's service worker, other sites' integrations, and every language but English.
rm -rf "$SITE/WEB-INF" "$SITE/META-INF" "$SITE/connect" "$SITE/js/grapheditor"
# js/diagramly holds the unminified sources; the desktop editor loads only these two of them next to app.min.js
find "$SITE/js/diagramly" -type f ! -name ElectronApp.js ! -name DesktopLibrary.js -delete
find "$SITE/js/diagramly" -type d -empty -delete 2>/dev/null || true
rm -f "$SITE"/js/integrate.min.js "$SITE"/js/viewer.min.js "$SITE"/js/viewer-static.min.js "$SITE"/js/embed*.js "$SITE"/js/export*.js
rm -f "$SITE"/service-worker.js* "$SITE"/workbox-*.js* "$SITE"/teams.html "$SITE"/onedrive3.html "$SITE"/export3.html "$SITE"/open.html "$SITE"/clear.html
rm -f "$SITE"/monday-app-association.json
find "$SITE" -name '*.map' -delete
find "$SITE/resources" -type f ! -name 'dia.txt' ! -name 'dia_en.txt' ! -name 'dia_zh.txt' -delete 2>/dev/null || true

# the bridge, loaded before bootstrap.js (which turns desktop mode on from the browser's user agent)
cp "$HERE/pronao-electron.js" "$SITE/pronao-electron.js"
PY="${PYTHON:-$(command -v python3 || command -v python)}"
"$PY" -I - "$SITE/index.html" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
a = '<script src="js/bootstrap.js"></script>'
assert s.count(a) == 1, 'index.html changed: bootstrap.js tag not found'
open(p, 'w', encoding='utf-8').write(s.replace(a, '<script src="pronao-electron.js"></script>\n\t' + a))
PYEOF
echo "draw.io $VERSION ready in examples/drawio/site: $(du -sm "$SITE" | cut -f1) MB, $(find "$SITE" -type f | wc -l) files"
