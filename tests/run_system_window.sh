#!/usr/bin/env bash
# The colour scheme and files dragged onto the window, for real (Windows).
#
# THEME. The page says what its media query prefers-color-scheme answers; Windows is asked whether the title bar is dark (window_poke.exe darkbar). The test forces dark, light and
# "follow the system", and compares with the user's real setting (window_poke.exe appsdark). Then it plays a change of the system's scheme WITHOUT changing anything for other programs:
# the registry value Windows keeps is flipped, only this window is sent the notice Windows sends (WM_SETTINGCHANGE "ImmersiveColorSet"), and the value is put back (also if the test
# is stopped). The program must tell the page once per change and, if it follows the system, the title bar must follow.
#
# DROP. window_poke.exe drop starts a real OLE drag with the left button held down and moves the pointer over the window with SendInput, as the file manager does. The program must
# answer "copy", the page must hear enter / over / drop, the dropped file must be readable by the page (and a neighbour not), at most the number of files the application set must be
# taken, a drag of TEXT must be refused, a folder is skipped unless allowed, and after the application stopped accepting nothing arrives.
#
# Then CONTROLS: a shim in which the page's scheme is never set, one that never reports a change of the system's scheme, and one in which the window is no drop target.
# Not tested: dragging from a real file manager window (the drag is made by this test, with the same OLE calls), several monitors, a high display scaling, and the real system changing
# its scheme (that needs the user's settings changed for a moment, which a test should not do).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$HERE/tests/lib_stop.sh"
TITLE="system test"
KEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'

appsvalue() { MSYS_NO_PATHCONV=1 reg query "$KEY" /v AppsUseLightTheme 2> /dev/null | tr -d '\r' | awk '/AppsUseLightTheme/ { print $NF }'; }       # 0x0 or 0x1, or nothing
setapps() { MSYS_NO_PATHCONV=1 reg add "$KEY" /v AppsUseLightTheme /t REG_DWORD /d "$1" /f > /dev/null 2>&1; }
# the user's setting is written down before the test touches it, and put back at the end -- and, if an earlier run was cut short (the file is still there), first thing
SAVED="${TMPDIR:-/tmp}/app-cling-apps-theme.original"
if [ -f "$SAVED" ]; then setapps $(( $(cat "$SAVED") )); rm -f "$SAVED"; fi
ORIGINAL="$(appsvalue)"
[ -z "$ORIGINAL" ] || echo "$ORIGINAL" > "$SAVED"
restore() { if [ -n "$ORIGINAL" ]; then setapps $(( ORIGINAL )); fi; }
finish() { restore; rm -f "$SAVED"; }
trap finish EXIT

