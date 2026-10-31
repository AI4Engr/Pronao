#!/usr/bin/env bash
# What the CeeLing modules need from each other, so that they can be compiled one by one and still fit together.
#   scripts/pointer_demands.sh <cling> <dir> "<cling flags>" "<modules>"        writes <dir>/demands.txt
#
# CeeLing chooses unique_ptr or shared_ptr for every field, container element and method result from how the class is USED, and it does that for one file at a time.
# A class that lives in one module and is used in another (a field handed to a closure, an element copied out of a vector) can need shared_ptr where the file that declares
# it, which does not see the use, would choose unique_ptr: the two generated files would not fit. So each module is compiled once to WRITE what it needs from the classes it
# includes (--ptr-demands-out), the needs of all modules are put together, and every module is then compiled READING them (--ptr-demands). A need can only make a member
# shared, and making one member shared can make another one needed, so this is repeated until the demands stop changing (a few rounds at most).
# Only the modules listed here write demands; a test program that includes them reads the result but adds nothing, so the library does not depend on who uses it.
# See smart_pointer_analyzer.h (SmartPointerAnalyzer::setDemands) in the compiler.
set -uo pipefail
CLING="$1"; DIR="$2"; FLAGS="$3"; MODULES="$4"
DEMANDS="$DIR/demands.txt"
: > "$DEMANDS"
for round in 1 2 3 4 5 6; do
    pids=()
    for m in $MODULES; do
        ( "$CLING" $FLAGS --ptr-demands "$DEMANDS" --ptr-demands-out "$DIR/$m.demands" --cpp "$DIR/$m.demands.cpp" "$DIR/$m.clg" > "$DIR/$m.demands.log" 2>&1 \
            || { grep -v Detected "$DIR/$m.demands.log"; exit 1; } ) &
        pids+=($!)
        while [ "$(jobs -rp | wc -l)" -ge "${JOBS:-8}" ]; do wait -n || { echo "pointer demands: a module did not compile" >&2; exit 1; }; done
    done
    for p in "${pids[@]}"; do wait "$p" || { echo "pointer demands: a module did not compile" >&2; exit 1; }; done
    cat "$DEMANDS" "$DIR"/*.demands 2> /dev/null | sort -u > "$DIR/demands.next"
    if cmp -s "$DIR/demands.next" "$DEMANDS"; then
        rm -f "$DIR/demands.next" "$DIR"/*.demands.cpp "$DIR"/*.demands.log
        exit 0
    fi
    mv "$DIR/demands.next" "$DEMANDS"
done
echo "pointer demands did not settle after 6 rounds (see $DEMANDS)" >&2
exit 1
