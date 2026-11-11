#!/usr/bin/env bash
# Sanitizer runs for the headless build, in WSL (the MinGW toolchain on the dev machine has no sanitizer runtime).
#   tests/run_sanitizers.sh
# 1. ASan + UBSan over the CeeLing end-to-end tests (built WITHOUT --pool-shared: the pool would hide a use-after-free), the file
#    policy tests, the program lookup tests, the native process runner, the folder watcher and the queue stress test.
# 2. ThreadSanitizer over the queue stress test, the watcher AND the CeeLing end-to-end tests, which start real worker threads
#    (spawn), both without and with --pool-shared.
# 3. NEGATIVE CONTROLS: the same TSan runs against builds whose lock is removed (producer side of the queue; reply side). Each
#    MUST report a data race; if one does not, the TSan run it controls proves nothing and this script fails.
# Only the native layer, the test doubles and the CeeLing layer are covered; webview and the browser engine are not built here.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/scripts/env.sh"
# the sources are copied under BASE, which WSL must see: a Windows folder is reached as /mnt/<drive>/... (wslpath converts)
BASE="${BASE:-${TMPDIR:-/tmp}/pronao-san}"
WSL_DISTRO="${WSL_DISTRO:-}"             # empty: WSL's default distribution
rm -rf "$BASE"; mkdir -p "$BASE/shim" "$BASE/tests"
cp "$CJSON"/shim/cjson_num.h "$BASE/shim/"
# the sources the WSL half compiles, laid out as in the repository (the files include each other by relative path)
mkdir -p "$BASE/repo"
cp -r "$HERE/shim" "$HERE/tests" "$HERE/scripts" "$BASE/repo/"
rm -rf "$BASE/repo/shim_nolock"
bash "$HERE/scripts/pack_assets.sh" - "$BASE/repo"          # (no embedded web files in these builds)

# transpile everything twice: plain (work) and with the pooled allocator (work_pool)
MAINS="headless_main files_main programs_main store_main links_main instance_main placement_main menu_main notify_main layout_main system_main site_main"
for variant in work work_pool; do
    flags=""; [ "$variant" = "work_pool" ] && flags="--pool-shared"
    mkdir -p "$BASE/$variant"
    for f in cjson_item cjson_api cjson_parse cjson_print; do cp "$CJSON/src/$f.clg" "$BASE/$variant/"; done
    . "$HERE/scripts/modules.sh"
    for f in $APP_MODULES; do cp "$HERE/src/$f.clg" "$BASE/$variant/"; done
    for m in $MAINS; do cp "$HERE/tests/$m.clg" "$BASE/$variant/"; done
    # the modules are compiled one by one: what they need from each other's classes is worked out first (as scripts/build.sh does)
    bash "$HERE/scripts/pointer_demands.sh" "$CLING" "$BASE/$variant" "$flags" "$APP_MODULES" || { echo "POINTER DEMANDS FAILED: $variant"; exit 1; }
    flags="$flags --ptr-demands $BASE/$variant/demands.txt"
    for f in cjson_item cjson_api cjson_parse cjson_print $APP_MODULES $MAINS; do
        "$CLING" $flags --cpp "$BASE/$variant/$f.cpp" "$BASE/$variant/$f.clg" > "$BASE/$variant/$f.log" 2>&1 || { grep -v Detected "$BASE/$variant/$f.log"; echo "TRANSPILE FAILED: $variant/$f"; exit 1; }
        case " $MAINS " in *" $f "*) ;; *) "$CLING" $flags --cpp "$BASE/$variant/$f.h" "$BASE/$variant/$f.clg" >> "$BASE/$variant/$f.log" 2>&1 || { echo "TRANSPILE FAILED (header): $variant/$f"; exit 1; } ;; esac
    done
    {
        echo '#include <string>'
        echo 'std::string app_bridge_js() { return ""; }'
        echo 'std::string app_page_html() { return ""; }'
        echo 'std::string app_extra_js() { return ""; }'
    } > "$BASE/$variant/embedded.cpp"
done