story() { # kind(theme|drop) dir shim-dir-or-empty -> 0 if the whole story worked
    local KIND="$1" S="$2" SHIMDIR="$3"
    rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/work" "$S/run" "$S/ins" "$S/dropped"
    g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 -lole32 -luuid -ladvapi32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; return 2; }
    SHIM="${SHIMDIR:-$HERE/shim}" ASSETS="$HERE/tests/site_system" WORK="$S/work" bash "$HERE/scripts/build.sh" "$HERE/tests/system_window.clg" "$S/t.exe" > "$S/build.log" 2>&1 || { cat "$S/build.log"; echo "BUILD FAILED"; return 2; }
    export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")" SYSTEM_TEST_DIR="$(cygpath -w "$S/ins")"
    [ "$KIND" = drop ] && export SYSTEM_TEST_DROP=on || export SYSTEM_TEST_DROP=off
    local P="$S/poke.exe" OUT="$S/out.txt" N=0
    (cd "$S/run" && timeout 150 "$S/t.exe" > "$OUT" 2>&1; echo $? > "$S/rc") &
    local pid=$!
    waitfor() { local n=0 limit=$(( ${2:-8} * 5 )); until grep -Eq "$1" "$OUT" 2> /dev/null; do sleep 0.2; n=$((n + 1)); [ "$n" -gt "$limit" ] && { echo "never saw: $1"; return 1; }; done; return 0; }
    fail() { echo "FAILED: $1"; stop_program "$S/t.exe"; wait "$pid" 2> /dev/null; return 1; }
    do_() { N=$((N + 1)); printf '%s %s' "$N" "$*" > "$S/ins/instructions.txt"; waitfor "^done $N " 10 || return 1; }
    count() { grep -c "^seen $1 " "$OUT" 2> /dev/null; }
    # one drag of files over the window; the engine sometimes answers "no" to the first drag after it started (it has not yet heard the pointer arrive), so a "no" is tried once more
    drag() { local r; r="$("$P" drop "$TITLE" "$@")"; if [ "$r" = none ] || [ "$r" = cancel ]; then sleep 1; r="$("$P" drop "$TITLE" "$@")"; fi; echo "$r"; }
    lastseen() { grep "^seen $1 " "$OUT" | tail -1 | cut -d' ' -f3-; }

    waitfor '^ready' 30 && "$P" wait "$TITLE" || fail "the program never became ready" || return 1

    if [ "$KIND" = theme ]; then
        local sys; sys="$("$P" appsdark)"; local sysname=light; [ "$sys" = 1 ] && sysname=dark
        waitfor '^seen scheme (dark|light)$' 20 || fail "the page never said what its scheme is" || return 1
        [ "$(lastseen scheme)" = "$sysname" ] || fail "an untouched window: the page says $(lastseen scheme), the user's setting is $sysname" || return 1
        echo "an untouched window follows the system ($sysname)"

        do_ theme dark || fail "theme dark was refused" || return 1
        waitfor '^seen scheme dark$' 10 || fail "forced dark: the page's scheme did not become dark" || return 1
        sleep 0.3; [ "$("$P" darkbar "$TITLE")" = 1 ] || fail "forced dark: the title bar is not dark" || return 1
        echo "forced dark: the page and the title bar are dark"
        do_ theme light || fail "theme light was refused" || return 1
        waitfor '^seen scheme light$' 10 || fail "forced light: the page's scheme did not become light" || return 1
        sleep 0.3; [ "$("$P" darkbar "$TITLE")" = 0 ] || fail "forced light: the title bar is not light" || return 1
        echo "forced light: the page and the title bar are light"
        do_ theme system || fail "theme system was refused" || return 1
        waitfor "^seen scheme $sysname\$" 10 || fail "follow the system: the page's scheme is not the system's ($sysname)" || return 1
        sleep 0.3; [ "$("$P" darkbar "$TITLE")" = "$sys" ] || fail "follow the system: the title bar does not match the system's" || return 1
        echo "follow the system: both match the setting ($sysname)"

        # the system changes (played): the registry value flips, this window alone is told
        [ -n "$ORIGINAL" ] || fail "this Windows has no AppsUseLightTheme value to play a change with" || return 1
        local before; before="$(count theme)"
        local flipped=$(( sys == 1 ? 1 : 0 ))                       # 1 = light apps; the system was dark (sys 1), so the value now is 0, and it flips to 1
        setapps "$flipped"
        "$P" themechange "$TITLE" || fail "could not send the notice" || return 1
        waitfor "^seen theme .*\"mode\":\"system\"" 10 && [ "$(count theme)" -gt "$before" ] || fail "following the system, the page was not told of the change" || return 1
        local newname=dark; [ "$flipped" = 1 ] && newname=light
        grep "^seen theme " "$OUT" | tail -1 | grep -q "\"system\":\"$newname\"" || fail "the event does not name the new scheme ($newname): $(lastseen theme)" || return 1
        sleep 0.3; [ "$("$P" darkbar "$TITLE")" = "$(( flipped == 1 ? 0 : 1 ))" ] || fail "the title bar did not follow the system" || return 1
        echo "the system changed to $newname: the page was told and the title bar followed"
        before="$(count theme)"
        "$P" themechange "$TITLE"; sleep 0.8
        [ "$(count theme)" = "$before" ] || fail "the same scheme reported again told the page again" || return 1
        echo "a second notice for the same scheme told nobody"

        # a forced scheme stays, but the application hears of the system: forced to the OPPOSITE of what the system goes back to, so that following it would show
        local forced=dark forcedbar=1; [ "$sys" = 1 ] && { forced=light; forcedbar=0; }
        do_ theme "$forced" > /dev/null; sleep 0.5; before="$(count theme)"
        restore; "$P" themechange "$TITLE"
        local waited=0
        until [ "$(count theme)" -gt "$before" ]; do
            sleep 0.2; waited=$((waited + 1))
            if [ "$waited" -gt 50 ]; then fail "forced $forced: the page was not told that the system went back to $sysname"; return 1; fi
        done
        lastseen theme | grep -q "\"mode\":\"$forced\",\"system\":\"$sysname\"" || fail "forced $forced: the event does not say so: $(lastseen theme)" || return 1
        sleep 0.3; [ "$("$P" darkbar "$TITLE")" = "$forcedbar" ] || fail "forced $forced: the title bar followed the system (it reads $("$P" darkbar "$TITLE"))" || return 1
        echo "forced $forced stays $forced when the system goes back to $sysname, and the page hears of it"
        restore
    else
        local D; D="$(cygpath -w "$S/dropped")"
        printf 'alpha' > "$S/dropped/a.txt"; printf 'bravo' > "$S/dropped/b.txt"; printf 'charlie' > "$S/dropped/c.txt"; printf 'delta' > "$S/dropped/d.txt"; printf 'echo' > "$S/dropped/e.txt"
        mkdir -p "$S/dropped/sub"; printf 'inside' > "$S/dropped/sub/inner.txt"
        local A="$D\\a.txt" B="$D\\b.txt" C="$D\\c.txt" DD="$D\\d.txt" E="$D\\e.txt" SUB="$D\\sub"
        [ "$(lastseen flag)" = true ] || waitfor '^seen flag true$' 10 || fail "the page was not told that drops are on (window.__cling_drop)" || return 1

        said="$(drag "$A")"; [ "$said" != none ] && [ "$said" != cancel ] || fail "the window did not accept a dropped file (the drag said: $said)" || return 1
        waitfor '^seen drop ' 10 || fail "the page never heard of the drop" || return 1
        [ "$(count enter)" -ge 1 ] || fail "the page did not hear the drag enter" || return 1
        [ "$(count over)" -ge 1 ] || fail "the page did not hear the drag over" || return 1
        lastseen drop | grep -q '"n":1,"skipped":0,"kinds":\["file"\],"texts":\["alpha"\]' || fail "the page did not get the file and its text: $(lastseen drop)" || return 1
        echo "a dropped file reached the page and could be read: $(lastseen drop)"

        local before said; before="$(count drop)"
        said="$(drag "$A" "$B" "$C")"; [ "$said" != none ] && [ "$said" != cancel ] || fail "three files were not accepted (the drag said: $said)" || return 1
        waitfor '^seen drop .*"n":3' 10 || fail "three files did not arrive as three" || return 1
        lastseen drop | grep -q '"texts":\["alpha","bravo","charlie"\]' || fail "three files: wrong contents or order: $(lastseen drop)" || return 1
        echo "three files arrived in order and could all be read"

        drag "$A" "$B" "$C" "$DD" "$E" > /dev/null
        waitfor '^seen drop .*"n":4,"skipped":1' 10 || fail "five files were not cut to the application's four: $(lastseen drop)" || return 1
        echo "five files: the first four taken, one skipped (the application's limit)"

        drag "$SUB" > /dev/null
        waitfor '^seen drop .*"kinds":\["folder"\]' 10 || fail "a dropped folder was not reported as one: $(lastseen drop)" || return 1
        echo "a dropped folder was reported as a folder"

        before="$(count drop)"; local enters; enters="$(count enter)"
        "$P" droptext "$TITLE" > /dev/null; sleep 1.5
        [ "$(count drop)" = "$before" ] || fail "a drag of text was reported as a drop of files" || return 1
        [ "$(count enter)" = "$enters" ] || fail "a drag of text was reported as a drag of files" || return 1
        echo "a drag of text is not a drop of files"

        do_ dropoff || fail "dropoff was refused" || return 1
        sleep 0.5; before="$(count drop)"
        drag "$A" > /dev/null; sleep 2
        [ "$(count drop)" = "$before" ] || fail "after the application stopped accepting, a drop still arrived" || return 1
        echo "after dropoff nothing arrives"
        do_ dropon || fail "dropon was refused" || return 1
        sleep 0.5
        drag "$B" > /dev/null
        waitfor '^seen drop .*"texts":\["bravo"\]' 10 || fail "accepting again did not work: $(lastseen drop)" || return 1
        echo "and accepting again works"
    fi
    do_ quit; waitfor '^closed$' 15 || fail "the program did not end" || return 1
    wait "$pid"
    [ "$(cat "$S/rc")" = 0 ] || { echo "the program ended with $(cat "$S/rc")"; return 1; }
    return 0
}

