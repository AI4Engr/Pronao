#!/usr/bin/env bash
# Linux counterpart of close_dialogs.ps1: closes windows of the program under test by exact title the way a user pressing the X would (tests/xclose.c sends
# WM_DELETE_WINDOW). For a Yes/No box the X counts as No, as in the program. Needs DISPLAY (X11, or WSLg with GDK_BACKEND=x11), gcc and the X11 headers.
#   close_dialogs.sh "<title one>" "<title two>" ...      exit 0 when every title was found and closed within 25 seconds
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="${TMPDIR:-/tmp}/pronao-xclose"
gcc -O1 -o "$TOOL" "$HERE/xclose.c" -lX11 || { echo "close_dialogs: cannot build xclose (libx11-dev?)"; exit 1; }
deadline=$((SECONDS + 25)); declare -A closed
while [ $SECONDS -lt $deadline ] && [ ${#closed[@]} -lt $# ]; do
    for t in "$@"; do
        [ -n "${closed[$t]:-}" ] && continue
        if xdotool search --onlyvisible --name "^${t}\$" > /dev/null 2>&1; then
            sleep 0.5                                                    # let it finish appearing
            "$TOOL" "$t" > /dev/null && closed[$t]=1
        fi
    done
    sleep 0.1
done
[ ${#closed[@]} -eq $# ] && exit 0
echo "close_dialogs: only closed ${#closed[@]} of $#"; exit 1
