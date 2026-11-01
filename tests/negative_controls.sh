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
crlf = '\r\n' in s                                  # (a checkout may have CRLF line ends: compare with LF, write back the original ends)
s = s.replace('\r\n', '\n')
assert s.count(old) == 1, (old, s.count(old))
s = s.replace(old, new)
open(p, 'w', encoding='utf-8', newline='').write(s.replace('\n', '\r\n') if crlf else s)
PYEOF
    local extra=""
    [ "$5" = "tauriport_main" ] && extra="$HERE/tests/support.clg $dir/src/tauriport.clg"
    local assets="-"
    [ "$5" = "site_main" ] && assets="$HERE/tests/site"
    ASSETS="$assets" EXTRA="$extra" SRC="$dir/src" SHIM="$dir/shim" DOUBLES="$dir/tests/doubles" WORK="$dir/work" bash "$HERE/scripts/build.sh" "$HERE/tests/$5.clg" "$dir/t.exe" --headless > "$dir/build.log" 2>&1 || { echo "BUILD FAILED: $1"; tail -5 "$dir/build.log"; bad=1; return; }
    # run the program only until the check that must fail has failed (or it ends): the rest of its output proves nothing more
    APP_SITE_DIR="$(cd "$HERE/tests/site" && (cygpath -m "$PWD" 2>/dev/null || pwd))" "$dir/t.exe" > "$dir/out.txt" 2>&1 &
    local pid=$! waited=0
    while kill -0 "$pid" 2> /dev/null && ! grep -q "^FAIL .*$6" "$dir/out.txt" 2> /dev/null && [ "$waited" -lt 600 ]; do sleep 0.2; waited=$((waited + 1)); done
    kill "$pid" 2> /dev/null; wait "$pid" 2> /dev/null
    local failed; failed=$(grep -c '^FAIL ' "$dir/out.txt")
    if [ "$failed" -gt 0 ] && grep -q "^FAIL .*$6" "$dir/out.txt"; then echo "ok: without '$1' $failed checks fail (including \"$6\")"; else echo "NEGATIVE CONTROL DID NOT FAIL: $1 ($failed failures)"; bad=1; fi
}
control "final-path check" files.clg 'if (!allowedFinal(o.fin)) {' 'if (false) {' files_main "through a link that leads out"
control "root-itself guard on remove" files.clg 'if (isRootItself(o.fin)) {' 'if (false) {' files_main "remove the allowed folder itself"
control "a file grant stays exact" files.clg '            return addGrant(o.fin, 1);' '            return addGrant(parentOf(o.fin), 0);' files_main "its neighbour is not"
control "permission re-check before a watch delivers" watches.clg 'if (now.status != 0 || now.text != s.root) {' 'if (false) {' headless_main "withdrawing the permission ends the watch"
control "a dialog pick must be granted" app.clg 'g = gate.grantFile(f[3]);' 'g = new FsResult(0, f[3]);' headless_main "the picked file is now readable"
control "program name check" app.clg 'if (!programNameOk(name) || programs.indexOf(name) >= 0 || elements.size() > 64) {' 'if (programs.indexOf(name) >= 0 || elements.size() > 64) {' headless_main "a name with a space is refused"
control "watch limit" app.clg 'if (owner.watches.states.size() >= 32) {' 'if (false) {' headless_main "at most 32 folders"
control "dialog title check" dialogs.clg 'if (!plainText(title, 200)) {' 'if (false) {' headless_main "an over-long title is refused"
control "a call is answered once" shim/queue.h '    bool answer(const std::string& id) { std::lock_guard<std::mutex> l(m_); return pending_.erase(id) > 0; }' '    bool answer(const std::string& id) { return true; }' headless_main "and nothing else arrives"
control "a window command list" dispatch.clg '        if (!defaultRestricted) {' '        if (true) {' headless_main "a registered command that is not listed is forbidden"
control "dev server must be loopback" windows.clg '    if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") {' '    if (false) {' headless_main "another host is refused"
control "pending calls are told apart by window" later.clg '            if (ids[i] == id && handles[i] == handle) {' '            if (ids[i] == id) {' headless_main "a record for window b settles b's pending call only"
control "cancel tags are told apart by window" dispatch.clg '            if (tags[i] == tag && tagHandles[i] == handle) {' '            if (tags[i] == tag) {' headless_main "and leaves a's call with the same tag and id running"
control "a window own command list" dispatch.clg '        if (own) {' '        if (false) {' headless_main "it then calls what its own list allows"
control "a run is vetted before the native runner" programs.clg '    if (!toolRunOk(run)) {' '    if (false) {' headless_main "runTool refuses what is not fit, without starting anything"
control "a relative program is not fit to run" programs.clg '    if (!exeAbsolute || !cwdAbsolute || !plainText(exe, 32000) || !plainText(cwd, 32000)) {' '    if (!plainText(exe, 32000) || !plainText(cwd, 32000)) {' headless_main "a relative program is not"
control "a site origin is vetted" assets.clg ' || endsWithText(origin, "/")) {' ') {' site_main "an origin with a trailing slash is refused"
control "request size limit" shim/queue.h '        if (static_cast<int>(request.size()) > max_request) return 1;' '' headless_main "a call over the size limit is refused"
control "queue length limit" shim/queue.h '        if (static_cast<int>(q_.size()) >= max_queued) return 2;' '        if (false) return 2;' headless_main "with 3 waiting"
control "ids a page cannot have" shim/queue.h '        if (id.empty() || id.size() > 64) return 3;' '' headless_main "an id the page could not have been given"
control "environment is withheld" programs.clg 'splitOn("PATH,SystemRoot,' 'splitOn("APP_CLING_SECRET,PATH,SystemRoot,' headless_main "a variable of this process is not passed"
control "asset names are vetted" assets.clg '        if (part.length() == 0 || part == "." || part == "..") {' '        if (false) {' site_main "names it may not"
control "the origin boundary" shim/assets.h "(url.size() > origin_.size() && url[origin_.size()] != '/' && url[origin_.size()] != '?' && url[origin_.size()] != '#')" "false" site_main "another origin is left to the network"
control "unknown addresses are 404" shim/assets.h '        if (it == items_.end()) { status = 404;' '        if (it == items_.end()) { return false; status = 404;' site_main "an unknown address is a 404"
control "a window in full screen is not moved" windows.clg '        if (now.size() != 5 || (now[4] / 4) % 2 == 1) {' '        if (now.size() != 5) {' layout_main "a window in full screen is not moved"
control "a place is fitted to the monitors" windows.clg '        std::vector<int> fit = fitToMonitors(x, y, width, height, monitorList());' '        std::vector<int> fit; fit.push_back(x); fit.push_back(y); fit.push_back(width); fit.push_back(height);' layout_main "and fits one that does not: moved in, made smaller"
control "an edge must be one of eight" windows.clg '        if (code == 0) {' '        if (false) {' layout_main "an edge that is not one is refused"
control "full screen is offered only when asked for" app.clg '        if (fullscreen) {' '        if (true) {' layout_main "full screen and always on top are not offered unless asked for"
control "the position a page asks for is sane" app.clg '            if (x.valuedouble < -32000.0 || x.valuedouble > 32000.0 || y.valuedouble < -32000.0 || y.valuedouble > 32000.0) {' '            if (false) {' layout_main "and sane ones"
control "a modal window disables its opener" app.clg '                child.setModalFor(opener);' '' layout_main "the new window is modal for the page that opened it"
control "notifications are rate limited" notify.clg '        if (now - lastMs < gapMs) {' '        if (false) {' notify_main "a second one at once is held back"
control "a notification title is limited" notify.clg '        if (title.length() == 0 || !cleanText(title, 63, false)) {' '        if (title.length() == 0) {' notify_main "a title is 1 to 63 bytes on one line"
control "a notification has no control characters" notify.clg '            if ((c >= 0 && c < 32 && !(lines && c == 10)) || c == 127) {' '            if (false) {' notify_main "no control characters"
control "a notification id is a name" notify.clg '        if (id.length() > 0 && !menuId(id)) {
            return "invalid_argument: `id`' '        if (false) {
            return "invalid_argument: `id`' notify_main "the id is a name"
