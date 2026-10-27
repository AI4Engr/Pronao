#!/usr/bin/env bash
# The native layer is the only code no compiler checks, so it is kept small: what one platform's build compiles (the shared files
# plus shim/win or shim/posix) must stay under LIMIT lines of CODE (blank lines and comment-only lines are not counted; the physical
# line count is printed too). Fails if over.
#   scripts/shim_lines.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIMIT=1000
cd "$HERE/shim"
code() { cat "$@" | grep -c -v -E '^\s*(//.*)?$'; }
shared=$(code *.cpp *.h); sharedAll=$(cat *.cpp *.h | wc -l)
rc=0
for p in win posix; do
    own=$(code $p/*); ownAll=$(cat $p/* | wc -l)
    total=$((shared + own))
    printf '%-6s %4d lines of code (%d shared + %d in %s/); %d physical lines' "$p" "$total" "$shared" "$own" "$p" $((sharedAll + ownAll))
    if [ "$total" -ge "$LIMIT" ]; then printf '  OVER THE LIMIT of %d\n' "$LIMIT"; rc=1; else printf '  (limit %d)\n' "$LIMIT"; fi
done
exit $rc
