# Finds the tools the scripts need. Source it:  . "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# Sets CLING (the CeeLing compiler), CJSON (cJSON-cling), CXX (the C++ compiler) from the environment, else from the usual layout:
# CeeLing, cJSON-cling and pronao checked out side by side, CeeLing built in its own build/ folder.
PRONAO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRONAO_PARENT="$(dirname "$PRONAO_ROOT")"
pronao_fail() { echo "pronao: $*" >&2; exit 1; }

if [ -z "${CLING:-}" ]; then
    for candidate in "$PRONAO_PARENT/CeeLing/build/cling.exe" "$PRONAO_PARENT/CeeLing/build/cling"; do
        if [ -f "$candidate" ]; then CLING="$candidate"; break; fi
    done
fi
if [ -z "${CLING:-}" ] && command -v cling >/dev/null 2>&1; then CLING="$(command -v cling)"; fi
[ -n "${CLING:-}" ] && [ -f "$CLING" ] || pronao_fail "the CeeLing compiler was not found. Build CeeLing (dev.bat build / ./dev.sh build) in a folder next to pronao, or set CLING=<path to cling.exe>."

if [ -z "${CJSON:-}" ]; then
    for candidate in "$PRONAO_PARENT/cJSON-cling" "$PRONAO_PARENT/cjson-cling"; do
        if [ -f "$candidate/src/cjson_item.clg" ]; then CJSON="$candidate"; break; fi
    done
fi
[ -n "${CJSON:-}" ] && [ -f "$CJSON/src/cjson_item.clg" ] || pronao_fail "cJSON-cling was not found. Check it out next to pronao, or set CJSON=<its folder>."

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || pronao_fail "no C++ compiler ('$CXX'). On Windows install MinGW-w64 (MSYS2) and put its bin folder on PATH, or set CXX=<compiler>."
export CLING CJSON CXX
