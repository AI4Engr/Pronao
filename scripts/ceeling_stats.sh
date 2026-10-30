#!/usr/bin/env bash
# What CeeLing did for app-cling, in numbers: counts in src/*.clg and in the C++ the compiler generated from it.
#   scripts/ceeling_stats.sh [work dir of a build]       (default: the one tests/run_headless.sh leaves behind)
# Used for docs/ceeling-in-app-cling.md; run it again to refresh the numbers there.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${1:-${TMPDIR:-/tmp}/app-cling-headless/work_headless_main}"
[ -f "$W/app.cpp" ] || { echo "no generated code in $W (run tests/run_headless.sh first, or pass a build's work directory)"; exit 1; }

# the source without comment lines and blank lines (a word in a comment is not code)
SRC_CODE="$(mktemp)"
trap 'rm -f "$SRC_CODE"' EXIT
cat "$HERE"/src/*.clg | grep -vE '^[[:space:]]*(//.*)?$' > "$SRC_CODE"

count() { grep -oE "$1" "${@:2}" 2> /dev/null | wc -l; }

echo "CeeLing source (src/*.clg): $(wc -l < "$SRC_CODE") lines of code (comments and blank lines left out)"
echo "  ext_fn declarations (the trusted native surface): $(grep -c '^ext_fn' "$SRC_CODE")"
echo "  closures: $(count 'closure [A-Z][A-Za-z]* \[' "$SRC_CODE") (spawn jobs among them: $(count 'spawn\(closure Job' "$SRC_CODE"))"
echo "  manual-memory words in the source (delete, .get(), reinterpret, std::move, unique_ptr, shared_ptr, weak_ptr): $(grep -ciE 'delete |\.get\(\)|reinterpret|std::move|unique_ptr|shared_ptr|weak_ptr' "$SRC_CODE")"

cd "$W"
GEN="app.cpp files.cpp core.cpp text.cpp assets.cpp"
echo "Generated C++ from those files ($W):"
echo "  std::unique_ptr: $(count 'std::unique_ptr' $GEN)   std::shared_ptr: $(count 'std::shared_ptr' $GEN)   std::weak_ptr: $(count 'std::weak_ptr' $GEN)   std::move: $(count 'std::move' $GEN)"
echo "  bounds checks inserted (cling::at): $(count 'cling::at\(' $GEN)"
echo "  null checks inserted (cling::nn):   $(count 'cling::nn\(' $GEN)"
