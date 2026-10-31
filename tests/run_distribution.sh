#!/usr/bin/env bash
# Giving the program to someone else (Windows): a release build must be a windowed program (no black console window on double-click), carry its version and icon,
# check that the window's runtime is installed before it tries to open a window, and say so in a message box when it is not.
#   1. RELEASE=1 links with -mwindows (the PE subsystem is "Windows GUI"); GUI=0 keeps the console (the control)
#   2. the version resource and the icon are in the exe (what Explorer's Properties > Details shows)
#   3. the program starts, opens its window, and ends when the window is closed
#   4. a computer WITHOUT the runtime (a doctored copy of the shim that reports it missing): a message box, no window, exit code 3
#   5. bad values for the version resource are refused by the build
# Not tested: a computer that really has no runtime, and what the message box looks like (this one has the runtime, and the test answers the box from outside).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="${TMPDIR:-/tmp}/app-cling-dist"
rm -rf "$S" 2>/dev/null || true; mkdir -p "$S/run"
g++ --std=c++17 -O1 "$HERE/tests/window_poke.cpp" -o "$S/poke.exe" -luser32 -lgdi32 -lshell32 > "$S/poke.log" 2>&1 || { cat "$S/poke.log"; echo "POKE BUILD FAILED"; exit 1; }
P="$S/poke.exe"
python3 "$HERE/scripts/make_icon.py" "$S/app.ico" dist-test ff0000 || exit 1

build() { # out.exe [env assignments...] -> builds tests/runtime_window.clg
    local out="$1"; shift
    env "$@" ASSETS="$HERE/tests/site_placement" WORK="$S/work-$(basename "$out" .exe)" bash "$HERE/scripts/build.sh" "$HERE/tests/runtime_window.clg" "$out" > "$out.log" 2>&1
}
subsystem() { objdump -x "$1" 2> /dev/null | grep -i "^Subsystem" | head -1 | awk '{print $NF}' | tr -d '()'; }

# 1: windowed or console
build "$S/gui.exe" RELEASE=1 STRIP=1 ICON="$S/app.ico" APP_VERSION=2.3.4 APP_NAME="Runtime Test" APP_COMPANY="Example Co" APP_DESCRIPTION="A test program" APP_COPYRIGHT="(c) Example Co" || { cat "$S/gui.exe.log"; echo "RELEASE BUILD FAILED"; exit 1; }
build "$S/console.exe" RELEASE=1 GUI=0 || { cat "$S/console.exe.log"; echo "CONSOLE BUILD FAILED"; exit 1; }
build "$S/debug.exe" || { cat "$S/debug.exe.log"; echo "DEBUG BUILD FAILED"; exit 1; }
echo "subsystems: release $(subsystem "$S/gui.exe"), release with GUI=0 $(subsystem "$S/console.exe"), plain build $(subsystem "$S/debug.exe")"
[ "$(subsystem "$S/gui.exe")" = "GUI" ] || { echo "a release build is not a windowed program"; exit 1; }
[ "$(subsystem "$S/console.exe")" = "CUI" ] && [ "$(subsystem "$S/debug.exe")" = "CUI" ] || { echo "GUI=0 and the plain build must keep the console"; exit 1; }

# 2: the version resource and the icon
info="$(powershell -NoProfile -Command "\$v = (Get-Item '$(cygpath -w "$S/gui.exe")').VersionInfo; \"\$(\$v.FileVersion)|\$(\$v.ProductVersion)|\$(\$v.CompanyName)|\$(\$v.ProductName)|\$(\$v.FileDescription)|\$(\$v.LegalCopyright)|\$(\$v.OriginalFilename)\"" | tr -d '\r')"
echo "version resource: $info"
[ "$info" = "2.3.4|2.3.4|Example Co|Runtime Test|A test program|(c) Example Co|gui.exe" ] || { echo "the version resource is not what was asked for"; exit 1; }
[ "$("$P" exeicon "$(cygpath -w "$S/gui.exe")")" = "ff0000 255" ] || { echo "the icon is not in the exe"; exit 1; }
v2="$(powershell -NoProfile -Command "(Get-Item '$(cygpath -w "$S/debug.exe")').VersionInfo.FileVersion" | tr -d '\r')"
[ -z "$v2" ] || { echo "a build without APP_VERSION has a version resource: $v2"; exit 1; }

# 3: the windowed program starts, opens its window, ends when it is closed
export WEBVIEW2_USER_DATA_FOLDER="$(cygpath -w "$S/profile-$$")"
(cd "$S/run" && timeout 60 "$S/gui.exe"; echo $? > "$S/gui.rc") &
pid=$!
"$P" wait "runtime window" || { kill "$pid" 2> /dev/null; echo "the windowed program never opened its window"; exit 1; }
"$P" close "runtime window"
wait "$pid"
[ "$(cat "$S/gui.rc")" = 0 ] || { echo "the windowed program ended with $(cat "$S/gui.rc")"; exit 1; }
echo "the windowed program opened its window and ended with 0"

# 4: a computer without the runtime
C="$S/norun"
mkdir -p "$C"; cp -r "$HERE/shim" "$C/shim"
python3 - "$C/shim/win/runtime.cpp" <<'PYEOF' || { echo "CONTROL SETUP FAILED"; exit 1; }
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', newline='').read()
key = "    std::wstring v;\n"
assert s.count(key) == 1, s.count(key)
open(p, 'w', encoding='utf-8', newline='').write(s.replace(key, "    std::wstring v; version.clear(); return 1;   // (this copy of the shim pretends the runtime is not installed)\n"))
PYEOF
SHIM="$C/shim" build "$S/missing.exe" || { cat "$S/missing.exe.log"; echo "BUILD (runtime missing) FAILED"; exit 1; }
(cd "$S/run" && timeout 60 "$S/missing.exe"; echo $? > "$S/missing.rc") &
pid=$!
text="$("$P" box "Runtime Test")" || { kill "$pid" 2> /dev/null; echo "no message box appeared"; exit 1; }
echo "message box: $text"
case "$text" in *"Microsoft WebView2 Runtime"*"does not have it"*) ;; *) kill "$pid" 2> /dev/null; echo "the message box does not say what is missing"; exit 1 ;; esac
[ "$("$P" state "runtime window" 2> /dev/null)" = none ] || { echo "a window was opened without the runtime"; exit 1; }
"$P" boxanswer "Runtime Test" no || { echo "the box could not be answered"; exit 1; }
wait "$pid"
[ "$(cat "$S/missing.rc")" = 3 ] || { echo "the program ended with $(cat "$S/missing.rc"), not 3"; exit 1; }
echo "without the runtime: a message box, no window, exit 3"

# 5: values that do not belong in a version resource are refused
for bad in "APP_VERSION=1.2.x" "APP_VERSION=1..2" "APP_VERSION=.1" 'APP_COMPANY=Bad"Co' "APP_COMPANY=Back\\slash"; do
    if build "$S/bad.exe" APP_VERSION=1.0.0 "$bad"; then echo "the build accepted $bad"; exit 1; fi
done
echo "distribution test: ok"
