# Finds the tools the scripts need. Source it:  . "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# Sets CLING (the CeeLing compiler), CJSON (cJSON-cling), CXX (the C++ compiler) from the environment, else from the usual layout:
# CeeLing, cJSON-cling and app-cling checked out side by side, CeeLing built in its own build/ folder.
APP_CLING_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_CLING_PARENT="$(dirname "$APP_CLING_ROOT")"
app_cling_fail() { echo "app-cling: $*" >&2; exit 1; }

if [ -z "${CLING:-}" ]; then
    for candidate in "$APP_CLING_PARENT/CeeLing/build/cling.exe" "$APP_CLING_PARENT/CeeLing/build/cling"; do
        if [ -f "$candidate" ]; then CLING="$candidate"; break; fi
    done
fi
if [ -z "${CLING:-}" ] && command -v cling >/dev/null 2>&1; then CLING="$(command -v cling)"; fi
[ -n "${CLING:-}" ] && [ -f "$CLING" ] || app_cling_fail "the CeeLing compiler was not found. Build CeeLing (dev.bat build / ./dev.sh build) in a folder next to app-cling, or set CLING=<path to cling.exe>."

if [ -z "${CJSON:-}" ]; then
    for candidate in "$APP_CLING_PARENT/cJSON-cling" "$APP_CLING_PARENT/cjson-cling"; do
        if [ -f "$candidate/src/cjson_item.clg" ]; then CJSON="$candidate"; break; fi
    done
fi
[ -n "${CJSON:-}" ] && [ -f "$CJSON/src/cjson_item.clg" ] || app_cling_fail "cJSON-cling was not found. Check it out next to app-cling, or set CJSON=<its folder>."

CXX="${CXX:-g++}"
command -v "$CXX" >/dev/null 2>&1 || app_cling_fail "no C++ compiler ('$CXX'). On Windows install MinGW-w64 (MSYS2) and put its bin folder on PATH, or set CXX=<compiler>."
export CLING CJSON CXX