kind_of_story() { story "$1" "${TMPDIR:-/tmp}/app-cling-system-$1" ""; }
for kind in ${ONLY:-theme drop}; do                       # (ONLY=drop or ONLY=theme runs one story, without the controls)
    kind_of_story "$kind" || { echo "$kind test: FAILED"; stop_program "${TMPDIR:-/tmp}/app-cling-system-$kind/t.exe"; restore; exit 1; }
    echo "$kind test: ok"
done
restore
[ -z "${ONLY:-}" ] || exit 0

control() { # kind  name  file  old  new
    local C="${TMPDIR:-/tmp}/app-cling-system-control-${2// /_}"
    rm -rf "$C" 2>/dev/null || true; mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
    python3 - "$C/shim/$3" "$4" "$5" <<'PYEOF' || { echo "CONTROL SETUP FAILED: $2"; exit 1; }
import sys
p, old, new = sys.argv[1:4]
s = open(p, encoding='utf-8', newline='').read()
assert s.count(old) == 1, (old, s.count(old))
open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
PYEOF
    if story "$1" "$C/case" "$C/shim" > "$C/out.txt" 2>&1; then stop_program "$C/case/t.exe"; restore; echo "CONTROL DID NOT FAIL: $2"; exit 1; fi
    stop_program "$C/case/t.exe"; restore
    echo "control: $2: the test fails, as it must"
}
control theme "the page scheme is never set" win/theme.cpp 'if (SUCCEEDED(profile->put_PreferredColorScheme(scheme))) rc = 0;' 'rc = 0;'
control theme "a change of the system is never reported" app_shim.cpp '        themeSettingChanged(a, h);
        return result;' '        return result;'
control theme "a forced scheme is not given back after the notice" app_shim.cpp '    if (mode == 1 || mode == 2) app_theme_titlebar(h, mode == 2);
' ''
control drop "the paths never reach the program" win/drop.cpp 'app_push(handle_, std::string(), record, 100);' ';'