control "a click on a window is not a notification click" app.clg '        if (trayIndex(from) < 0 || (id.length() > 0 && !menuId(id))) {' '        if (id.length() > 0 && !menuId(id)) {' notify_main "a notify record on a window is ignored"
control "only the application can set the gap" app.clg '        if (ms < 200 || ms > 600000) {' '        if (false) {' notify_main "the gap can be set, within limits, only by the application"
control "a missing runtime ends the program" app.clg '        if (app_runtime_version(version) != 1) {' '        if (true) {' links_main "without the runtime the program is told to end"
control "No does not open the download page" app.clg '        if (app_message_box(name, text, 1) == 1) {' '        if (true) {' links_main "No opens nothing"
control "a page can ask only for a registered menu" app.clg '            int index = book.indexOf(name);' '            int index = 0;' menu_main "a name that was not registered is refused and nothing is shown"
control "popup names are names" app.clg '        if (!menuId(name) || popups.indexOf(name) >= 0 || !popup.complete() || popups.names.size() >= 16) {' '        if (popups.indexOf(name) >= 0 || !popup.complete() || popups.names.size() >= 16) {' menu_main "a pop-up menu needs a good name"
control "a greyed item of the tray menu cannot be chosen" app.clg '            if (mi < 0 || (tm.flags[mi] / 2) % 2 == 1) {' '            if (mi < 0) {' menu_main "a greyed item of the tray menu does nothing"
control "a tray record counts only on a tray" app.clg '            if (trayIndex(from) >= 0 && (f[1] == "click" || f[1] == "double")) {' '            if (f[1] == "click" || f[1] == "double") {' menu_main "a tray record on a window is ignored"
control "the close button hides a window that asked for it" app.clg '        if (st.hides) {' '        if (false) {' menu_main "the close button hides the window and nothing is closed"
control "a tray tooltip has a limit" app.clg '        if (!plainText(tooltip, 127) || tooltip.length() == 0 || !popup.complete() || trays.size() >= 4) {' '        if (tooltip.length() == 0 || !popup.complete() || trays.size() >= 4) {' menu_main "a tooltip is at most 127 bytes"
control "a program with a tray ends on quit" app.clg '                    if (windows.count() == 0 && (!stayWithoutWindows || quitCalled)) {' '                    if (windows.count() == 0) {' menu_main "with no window the loop goes on"
control "a click must name an item the menu has" app.clg '        if (w == nullptr || !(w.menuItemEnabled(id) || popups.hasItem(id))) {' '        if (w == nullptr) {' menu_main "an id the menu does not have does nothing"
control "a greyed item cannot be clicked" windows.clg '        return (kind == "i" || kind == "c") && (menu.flags[i] / 2) % 2 == 0;' '        return kind == "i" || kind == "c";' menu_main "a greyed item does nothing"
control "a menu id is used once" menu.clg '        if (known(id)) {' '        if (false) {' menu_main "the same id again is refused and nothing is added"
control "a shortcut that types into a page needs a modifier" menu.clg '    if (!functionKey && !commandKey) {' '    if (false) {' menu_main "a key that types into a page needs Ctrl or Alt"
control "a label has at most one mnemonic" menu.clg '    return marks <= 1;' '    return true;' menu_main "menu labels"
control "submenus are nested at most four deep" menu.clg '        if (depth >= 4) {' '        if (false) {' menu_main "not five"
control "a place is moved inside a monitor" placement.clg '        nx = larger(monitors[best * 4], smaller(x, monitors[best * 4 + 2] - w));' '        nx = x;' placement_main "one that hangs over the right edge is moved in"
control "a window is no bigger than the biggest monitor" placement.clg '    int w = larger(200, smaller(width, maxW));' '    int w = larger(200, width);' placement_main "one that is too big is made the size of the monitor"
control "a place must be sane numbers" placement.clg '        return x >= -32000 && x <= 32000 && y >= -32000 && y <= 32000 && w >= 1 && w <= 32000 && h >= 1 && h <= 32000;' '        return true;' placement_main "numbers that make no sense are refused"
control "a placement record must be numbers" app.clg '            if (numbers && moved != nullptr && moved.memoryKey.length() > 0) {' '            if (moved != nullptr && moved.memoryKey.length() > 0) {' placement_main "a bad record changes nothing"
control "tracking starts with the remembered window" app.clg '            app_window_track(h, 1);' '' placement_main "the window is tracked"
control "a later launch passes at most 64 arguments" app.clg '        while (i < f.size() && kept < 64) {' '        while (i < f.size()) {' instance_main "at most 64 arguments are passed on"
control "arguments of a later launch must be text" app.clg '            if (plainText(f[i], 4096)) {' '            if (true) {' instance_main "an argument or folder that is not text is left out"
control "a later launch brings the window to the front" app.clg '        app_window_state(from, 3);' '' instance_main "the window is brought to the front"
control "only the instance listens" app.clg '        if (st == 0 && first == 1) {' '        if (st == 0) {' instance_main "another one is running and was told"
control "an instance name must be plain" app.clg '        if (!plainName(appName)) {' '        if (false) {' instance_main "a name that is not plain claims nothing"
control "an address must be of an allowed scheme" links.clg '        if (scheme.length() == 0 || !schemeAllowed(scheme)) {' '        if (scheme.length() == 0) {' links_main "a scheme that is not allowed"
control "a host with a user name is refused" links.clg '        if (!hostChar(rest[end])) {' '        if (false) {' links_main "a user name in front of the host is refused"
control "a program type is never opened" links.clg '        if (ext.length() == 0 || dangerousExtension(ext)) {' '        if (ext.length() == 0) {' links_main "a program type is refused even if the list was filled by another way"
control "a file that cannot be resolved is not opened" app.clg '            fsCheck(r.status, "cannot open " + path);' '' links_main "a folder is not opened as a file"
control "a click is told from a script" app.clg '        if (links.browserLinks && f.size() >= 4 && f[3] == "1") {' '        if (links.browserLinks) {' links_main "a script that sets location is not a click"
control "opens are rate limited" links.clg '        if (now - lastMs < 300.0) {' '        if (false) {' links_main "one open per 300 ms"
control "the config folder is not offered to the page" app.clg '        if (kind == "config") {' '        if (false) {' store_main "allowAppFolder refuses the config folder"
control "a setting is checked against its range" settings.clg '        if (v < lows[i] || v > highs[i]) {' '        if (false) {' store_main "an int outside its range is refused"
control "a setting string has a limit" settings.clg '            if (s.length() > limits[i]) {' '            if (false) {' store_main "a string over its limit is refused"
control "a setting is saved by swapping the file in" settings.clg '        return gate.replace(tmp, filePath());' '        return 0;' store_main "another run reads what was saved"
control "replace stays inside one folder" files.clg '        if (folder != parentOf(b.text)) {' '        if (false) {' files_main "replace into another folder is refused"
if command -v lore > /dev/null 2>&1 || [ -n "${LORE_CLI_PATH:-}" ]; then
    control "recursive remove checks the folder first" files.clg '            if (isRootItself(here.text)) {' '            if (false) {' tauriport_main "the opened folder itself is never removed"
    control "paths that could be options are refused" tauriport.clg '    if (path.length() == 0 || path.length() > 1000 || !plainText(path, 1000) || path[0] == '"'"'-'"'"'' '    if (path.length() == 0 || path.length() > 1000 || !plainText(path, 1000)' tauriport_main "a path that could be read as an option is refused"
fi
control "a dropped file is granted to the policy" drops.clg '                g = gate.grantFile(path);' '                g = new FsResult(0, path);' system_main "and the page can read it"
control "a dropped folder needs the application yes" drops.clg '                if (folders) {' '                if (true) {' system_main "only what exists"
control "a drop takes at most the number set" drops.clg 'if (taken >= maxFiles || !plainText(path, 32000)) {' 'if (!plainText(path, 32000)) {' system_main "at most the number"
control "a dropped name must exist" drops.clg '            } else if (found == 1 || found == 5) {' '            } else {' system_main "only what exists"
control "a colour mode must be one of three" theme.clg '        if (code < 0) {' '        if (false) {' system_main "a word that is not a mode"
wait
for ((k = 0; k < ${#NAMES[@]}; k++)); do
    cat "$W/result.$k"
    grep -q '^ok: ' "$W/result.$k" || bad=1
done
[ "$bad" -eq 0 ] && echo "negative controls: ALL OK" || echo "negative controls: FAILED"
exit $bad
