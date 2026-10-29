#!/usr/bin/env bash
# NEGATIVE CONTROLS for the policy that lives in CeeLing. Each one removes a check from a copy of src/ and rebuilds a test program;
# the tests MUST then fail, otherwise they never proved that check. (The lock-removal controls for the threads are in
# run_sanitizers.sh.)
#   tests/negative_controls.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/app-cling-negative"
rm -rf "$W"; mkdir -p "$W"
"${CXX:-g++}" --std=c++17 -O1 "$HERE/tests/proc_helper.cpp" -o "$W/proc_helper.exe" || exit 1
export APP_PROC_ECHO="$W/proc_helper.exe"
# Speed: the controls are independent, so up to MAXPAR (default 10) run at the same time; their programs are built at -O0 and share compiled modules
# through OBJ_CACHE (a control that changes one file recompiles that file and the main, nothing else). Results are printed in order at the end.
export OPT="${OPT:--O0}" OBJ_CACHE="$W/cache"
MAXPAR="${MAXPAR:-10}"
bad=0
NAMES=()
control() { # queue a control; see control_run for the arguments
    local idx=${#NAMES[@]}
    NAMES+=("$1")
    while [ "$(jobs -rp | wc -l)" -ge "$MAXPAR" ]; do wait -n; done
    control_run "$@" > "$W/result.$idx" 2>&1 &
}
# a warm cache first: the modules every control shares
WORK="$W/warm" bash "$HERE/scripts/build.sh" "$HERE/tests/headless_main.clg" "$W/warm.exe" --headless > /dev/null 2>&1
control_run() { # name  file (relative to src/, or shim/...)  old  new  program  must-fail-substring
    local dir="$W/${1// /_}"
    mkdir -p "$dir/src" "$dir/work" "$dir/shim"
    cp "$HERE"/src/* "$dir/src/"
    cp "$HERE/examples/tauriport/tauriport.clg" "$dir/src/"      # (only compiled by the tauriport controls)
    cp -r "$HERE"/shim/. "$dir/shim/"
    mkdir -p "$dir/tests/doubles" && cp "$HERE"/tests/doubles/* "$dir/tests/doubles/"     # (the doubles include ../../shim/queue.h)
    local target="$dir/src/$2"
    case "$2" in shim/*) target="$dir/$2" ;; esac
    python3 - "$target" "$3" "$4" <<'PYEOF' || { echo "SETUP FAILED: $1"; bad=1; return; }
import sys
p, old, new = sys.argv[1:4]
s = open(p, encoding='utf-8', newline='').read()
assert s.count(old) == 1, (old, s.count(old))
open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
PYEOF
    local extra=""
    [ "$5" = "tauriport_main" ] && extra="$HERE/tests/support.clg $dir/src/tauriport.clg"
    local assets="-"
    [ "$5" = "site_main" ] && assets="$HERE/tests/site"
    ASSETS="$assets" EXTRA="$extra" SRC="$dir/src" SHIM="$dir/shim" DOUBLES="$dir/tests/doubles" WORK="$dir/work" bash "$HERE/scripts/build.sh" "$HERE/tests/$5.clg" "$dir/t.exe" --headless > "$dir/build.log" 2>&1 || { echo "BUILD FAILED: $1"; tail -5 "$dir/build.log"; bad=1; return; }
    # run the program only until the check that must fail has failed (or it ends): the rest of its output proves nothing more
    APP_SITE_DIR="$(cd "$HERE/tests/site" && (cygpath -m "$PWD" 2>/dev/null || pwd))" "$dir/t.exe" > "$dir/out.txt" 2>&1 &
    local pid=$! waited=0
    while kill -0 "$pid" 2> /dev/null && ! grep -q "^FAIL .*$6" "$dir/out.txt" && [ "$waited" -lt 600 ]; do sleep 0.2; waited=$((waited + 1)); done
    kill "$pid" 2> /dev/null; wait "$pid" 2> /dev/null
    local failed; failed=$(grep -c '^FAIL ' "$dir/out.txt")
    if [ "$failed" -gt 0 ] && grep -q "^FAIL .*$6" "$dir/out.txt"; then echo "ok: without '$1' $failed checks fail (including \"$6\")"; else echo "NEGATIVE CONTROL DID NOT FAIL: $1 ($failed failures)"; bad=1; fi
}
control "final-path check" files.clg 'if (!allowedFinal(o.fin)) {' 'if (false) {' files_main "through a link that leads out"
control "root-itself guard on remove" files.clg 'if (isRootItself(o.fin)) {' 'if (false) {' files_main "remove the allowed folder itself"
control "a file grant stays exact" files.clg '            return addGrant(o.fin, 1);' '            return addGrant(parentOf(o.fin), 0);' files_main "its neighbour is not"
control "permission re-check before a watch delivers" app.clg 'if (now.status != 0 || now.text != s.root) {' 'if (false) {' headless_main "withdrawing the permission ends the watch"
control "a dialog pick must be granted" app.clg 'g = gate.grantFile(f[3]);' 'g = new FsResult(0, f[3]);' headless_main "the picked file is now readable"
control "program name check" app.clg 'if (!programNameOk(name) || programs.indexOf(name) >= 0 || elements.size() > 64) {' 'if (programs.indexOf(name) >= 0 || elements.size() > 64) {' headless_main "a name with a space is refused"
control "watch limit" app.clg 'if (owner.watches.states.size() >= 32) {' 'if (false) {' headless_main "at most 32 folders"
control "dialog title check" app.clg 'if (!plainText(title, 200)) {' 'if (false) {' headless_main "an over-long title is refused"
control "a call is answered once" shim/queue.h '    bool answer(const std::string& id) { std::lock_guard<std::mutex> l(m_); return pending_.erase(id) > 0; }' '    bool answer(const std::string& id) { return true; }' headless_main "and nothing else arrives"
control "a window command list" app.clg '        if (!defaultRestricted) {' '        if (true) {' headless_main "a registered command that is not listed is forbidden"
control "dev server must be loopback" app.clg '    if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") {' '    if (false) {' headless_main "another host is refused"
control "pending calls are told apart by window" app.clg '            if (ids[i] == id && handles[i] == handle) {' '            if (ids[i] == id) {' headless_main "a record for window b settles b's pending call only"
control "cancel tags are told apart by window" app.clg '            if (tags[i] == tag && tagHandles[i] == handle) {' '            if (tags[i] == tag) {' headless_main "and leaves a's call with the same tag and id running"
control "a window own command list" app.clg '        if (own) {' '        if (false) {' headless_main "it then calls what its own list allows"
control "request size limit" shim/queue.h '        if (static_cast<int>(request.size()) > max_request) return 1;' '' headless_main "a call over the size limit is refused"
control "queue length limit" shim/queue.h '        if (static_cast<int>(q_.size()) >= max_queued) return 2;' '        if (false) return 2;' headless_main "with 3 waiting"
control "ids a page cannot have" shim/queue.h '        if (id.empty() || id.size() > 64) return 3;' '' headless_main "an id the page could not have been given"
control "environment is withheld" app.clg 'splitOn("PATH,SystemRoot,' 'splitOn("APP_CLING_SECRET,PATH,SystemRoot,' headless_main "a variable of this process is not passed"
control "asset names are vetted" assets.clg '        if (part.length() == 0 || part == "." || part == "..") {' '        if (false) {' site_main "names it may not"
control "the origin boundary" shim/assets.h "(url.size() > origin_.size() && url[origin_.size()] != '/' && url[origin_.size()] != '?' && url[origin_.size()] != '#')" "false" site_main "another origin is left to the network"
control "unknown addresses are 404" shim/assets.h '        if (it == items_.end()) { status = 404;' '        if (it == items_.end()) { return false; status = 404;' site_main "an unknown address is a 404"
if command -v lore > /dev/null 2>&1 || [ -n "${LORE_CLI_PATH:-}" ]; then
    control "recursive remove checks the folder first" files.clg '            if (isRootItself(here.text)) {' '            if (false) {' tauriport_main "the opened folder itself is never removed"
    control "paths that could be options are refused" tauriport.clg '    if (path.length() == 0 || path.length() > 1000 || !plainText(path, 1000) || path[0] == '"'"'-'"'"'' '    if (path.length() == 0 || path.length() > 1000 || !plainText(path, 1000)' tauriport_main "a path that could be read as an option is refused"
fi
wait
for ((k = 0; k < ${#NAMES[@]}; k++)); do
    cat "$W/result.$k"
    grep -q '^ok: ' "$W/result.$k" || bad=1
done
[ "$bad" -eq 0 ] && echo "negative controls: ALL OK" || echo "negative controls: FAILED"
exit $bad
