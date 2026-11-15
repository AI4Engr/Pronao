#!/usr/bin/env bash
# One running copy of a program, for real, on Linux: a window, a second process that must hand its arguments over and end, and the page that must hear of it
# (the Linux counterpart of run_instance_window.sh; the same program, tests/instance_window.clg). Then a CONTROL: a shim whose instance never delivers
# what it receives; the first never hears of the second launch and the test must fail. Needs a display (X11, or WSLg with GDK_BACKEND=x11).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export GDK_BACKEND="${GDK_BACKEND:-x11}" DISPLAY="${DISPLAY:-:0}" GSETTINGS_BACKEND=memory

run_case() { # dir  shim-dir-or-empty  -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S"; mkdir -p "$S/work" "$S/run"
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_instance" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/instance_window.clg" "$S/t" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    (cd "$S/run" && timeout 45 "$S/t" > "$S/first.out" 2>&1; echo $? > "$S/first.rc") &
    local first=$! waited=0
    until grep -aq '^first ready' "$S/first.out" 2> /dev/null; do
        sleep 0.2; waited=$((waited + 1))
        [ "$waited" -gt 100 ] && { echo "the first instance never became ready"; grep -av 'dconf\|Dropped' "$S/first.out" | head; kill "$first" 2> /dev/null; return 1; }
    done
    local second src
    second="$(cd "$S/run" && timeout 20 "$S/t" alpha "b c" 2>&1)"; src=$?
    echo "second launch: exit $src: $second"
    wait "$first"
    local frc; frc="$(cat "$S/first.rc" 2> /dev/null)"
    echo "first instance: exit $frc: $(grep -av 'dconf\|Dropped' "$S/first.out" | tail -2 | tr '\n' ' ')"
    [ "$src" -eq 0 ] && printf '%s\n' "$second" | grep -q 'second launch handed over' && [ "$frc" = 0 ] && grep -aq '^selftest ok' "$S/first.out"
}

run_case "${TMPDIR:-/tmp}/pronao-instance-linux" "" || { echo "instance test: FAILED"; exit 1; }
echo "instance test: ok"

C="${TMPDIR:-/tmp}/pronao-instance-linux-control"
rm -rf "$C"; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/posix/instance.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "if (!tooLong && !message.empty()) deliver(in, message);"
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "/* delivery removed */"))
PYEOF
if run_case "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the delivery"; exit 1; fi
echo "control: without the delivery the test fails, as it must"
