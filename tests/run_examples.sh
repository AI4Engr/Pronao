#!/usr/bin/env bash
# Builds and checks every example in examples/ that has a headless check (tests/examples/<name>_check.clg):
#   1. the real program (examples/<name>/main.clg) must compile and link against the test doubles;
#   2. the check program drives the example's commands through the real dispatcher and must print PASS lines only.
#   bash tests/run_examples.sh [name ...]        (no names: every example)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/pronao-examples"
rm -rf "$W"; mkdir -p "$W"
export OBJ_CACHE="$W/cache"
CXX="${CXX:-g++}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) PLAT=win ;; *) PLAT=posix ;; esac
"$CXX" --std=c++17 -O1 "$HERE/tests/proc_helper.cpp" -o "$W/proc_helper.exe" || { echo "HELPER BUILD FAILED"; exit 1; }
export APP_PROC_ECHO="$W/proc_helper.exe"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
    for c in "$HERE"/tests/examples/*_check.clg; do
        [ -f "$c" ] && names+=("$(basename "$c" _check.clg)")
    done
fi

OK=1
total=0
for name in "${names[@]}"; do
    d="$HERE/examples/$name"
    [ -d "$d" ] || { echo "$name: no such example"; OK=0; continue; }
    mod=""; [ -f "$d/commands.clg" ] && mod="$d/commands.clg"
    assets=""; [ -d "$d/dist" ] && assets="$d/dist"
    page=""; [ -f "$d/index.html" ] && page="$d/index.html"

    # 1. the real program compiles
    if ! ( PAGE="$page" ASSETS="$assets" EXTRA="$mod" WORK="$W/$name.main.work" OPT=-O0 \
           bash "$HERE/scripts/build.sh" "$d/main.clg" "$W/$name.main.exe" --headless > "$W/$name.main.log" 2>&1 ); then
        echo "FAIL $name: main.clg does not build"; tail -5 "$W/$name.main.log"; OK=0; continue
    fi

    # 2. the check
    if ! ( ASSETS="$assets" EXTRA="$HERE/tests/support.clg $mod" WORK="$W/$name.check.work" OPT=-O0 \
           bash "$HERE/scripts/build.sh" "$HERE/tests/examples/${name}_check.clg" "$W/$name.check.exe" --headless > "$W/$name.check.log" 2>&1 ); then
        echo "FAIL $name: the check does not build"; tail -5 "$W/$name.check.log"; OK=0; continue
    fi
    mkdir -p "$W/$name.run"
    site_dir=""; [ -n "$assets" ] && site_dir="$(cd "$assets" && (cygpath -m "$PWD" 2>/dev/null || pwd))"
    out="$(cd "$W/$name.run" && APP_SITE_DIR="$site_dir" "$W/$name.check.exe" 2>&1)"; rc=$?
    pass=$(printf '%s\n' "$out" | grep -c '^PASS ')
    fail=$(printf '%s\n' "$out" | grep -c '^FAIL ')
    total=$((total + pass))
    if [ $rc -ne 0 ] || [ "$fail" -ne 0 ] || [ "$pass" -eq 0 ]; then
        echo "FAIL $name: $pass passed, $fail failed (exit $rc)"; printf '%s\n' "$out" | grep -v '^PASS ' | head -8; OK=0
    else
        echo "ok   $name: $pass checks"
    fi
done
echo "$total example checks passed"
[ "$OK" -eq 1 ]
