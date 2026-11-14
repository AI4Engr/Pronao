#!/usr/bin/env bash
# Builds the page of this example: a small React host for the Excalidraw component (MIT, https://github.com/excalidraw/excalidraw), at the versions pinned in
# app/package.json and app/package-lock.json, into examples/excalidraw/site/ (gitignored). Needs Node 18 or newer and npm, and downloads the packages once.
#   bash examples/excalidraw/build_site.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$HERE/app"; SITE="$HERE/site"
command -v node > /dev/null 2>&1 || { echo "build_site.sh needs Node.js on PATH (https://nodejs.org)" >&2; exit 1; }
command -v npm > /dev/null 2>&1 || { echo "build_site.sh needs npm on PATH" >&2; exit 1; }
cd "$APP"
# the exact versions of app/package-lock.json, and no scripts of the packages are run
npm ci --no-audit --no-fund --ignore-scripts --loglevel=error
npx vite build > "$HERE/.vite.log" 2>&1 || { cat "$HERE/.vite.log"; exit 1; }
rm -rf "$SITE"; mkdir -p "$SITE"
cp -r "$APP/dist/." "$SITE/"
# the fonts of the component, so that nothing is fetched from a CDN (window.EXCALIDRAW_ASSET_PATH points here)
cp -r "$APP/node_modules/@excalidraw/excalidraw/dist/prod/fonts" "$SITE/fonts"
# LEAN=1 leaves out the Chinese hand-drawn font (Xiaolai, 209 small files, 12.9 MB that do not compress): text in Chinese is then drawn in a system font
# instead of the hand-drawn one. Every other font stays.
if [ -n "${LEAN:-}" ]; then rm -rf "$SITE/fonts/Xiaolai"; fi
cp "$APP/node_modules/@excalidraw/excalidraw/package.json" "$SITE/excalidraw-package.json"
# the licences of everything the page is built from, with their texts, carried by the program
(cd "$APP" && node make_notices.mjs "$SITE/THIRD-PARTY-NOTICES.txt")
# Excalidraw's own licence text is not in the npm package: it is MIT, and the notice is written out here so that the program carries it
cat > "$SITE/LICENSE-excalidraw.txt" <<'LICENSE'
Excalidraw is distributed under the MIT License, Copyright (c) 2020 Excalidraw (https://github.com/excalidraw/excalidraw/blob/master/LICENSE).
The other packages this program contains, with their licences and licence texts, are listed in THIRD-PARTY-NOTICES.txt.
LICENSE
echo "excalidraw site ready in examples/excalidraw/site: $(du -sm "$SITE" | cut -f1) MB, $(find "$SITE" -type f | wc -l) files"
