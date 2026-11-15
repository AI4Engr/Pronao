#!/usr/bin/env bash
# The .deb from scripts/package_linux.sh: it must build, say what it depends on (GTK 3 and WebKitGTK among the packages), hold the program, a launcher entry that validates
# (desktop-file-validate, when installed) and the icon at the size of the PNG, refuse bad names and versions, and the program taken out of the package must still run
# (the site self-test of tests/site_window.clg prints "selftest ok"). It does not install the package (that is dpkg's job, and changes the machine).
# Needs dpkg-deb and a display (X11, or WSLg with GDK_BACKEND=x11).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory
SCRIPT="${PACKAGE_SCRIPT:-$HERE/scripts/package_linux.sh}"
W="${PACKAGE_WORK:-${TMPDIR:-/tmp}/pronao-package-linux}"
rm -rf "$W"; mkdir -p "$W/work" "$W/run" "$W/out"
ASSETS="$HERE/tests/site_window" WORK="$W/work" bash "$HERE/scripts/build.sh" "$HERE/tests/site_window.clg" "$W/prog" > "$W/build.log" 2>&1 || { cat "$W/build.log"; echo "BUILD FAILED"; exit 1; }
python3 - "$W/icon.png" <<'PYEOF'
import sys, zlib, struct
w = h = 48
raw = b''.join(b'\x00' + b''.join(bytes((30, 120, 200, 255)) for x in range(w)) for y in range(h))
def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
PYEOF
fail() { echo "FAILED: $1"; exit 1; }
APP_TITLE="Package Test" APP_DESCRIPTION="A program for the package test" APP_MAINTAINER="Test <test@example.com>" bash "$SCRIPT" "$W/prog" pronao-package-test 1.2.3 "$W/icon.png" "$W/out" > "$W/pkg.log" 2>&1 || { cat "$W/pkg.log"; fail "the package was not built"; }
DEB="$W/out/pronao-package-test_1.2.3_$(dpkg --print-architecture).deb"
[ -f "$DEB" ] || fail "no .deb at $DEB"
INFO="$(dpkg-deb -I "$DEB")"
printf '%s\n' "$INFO" | grep -q '^ Package: pronao-package-test$' && printf '%s\n' "$INFO" | grep -q '^ Version: 1.2.3$' && printf '%s\n' "$INFO" | grep -q '^ Maintainer: Test <test@example.com>$' || fail "the control file is not what was asked: $INFO"
printf '%s\n' "$INFO" | grep -E '^ Depends:' | grep -Eq 'libgtk-3-0' && printf '%s\n' "$INFO" | grep -E '^ Depends:' | grep -Eq 'libwebkit2gtk-4\.[01]' || fail "the dependencies do not name GTK 3 and WebKitGTK: $(printf '%s\n' "$INFO" | grep '^ Depends:')"
echo "built; it depends on GTK 3 and WebKitGTK"
dpkg-deb -x "$DEB" "$W/root"
[ -x "$W/root/opt/pronao-package-test/pronao-package-test" ] && [ "$(readlink "$W/root/usr/bin/pronao-package-test")" = "/opt/pronao-package-test/pronao-package-test" ] || fail "the program or its link in /usr/bin is missing"
[ -f "$W/root/usr/share/icons/hicolor/48x48/apps/pronao-package-test.png" ] || fail "the icon is not under its own size (48x48)"
D="$W/root/usr/share/applications/pronao-package-test.desktop"
grep -q '^Name=Package Test$' "$D" && grep -q '^Exec=pronao-package-test$' "$D" && grep -q '^Icon=pronao-package-test$' "$D" || fail "the launcher entry is not right: $(cat "$D")"
if command -v desktop-file-validate > /dev/null; then desktop-file-validate "$D" || fail "the launcher entry does not validate"; echo "the launcher entry validates"; else echo "(desktop-file-validate is not installed: the launcher entry was only read)"; fi
if dpkg-deb -c "$DEB" | awk '{ print $2 }' | grep -vq 'root/root'; then fail "a file is not owned by root"; fi
for bad in "Bad_Name" "-x" "UPPER"; do
    bash "$SCRIPT" "$W/prog" "$bad" 1.0 "" "$W/out" > /dev/null 2>&1 && fail "the name '$bad' was accepted"
done
bash "$SCRIPT" "$W/prog" ok-name "v1" "" "$W/out" > /dev/null 2>&1 && fail "a version that does not start with a digit was accepted"
bash "$SCRIPT" "$W/prog" ok-name 1.0 "$W/pkg.log" "$W/out" > /dev/null 2>&1 && fail "a file that is not a PNG was accepted as the icon"
echo "bad names, versions and icons are refused"
OUT="$(cd "$W/run" && timeout 60 "$W/root/opt/pronao-package-test/pronao-package-test" 2>&1)"; RC=$?
printf '%s\n' "$OUT" | grep -aq '^selftest ok$' && [ "$RC" -eq 0 ] || fail "the program taken out of the package did not run: $(printf '%s\n' "$OUT" | grep -av dconf | tail -3)"
echo "the program taken out of the package runs"
echo "package test: ok"
if [ -z "${PACKAGE_SCRIPT:-}" ]; then
    # CONTROL: a packaging script that leaves out the dependencies must fail the test
    C="${TMPDIR:-/tmp}/pronao-package-linux-control"
    rm -rf "$C"; mkdir -p "$C"
    sed 's/^Depends: \$DEPENDS$/Depends: libc6/' "$HERE/scripts/package_linux.sh" > "$C/package_linux.sh"
    cmp -s "$C/package_linux.sh" "$HERE/scripts/package_linux.sh" && { echo "CONTROL SETUP FAILED"; exit 1; }
    if PACKAGE_SCRIPT="$C/package_linux.sh" PACKAGE_WORK="$C/work" bash "${BASH_SOURCE[0]}" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not look at the dependencies"; exit 1; fi
    echo "control: a package without its dependencies fails the test, as it must"
fi
