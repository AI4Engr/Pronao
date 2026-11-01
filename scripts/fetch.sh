#!/usr/bin/env bash
# Downloads the two third-party pieces app-cling's window layer needs, at PINNED versions, and checks their
# SHA-256 before unpacking. Nothing is committed from here (third_party/ is gitignored): Microsoft's WebView2
# SDK is not ours to redistribute, and the webview library is fetched rather than vendored so its version is
# an explicit, reviewable line in this file.
#
#   webview/webview 0.12.0   MIT        github.com/webview/webview
#   Microsoft.Web.WebView2 1.0.2210.55  (newer than the default webview 0.12 names: E8 and E18 need ICoreWebView2Profile::PreferredColorScheme and Controller4::AllowExternalDrop)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party/_dl
WEBVIEW_URL="https://github.com/webview/webview/archive/refs/tags/0.12.0.tar.gz"
WEBVIEW_SHA="e2c8d0bed3fcd13e624074448b7d3b1d0b0d03546a15839f17ee0013f2cba1db"
WV2_URL="https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.2210.55/microsoft.web.webview2.1.0.2210.55.nupkg"
WV2_SHA="c0945ddfe38272f10f24673a1cb07b54b018f614eec85d3e830628369039ad3d"

fetch() {  # url sha out
    if [ -f "$3" ] && [ "$(sha256sum "$3" | cut -d' ' -f1)" = "$2" ]; then return 0; fi
    curl -sSL -m 300 -o "$3" "$1"
    got="$(sha256sum "$3" | cut -d' ' -f1)"
    if [ "$got" != "$2" ]; then echo "SHA-256 mismatch for $1: wanted $2, got $got" >&2; rm -f "$3"; exit 1; fi
}
fetch "$WEBVIEW_URL" "$WEBVIEW_SHA" third_party/_dl/webview-0.12.0.tar.gz
fetch "$WV2_URL" "$WV2_SHA" third_party/_dl/webview2-1.0.2210.55.nupkg

rm -rf third_party/webview third_party/webview2
mkdir -p third_party/webview third_party/webview2
tar xzf third_party/_dl/webview-0.12.0.tar.gz -C third_party/webview --strip-components=1 \
    webview-0.12.0/core webview-0.12.0/LICENSE
PY="${PYTHON:-$(command -v python3 || command -v python || true)}"
[ -n "$PY" ] || { echo "fetch.sh needs Python 3 on PATH (or PYTHON=<path>) to unpack the WebView2 package" >&2; exit 1; }
"$PY" -I - <<'PYEOF'
import zipfile
z = zipfile.ZipFile("third_party/_dl/webview2-1.0.2210.55.nupkg")
for n in z.namelist():
    if n.startswith("build/native/include/") or n.startswith("build/native/x64/") or n == "LICENSE.txt":
        z.extract(n, "third_party/webview2")
PYEOF
echo "third_party ready: webview 0.12.0, WebView2 SDK 1.0.2210.55"
