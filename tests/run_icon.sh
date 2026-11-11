#!/usr/bin/env bash
# The program's icon, for real (Windows): ICON=<file.ico> must end up (1) in the exe, where Explorer reads it, and (2) on the window, where the title bar,
# Alt+Tab and the taskbar read it. The placeholder is red in the middle (make_icon.py is given the colour), and the test reads the colour back from the built exe
# and from the running window. Then the same build WITHOUT an icon must show no red (the control), and a file that is not an .ico must be refused by the build.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="${TMPDIR:-/tmp}/pronao-icon"
rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/with space" "$S/run"
P="$S/poke.exe"
g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$P" -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; exit 1; }

# the icon: the same bytes every time, and the right shape
python3 "$HERE/scripts/make_icon.py" "$S/with space/my icon.ico" red-app ff0000 || { echo "make_icon failed"; exit 1; }
python3 "$HERE/scripts/make_icon.py" "$S/again.ico" red-app ff0000
cmp -s "$S/with space/my icon.ico" "$S/again.ico" || { echo "make_icon is not deterministic"; exit 1; }
python3 "$HERE/scripts/make_icon.py" "$S/x.ico" x nothex 2> /dev/null && { echo "make_icon accepted a colour that is not hex"; exit 1; }
[ "$(head -c6 "$S/again.ico" | od -An -tx1 | tr -d ' \n')" = "000001000300" ] || { echo "the .ico header is wrong"; exit 1; }
python3 "$HERE/scripts/make_icon.py" "$S/a.ico" alpha; python3 "$HERE/scripts/make_icon.py" "$S/b.ico" beta
cmp -s "$S/a.ico" "$S/b.ico" && { echo "two names gave the same placeholder"; exit 1; }

build() { # out.exe [ICON path]
    ICON="${2:-}" ASSETS="$HERE/tests/site_placement" WORK="$S/work-$(basename "$1")" bash "$HERE/scripts/build.sh" "$HERE/tests/placement_window.clg" "$1" > "$1.log" 2>&1
}
build "$S/with.exe" "$S/with space/my icon.ico" || { cat "$S/with.exe.log"; echo "BUILD WITH ICON FAILED"; exit 1; }
build "$S/without.exe" "" || { cat "$S/without.exe.log"; echo "BUILD WITHOUT ICON FAILED"; exit 1; }

# a file that is not an icon, and one that is not there
printf 'not an icon' > "$S/fake.ico"
build "$S/fake.exe" "$S/fake.ico" && { echo "the build accepted a file that is not an .ico"; exit 1; }
grep -q "not an .ico" "$S/fake.exe.log" || { cat "$S/fake.exe.log"; echo "the refusal does not say why"; exit 1; }
build "$S/missing.exe" "$S/nothing.ico" && { echo "the build accepted an icon that does not exist"; exit 1; }

# 1: the exe
got="$("$P" exeicon "$(cygpath -w "$S/with.exe")")"; echo "exe with icon: $got"
[ "$got" = "ff0000 255" ] || { echo "the exe does not show the icon"; exit 1; }
got="$("$P" exeicon "$(cygpath -w "$S/without.exe")" 2>&1)"; rc=$?
echo "exe without icon: rc=$rc $got"
[ "$got" != "ff0000 255" ] || { echo "CONTROL DID NOT FAIL: a build without ICON shows the red icon"; exit 1; }

# 2: the window, big and small
export APPDATA="$(cygpath -w "$S/roaming")" HOME="$S/home" XDG_CONFIG_HOME="$S/home/.config"
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
window_icons() { # exe -> prints "big small"
    (cd "$S/run" && timeout 60 "$1" > "$1.out" 2>&1) &
    local pid=$! waited=0
    until grep -q '^ready' "$1.out" 2> /dev/null && "$P" wait "placement test"; do
        sleep 0.2; waited=$((waited + 1))
        [ "$waited" -gt 150 ] && { echo "the program never became ready"; kill "$pid" 2> /dev/null; return 1; }
    done
    local big small
    big="$("$P" icon "placement test" big 2>&1)"; small="$("$P" icon "placement test" small 2>&1)"
    "$P" close "placement test"
    wait "$pid"
    echo "$big / $small"
}
got="$(window_icons "$S/with.exe")"; echo "window with icon: $got"
[ "$got" = "ff0000 255 / ff0000 255" ] || { echo "the window does not show the icon (big and small)"; exit 1; }
got="$(window_icons "$S/without.exe")"; echo "window without icon: $got"
case "$got" in *"ff0000 255"*) echo "CONTROL DID NOT FAIL: a window without ICON shows the red icon"; exit 1 ;; esac
echo "icon test: ok"
