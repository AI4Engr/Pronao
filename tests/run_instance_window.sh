#!/usr/bin/env bash
# One running copy of a program, for real: a window, a second process that must hand its arguments over and end, and the page that must hear of it.
# Passes if the second launch exits 0 at once without a window and the first reports "selftest ok". Then a CONTROL: the same test against a shim whose
# instance never delivers what it receives; the first never hears of the second launch and the test must fail (time out).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_case() { # dir  shim-dir-or-empty  -> 0 if the whole story worked
    local S="$1" SHIMDIR="$2"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run"
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_instance" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/instance_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
    (cd "$S/run" && timeout 45 "$S/t.exe" > "$S/first.out" 2>&1; echo $? > "$S/first.rc") &
    local first=$!
    local waited=0
    until grep -q '^first ready' "$S/first.out" 2> /dev/null; do
        sleep 0.2; waited=$((waited + 1))
        [ "$waited" -gt 100 ] && { echo "the first instance never became ready"; cat "$S/first.out"; kill "$first" 2> /dev/null; return 1; }
    done
    local second
    second="$(cd "$S/run" && timeout 20 "$S/t.exe" alpha "b c" 2>&1)"; local src=$?
    echo "second launch: exit $src: $second"
    wait "$first"
    local frc; frc="$(cat "$S/first.rc" 2> /dev/null)"
    echo "first instance: exit $frc: $(tr -d '\r' < "$S/first.out" | tail -2 | tr '\n' ' ')"
    [ "$src" -eq 0 ] && printf '%s\n' "$second" | grep -q 'second launch handed over' && [ "$frc" = 0 ] && grep -q '^selftest ok' "$S/first.out"
}

run_case "${TMPDIR:-/tmp}/app-cling-instance" "" || { echo "instance test: FAILED"; exit 1; }
echo "instance test: ok"

C="${TMPDIR:-/tmp}/app-cling-instance-control"
rm -rf "$C" 2>/dev/null || true; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/win/instance.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "if (!tooLong && !message.empty()) deliver(in, message);"
assert s.count(key) == 1
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "/* delivery removed */"))
PYEOF
if run_case "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then echo "CONTROL DID NOT FAIL: the test does not need the delivery"; exit 1; fi
echo "control: without the delivery the test fails, as it must"
