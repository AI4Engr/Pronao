#!/usr/bin/env bash
# Makes a .deb of a program built for Linux by scripts/build.sh (a Debian / Ubuntu package that installs the program, a launcher entry with its icon, and depends on the
# libraries it was linked against). Run it on the machine that built the program: the dependencies are found by asking the system which packages own the libraries
# the program loads (ldd + dpkg -S), so they match what you built against, without version numbers.
#   bash scripts/package_linux.sh <program> <package-name> <version> [icon.png] [outdir]
# Environment: APP_TITLE (the name in the launcher, default the package name), APP_DESCRIPTION, APP_MAINTAINER ("Name <mail>", default the git user), APP_CATEGORIES
# (default "Utility;"), APP_SECTION (default "utils").
# The program is installed as /opt/<package-name>/<package-name> with a link in /usr/bin, so it can keep files beside itself. A package from here is NOT signed
# (apt repositories sign their index; dpkg-sig or a repository does the rest) and does not carry WebKitGTK: it depends on the system's.
set -euo pipefail
[ $# -ge 3 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
PROG="$1"; NAME="$2"; VERSION="$3"; ICON="${4:-}"; OUT="${5:-.}"
[ -x "$PROG" ] || { echo "package_linux: $PROG is not an executable program" >&2; exit 1; }
case "$NAME" in *[!a-z0-9.+-]*|""|[!a-z0-9]*) echo "package_linux: the package name must be lowercase letters, digits and . + - (starting with a letter or digit)" >&2; exit 1 ;; esac
case "$VERSION" in [0-9]*) ;; *) echo "package_linux: the version must start with a digit" >&2; exit 1 ;; esac
case "$VERSION" in *[!A-Za-z0-9.+~-]*) echo "package_linux: the version may hold letters, digits and . + ~ - only" >&2; exit 1 ;; esac
command -v dpkg-deb > /dev/null && command -v ldd > /dev/null && command -v dpkg > /dev/null || { echo "package_linux: needs dpkg-deb, dpkg and ldd (a Debian-based system)" >&2; exit 1; }
if [ -n "$ICON" ]; then
    [ -f "$ICON" ] || { echo "package_linux: $ICON does not exist" >&2; exit 1; }
    [ "$(head -c8 "$ICON" | od -An -tx1 | tr -d ' \n')" = "89504e470d0a1a0a" ] || { echo "package_linux: the icon must be a .png" >&2; exit 1; }
    SIZE="$(od -An -tu1 -j16 -N8 "$ICON" | awk '{ print $1*16777216+$2*65536+$3*256+$4 "x" $5*16777216+$6*65536+$7*256+$8 }')"
fi
TITLE="${APP_TITLE:-$NAME}"
DESC="${APP_DESCRIPTION:-$TITLE}"
MAINT="${APP_MAINTAINER:-$(git config user.name 2> /dev/null || echo unknown) <$(git config user.email 2> /dev/null || echo unknown@localhost)>}"
CATS="${APP_CATEGORIES:-Utility;}"
SECTION="${APP_SECTION:-utils}"
for v in "$TITLE" "$DESC" "$MAINT" "$CATS"; do case "$v" in *$'\n'*) echo "package_linux: a title, description, maintainer or category may not hold a line break" >&2; exit 1 ;; esac; done
ARCH="$(dpkg --print-architecture)"

# the packages that own the libraries the program asks for directly (the ones it was linked against; what those need in turn is their own packages' business)
DEPS=""
LDD="$(ldd "$PROG")"
if command -v readelf > /dev/null; then NEEDED="$(readelf -d "$PROG" | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')"; else NEEDED="$(printf '%s\n' "$LDD" | awk '/=> \// { print $1 }')"; fi
while read -r soname; do
    [ -n "$soname" ] || continue
    lib="$(printf '%s\n' "$LDD" | awk -v n="$soname" '$1 == n && $2 == "=>" { print $3; exit }')"
    [ -n "$lib" ] || continue
    # (dpkg records a library under /lib or /usr/lib, whichever the package used, and the two are one directory on a merged-/usr system: try every spelling)
    real="$(readlink -f "$lib")"; pkg=""
    for candidate in "$real" "$lib" "${real#/usr}" "${lib#/usr}"; do
        pkg="$(dpkg -S "$candidate" 2> /dev/null | head -1 | cut -d: -f1 || true)"
        [ -n "$pkg" ] && break
    done
    [ -n "$pkg" ] && DEPS="$DEPS
$pkg"
done <<< "$NEEDED"
DEPENDS="$(printf '%s\n' "$DEPS" | sort -u | grep -v '^$' | paste -sd, - | sed 's/,/, /g' || true)"
[ -n "$DEPENDS" ] || DEPENDS="libc6"

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
install -d "$ROOT/DEBIAN" "$ROOT/opt/$NAME" "$ROOT/usr/bin" "$ROOT/usr/share/applications"
install -m 0755 "$PROG" "$ROOT/opt/$NAME/$NAME"
ln -s "/opt/$NAME/$NAME" "$ROOT/usr/bin/$NAME"
ICONLINE=""
if [ -n "$ICON" ]; then
    install -d "$ROOT/usr/share/icons/hicolor/$SIZE/apps"
    install -m 0644 "$ICON" "$ROOT/usr/share/icons/hicolor/$SIZE/apps/$NAME.png"
    ICONLINE="Icon=$NAME"
fi
cat > "$ROOT/usr/share/applications/$NAME.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$TITLE
Comment=$DESC
Exec=$NAME
Terminal=false
Categories=$CATS
StartupWMClass=$NAME
$ICONLINE
DESKTOP
INSTALLED="$(du -sk "$ROOT" | cut -f1)"
cat > "$ROOT/DEBIAN/control" <<CONTROL
Package: $NAME
Version: $VERSION
Section: $SECTION
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Installed-Size: $INSTALLED
Maintainer: $MAINT
Description: $DESC
CONTROL
mkdir -p "$OUT"
FILE="$OUT/${NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$ROOT" "$FILE" > /dev/null
echo "package_linux: $FILE"
echo "  depends: $DEPENDS"