# negative controls: the same tree with one lock removed
for c in nolock nolock_ret; do rm -rf "$BASE/$c"; cp -r "$BASE/repo" "$BASE/$c"; done
# producer side: the queue's admit (what the page's calls go through; shared by the real window and the test double) without its mutex
python3 - "$BASE/nolock/shim/queue.h" "$BASE/nolock_ret/tests/doubles/headless_shim.cpp" <<'PYEOF'
import sys
q, d = sys.argv[1], sys.argv[2]
s = open(q, encoding='utf-8', newline='').read()
a = s.index('int admit(')
b = s.index('std::lock_guard<std::mutex> lock(m_);', a)
s2 = s[:b] + '/* lock removed */' + s[b + len('std::lock_guard<std::mutex> lock(m_);'):]
assert s2 != s
open(q, 'w', encoding='utf-8', newline='').write(s2)
t = open(d, encoding='utf-8', newline='').read()
key = '{ std::lock_guard<std::mutex> lock(app->m); app->replies.push_back('
assert key in t
t2 = t.replace(key, '{ app->replies.push_back(')
open(d, 'w', encoding='utf-8', newline='').write(t2)
PYEOF
[ $? -eq 0 ] || { echo "NEGATIVE CONTROL SETUP FAILED"; exit 1; }
# control 4: the wait-any notification removed from the queue (every producer-side "something changed" bump): the ping-pong test, where the
# consumer is already asleep when a call arrives, must see the wakeup arrive late and fail
rm -rf "$BASE/nonotify"; cp -r "$BASE/repo" "$BASE/nonotify"
python3 - "$BASE/nonotify/shim/queue.h" <<'PYEOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
assert s.count("if (notify_) notify_->bump();") == 3
open(p, 'w', encoding='utf-8', newline='').write(s.replace("if (notify_) notify_->bump();", "/* notification removed */"))
PYEOF
[ $? -eq 0 ] || { echo "NEGATIVE CONTROL 4 SETUP FAILED"; exit 1; }
# control 3: the same transpiled program with the lock taken out of Channel::send in every generated file (the runtime template is
# emitted into each .cpp); the producer / consumer check in headless_main then races on the queue and TSan must say so
rm -rf "$BASE/work_nochannel"; cp -r "$BASE/work" "$BASE/work_nochannel"
python3 - "$BASE/work_nochannel" <<'PYEOF'
import glob, sys
lock = "std::lock_guard<std::mutex> lock(s_->m);"
tail = "if (s_->closed || s_->items.size() >= s_->capacity) return false;"
n = 0
for p in glob.glob(sys.argv[1] + "/*.cpp"):
    s = open(p, encoding='utf-8', newline='').read()
    i = s.find(tail)
    j = s.rfind(lock, 0, i) if i >= 0 else -1
    if j >= 0:
        s = s[:j] + "/* lock removed */" + s[j + len(lock):]
        open(p, 'w', encoding='utf-8', newline='').write(s)
        n += 1
assert n > 0, "no generated file carries the Channel template"
PYEOF
[ $? -eq 0 ] || { echo "NEGATIVE CONTROL 3 SETUP FAILED"; exit 1; }
cp "$HERE/tests/wsl_run.sh" "$HERE/scripts/modules.sh" "$BASE/"

if command -v cygpath >/dev/null 2>&1; then
    # Windows: the sanitizer half runs in WSL, which reaches the folder as /mnt/<drive>/... (asked of WSL itself, so any drive works)
    WSL_BASE="$(MSYS_NO_PATHCONV=1 wsl ${WSL_DISTRO:+-d "$WSL_DISTRO"} -- wslpath -a "$(cygpath -m "$BASE")" | tr -d '')"
    [ -n "$WSL_BASE" ] || { echo "WSL did not translate $BASE (is WSL installed? set WSL_DISTRO=<name> to choose a distribution)"; exit 1; }
    MSYS_NO_PATHCONV=1 wsl ${WSL_DISTRO:+-d "$WSL_DISTRO"} -- bash "$WSL_BASE/wsl_run.sh" "$WSL_BASE"
else
    bash "$BASE/wsl_run.sh" "$BASE"          # Linux: no WSL needed
fi
rc=$?
if [ $rc -eq 0 ]; then echo "sanitizers: ALL OK"; else echo "sanitizers: FAILED"; fi
exit $rc
