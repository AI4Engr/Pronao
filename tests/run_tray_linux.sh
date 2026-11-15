#!/usr/bin/env bash
# The tray icon on Linux, up to the desktop's panel: the program registers a StatusNotifierItem with the watcher (tests/tray_mock.py plays the watcher and the panel on a private
# session bus, and calls the item the way a panel does). Checked: the icon registers and has its tooltip and a pixmap (when the program has a PNG icon); the menu the panel reads
# is the one the program declared (labels with mnemonics, a separator, the tick); choosing an item reaches the program (and a tick the program sets is what the panel then reads);
# a click on the icon reaches the program; a notification from the tray icon reaches the notification service and its click comes back; Quit ends the program and the name
# leaves the bus (so the panel removes the icon); with no watcher the program is told it has no tray icon. Then a CONTROL: a shim that does not report chosen items.
# What it cannot see: the icon and the menu drawn by a real panel (KDE, XFCE, GNOME with the AppIndicator extension), and a desktop without a tray.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory

story() { # dir shim-dir-or-empty -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S"; mkdir -p "$S/work" "$S/run"
    python3 - "$S/icon.png" <<'PYEOF'
import sys, zlib, struct
w = h = 64
raw = b''.join(b'\x00' + b''.join((bytes((200, 60, 40, 255)) if (x - 32) ** 2 + (y - 32) ** 2 < 28 ** 2 else bytes((0, 0, 0, 0))) for x in range(w)) for y in range(h))
def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
PYEOF
    ICON="$S/icon.png" SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_placement" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/tray_window.clg" "$S/t" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export S HERE
    dbus-run-session -- bash -c '
        say() { printf "%s\n" "$1" > "$S/cmd"; local n=0; while [ -e "$S/cmd" ] && [ "$n" -lt 50 ]; do sleep 0.1; n=$((n + 1)); done; sleep 0.2; }
        python3 "$HERE/tests/tray_mock.py" "$S/tray.log" "$S/cmd" > "$S/tray.out" 2>&1 &
        python3 "$HERE/tests/notify_mock.py" "$S/notify.log" "$S/nclick" "$S/nstop" > "$S/notify.out" 2>&1 &
        sleep 2
        cd "$S/run"; timeout 60 ../t > "$S/out.txt" 2>&1 &
        n=0; until grep -aq "^registered" "$S/tray.log" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt 100 ] && break; done
        sleep 0.5
        say layout; say tooltip
        say "click 2"; say layout
        say activate
        say "click 3"; sleep 1; touch "$S/nclick"; sleep 1.5
        say "click 5"; sleep 3
        say quit; touch "$S/nstop"; sleep 0.5' > /dev/null 2>&1
    local OUT="$S/out.txt" L="$S/tray.log"
    grep -aq '^registered org.kde.StatusNotifierItem-' "$L" || { echo "FAILED: the icon was never registered with the watcher: $(cat "$S/out.txt" 2> /dev/null | grep -av dconf | head -3)"; return 1; }
    grep -aq '^layout _Show window|1|0 ; _Mute|1|0 ; _Notify|1|0 ; - ; _Quit|1|0$' "$L" || { echo "FAILED: the menu the panel reads is not the declared one: $(grep -a '^layout' "$L" | head -1)"; return 1; }
    grep -aq '^tooltip tray test pixmaps=32x32$' "$L" || { echo "FAILED: tooltip or icon: $(grep -a '^tooltip' "$L")"; return 1; }
    echo "the icon registered; the panel read the declared menu, the tooltip and a 32x32 icon"
    grep -aq '^menu tray.mute$' "$OUT" || { echo "FAILED: choosing Mute did not reach the program"; return 1; }
    [ "$(grep -a '^layout' "$L" | sed -n 2p)" = 'layout _Show window|1|0 ; _Mute|1|1 ; _Notify|1|0 ; - ; _Quit|1|0' ] || { echo "FAILED: the tick the program set is not what the panel reads: $(grep -a '^layout' "$L" | sed -n 2p)"; return 1; }
    echo "choosing an item reached the program, and the tick it set is what the panel then read"
    grep -aq '^tray click$' "$OUT" || { echo "FAILED: a click on the icon did not reach the program"; return 1; }
    grep -aq '^notified ok$' "$OUT" && grep -aq '^notify .* 1 tray test | this is a notification | actions=1$' "$S/notify.log" && grep -aq '^notification click note-1$' "$OUT" || { echo "FAILED: the notification from the tray icon, or its click"; return 1; }
    echo "a click reached the program; a notification went to the service and its click came back"
    grep -aq '^menu tray.quit$' "$OUT" && grep -aq '^closed$' "$OUT" || { echo "FAILED: Quit did not end the program"; return 1; }
    grep -aq '^gone org.kde.StatusNotifierItem-' "$L" || { echo "FAILED: the name of the icon did not leave the bus when the program ended"; return 1; }
    echo "Quit ended the program and the name left the bus"
    return 0
}

story "${TMPDIR:-/tmp}/pronao-tray-linux" "" || { echo "tray test: FAILED"; exit 1; }

# no watcher (a desktop without a tray): the program is told
N="${TMPDIR:-/tmp}/pronao-tray-linux-none"
rm -rf "$N"; mkdir -p "$N/run"
( cd "$N/run" && timeout 30 dbus-run-session -- "${TMPDIR:-/tmp}/pronao-tray-linux/t" > "$N/out.txt" 2>&1 & )
sleep 10
grep -aq 'selftest FAILED: no tray icon' "$N/out.txt" || { echo "tray test: with no watcher the program did not say it has no tray icon: $(grep -av dconf "$N/out.txt" | head -3)"; exit 1; }
echo "with no watcher on the bus the program is told it has no tray icon"
echo "tray test: ok"

C="${TMPDIR:-/tmp}/pronao-tray-linux-control"
rm -rf "$C"; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/posix/tray.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = 'tell(t->handle, "menu", n.cid);'
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, ';'))
PYEOF
if story "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need chosen items to be reported"; exit 1; fi
echo "control: without reporting chosen items the test fails, as it must"
