#!/usr/bin/env bash
# The colour scheme and dropped files in a real window on Linux (the counterpart of run_system_window.sh; the same program, tests/system_window.clg, and page).
#   theme: forcing dark and light is seen by the page (prefers-color-scheme) and reported as theme.changed; following the system returns to what it was.
#   drop:  a real drag between two windows (tests/xdrag.c offers files as text/uri-list, the way a file manager does; the mouse is driven with xdotool): the page must hear enter /
#          over / leave / drop and read both dropped files, Then a CONTROL: a shim that never delivers the paths.
# Needs a display (X11, or WSLg with GDK_BACKEND=x11), xdotool, gcc and GTK headers. The desktop changing its scheme while the program runs is played only with XSETTINGSD=1 (needs xsettingsd, and a display of its own such as Xvfb).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory

story() { # kind(theme|drop) dir shim-dir-or-empty -> 0 if the whole story worked
    local KIND="$1" S="$2" SHIMDIR="$3"
    rm -rf "$S"; mkdir -p "$S/work" "$S/run" "$S/ins"
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_system" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/system_window.clg" "$S/t" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export SYSTEM_TEST_DIR="$S/ins"
    [ "$KIND" = drop ] && export SYSTEM_TEST_DROP=on || export SYSTEM_TEST_DROP=off
    local OUT="$S/out.txt" N=0
    (cd "$S/run" && timeout 90 "$S/t" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { local n=0 limit=$(( ${2:-8} * 5 )); until grep -aEq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done; return 0; }
    fail() { echo "FAILED: $1"; kill "$pid" 2> /dev/null; bash "$HERE/tests/close_dialogs.sh" "system test" > /dev/null 2>&1; wait "$pid" 2> /dev/null; return 1; }
    do_() { N=$((N + 1)); printf '%s %s' "$N" "$*" > "$S/ins/instructions.txt"; waitfor "^done $N " 10 || return 1; }
    lastseen() { grep -a "^seen $1 " "$OUT" | tail -1 | cut -d' ' -f3-; }

    waitfor '^ready' 30 || fail "the program never became ready" || return 1
    waitfor '^seen flag ' 10 || fail "the page never reported" || return 1
    if [ "$KIND" = theme ]; then
        local sys; sys="$(lastseen scheme)"
        echo "an untouched window: the page says $sys"
        do_ theme dark && waitfor '^seen scheme dark$' 10 || fail "forced dark: the page's scheme did not become dark" || return 1
        waitfor '^seen theme .*"mode":"dark"' 10 || fail "forced dark: the page was not told (theme.changed)" || return 1
        echo "forced dark: the page is dark and was told"
        do_ theme light && waitfor '^seen scheme light$' 10 || fail "forced light: the page's scheme did not become light" || return 1
        echo "forced light: the page is light"
        do_ theme system && waitfor '^seen theme .*"mode":"system"' 10 || fail "follow the system: the page was not told" || return 1
        do_ probe && [ "$(lastseen now)" = "$sys" ] || fail "follow the system: the page is $(lastseen now), the system's was $sys" || return 1
        echo "follow the system: back to $sys"
        # the desktop changes its scheme while the program runs (played with xsettingsd, a real XSETTINGS daemon: GTK reads the theme name from it, as it does from a desktop's
        # settings daemon). Opt-in, XSETTINGSD=1, and only on a display of its own (Xvfb): it takes over the XSETTINGS of the display it runs on.
        if [ "${XSETTINGSD:-0}" = 1 ] && command -v xsettingsd > /dev/null 2>&1; then
            printf 'Net/ThemeName "Adwaita"
' > "$S/xs.conf"
            xsettingsd -c "$S/xs.conf" > "$S/xs.log" 2>&1 & local xpid=$!
            sleep 1
            do_ theme system || { kill "$xpid" 2> /dev/null; fail "theme system"; return 1; }
            local before; before="$(grep -ac '^seen theme ' "$OUT")"
            printf 'Net/ThemeName "Adwaita-dark"
' > "$S/xs.conf"; kill -HUP "$xpid"
            waitfor '^seen theme .*"system":"dark"' 10 && waitfor '^seen scheme dark$' 10 || { kill "$xpid" 2> /dev/null; fail "the desktop went dark: the page was not told"; return 1; }
            printf 'Net/ThemeName "Adwaita"
' > "$S/xs.conf"; kill -HUP "$xpid"
            waitfor '^seen theme .*"system":"light"' 10 || { kill "$xpid" 2> /dev/null; fail "the desktop went light again: the page was not told"; return 1; }
            kill "$xpid" 2> /dev/null
            [ "$(grep -ac '^seen theme ' "$OUT")" -gt "$before" ] || fail "no theme event" || return 1
            echo "the desktop went dark and light again: the page was told each time and its scheme followed"
        fi
    else
        local D="$S/dropped"; mkdir -p "$D"; printf 'alpha' > "$D/a.txt"; printf 'bravo' > "$D/b.txt"
        gcc -o "$S/xdrag" "$HERE/tests/xdrag.c" $(pkg-config --cflags --libs gtk+-3.0) 2> /dev/null || { echo "cannot build xdrag"; return 2; }
        local tid; tid="$(xdotool search --onlyvisible --name '^system test$' | head -1)"
        [ -n "$tid" ] || fail "no window of the program" || return 1
        xdotool windowmove "$tid" 600 100
        ("$S/xdrag" "$D/a.txt" "$D/b.txt" > "$S/xdrag.out" 2>&1 &)
        local sid="" i
        for i in $(seq 1 30); do sid="$(xdotool search --onlyvisible --name '^pronao drag source$' | head -1)"; [ -n "$sid" ] && break; sleep 0.2; done
        [ -n "$sid" ] || fail "no drag source window" || return 1
        xdotool windowmove "$sid" 50 100; sleep 1
        xdotool mousemove --window "$sid" 100 60; sleep 0.3; xdotool mousedown 1; sleep 0.3
        for i in 1 2 3 4 5 6 7 8; do xdotool mousemove_relative -- 20 0; sleep 0.1; done
        xdotool mousemove --window "$tid" 300 200; sleep 0.5; xdotool mousemove --window "$tid" 305 205; sleep 0.5
        xdotool mouseup 1
        if [ "$SYSTEM_TEST_DROP" = on ]; then
            waitfor '^seen drop ' 10 || fail "the page never heard of the drop" || return 1
            [ "$(lastseen drop)" = '{"n":2,"skipped":0,"kinds":["file","file"],"texts":["alpha","bravo"]}' ] || fail "the drop is not what was dragged: $(lastseen drop)" || return 1
            grep -aq '^seen enter ' "$OUT" && grep -aq '^seen over ' "$OUT" || fail "no enter / over" || return 1
            echo "dropped two files: the page heard enter, over, drop and read both"
        fi
    fi
    do_ quit
    wait "$pid"
    return 0
}

story theme "${TMPDIR:-/tmp}/pronao-sys-theme" "" || { echo "system test (theme): FAILED"; exit 1; }
story drop "${TMPDIR:-/tmp}/pronao-sys-drop" "" || { echo "system test (drop): FAILED"; exit 1; }
echo "system test: ok"

C="${TMPDIR:-/tmp}/pronao-sys-control"
rm -rf "$C"; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/posix/drop.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "    if (d.paths.empty()) return;\n    std::lock_guard<std::mutex> lock(g_mutex);\n    g_last[handle] = d;"
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "    return;"))
PYEOF
if story drop "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the paths"; exit 1; fi
echo "control: without the paths from the desktop the test fails, as it must"
