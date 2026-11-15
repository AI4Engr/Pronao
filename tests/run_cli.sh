#!/usr/bin/env bash
# The `pronao` command (the file pronao at the top of the repository): what it does with the scripts it wraps, and what it refuses.
# Cheap part, on small fake projects whose build.sh makes a tiny program (so no window is needed): the usage and the exit codes, `create` (files, both icons, refusals),
# a folder that is not a project, `build` options, `test` (a failing test.sh must fail it), `run` (arguments reach the program) and `dev` (the address, the developer-tools
# switch, a development server that is started and stopped; a non-local address is refused), `package` (the version, the files, the .deb or the exe), `doctor`.
# Real part (unless CLI_TEST_CHEAP=1): a project made by `pronao create` goes through `pronao test` (headless build) and `pronao package` (release build); the file that comes out
# must exist, and on Linux the .deb must name the version and depend on GTK 3 and WebKitGTK.
# Then three CONTROLS, each a copy of `pronao` with one rule removed; the cheap part must fail for each.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRONAO="${CLI_UNDER_TEST:-$HERE/pronao}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe; OS=windows ;; *) EXE=; OS=linux ;; esac
W="${CLI_WORK:-${TMPDIR:-/tmp}/pronao-cli}"
rm -rf "$W"; mkdir -p "$W"
fail() { echo "FAILED: $1"; exit 1; }
pn() { bash "$PRONAO" "$@"; }

# ---- the cheap part ----
cheap() {
    out="$(pn help 2>&1)"; [ $? -eq 0 ] && printf '%s' "$out" | grep -q 'pronao create' || fail "help does not list the commands"
    pn > /dev/null 2>&1; [ $? -eq 2 ] || fail "no command must exit 2"
    pn frobnicate > /dev/null 2>&1; [ $? -eq 2 ] || fail "an unknown command must exit 2"
    echo "usage and exit codes"

    # create
    pn create "$W/my-app" > "$W/create.log" 2>&1 || { cat "$W/create.log"; fail "create"; }
    for f in main.clg index.html build.sh README.md .gitignore icon.ico icon.png; do [ -f "$W/my-app/$f" ] || fail "create made no $f"; done
    [ "$(head -c8 "$W/my-app/icon.png" | od -An -tx1 | tr -d ' \n')" = "89504e470d0a1a0a" ] || fail "icon.png is not a PNG"
    pn create "$W/my-app" > /dev/null 2>&1 && fail "create wrote into a project that exists"
    pn create "$W/9bad" > /dev/null 2>&1 && fail "create accepted a name that starts with a digit"
    pn create "$W/bad name" > /dev/null 2>&1 && fail "create accepted a name with a space"
    pn create > /dev/null 2>&1; [ $? -eq 2 ] || fail "create with no folder must exit 2"
    echo "create: the files, both icons, and the refusals"

    # a folder that is not a project
    mkdir -p "$W/plain"
    out="$(pn -C "$W/plain" build 2>&1)"; [ $? -ne 0 ] && printf '%s' "$out" | grep -q 'not a Pronao project' || fail "build outside a project must say so"
    pn -C "$W/does-not-exist" build > /dev/null 2>&1 && fail "-C with a folder that is not there must fail"
    echo "a folder that is not a project is refused"

    # a fake project: build.sh makes a small program that prints what it was given
    mkdir -p "$W/fake"
    cp "$HERE/tests/cli_fake_build.sh" "$W/fake/build.sh"
    out="$(pn -C "$W/fake" build --nonsense 2>&1)"; [ $? -ne 0 ] || fail "build accepted an unknown option"
    pn -C "$W/fake" build --release > /dev/null 2>&1 || fail "build --release"
    [ "$(cat "$W/fake/last-build-args.txt")" = "--release" ] || fail "build did not pass --release on: $(cat "$W/fake/last-build-args.txt")"
    pn -C "$W/fake" build > /dev/null 2>&1 || fail "build"
    echo "build: the options go through, an unknown one is refused"

    # test
    out="$(pn -C "$W/fake" test 2>&1)"; [ $? -eq 0 ] && printf '%s' "$out" | grep -q 'only the build was checked' || fail "test without a test.sh: $out"
    [ "$(cat "$W/fake/last-build-args.txt")" = "--headless" ] || fail "test did not build headless"
    printf 'exit 1\n' > "$W/fake/test.sh"
    pn -C "$W/fake" test > /dev/null 2>&1 && fail "a failing test.sh did not fail the test"
    printf 'exit 0\n' > "$W/fake/test.sh"
    pn -C "$W/fake" test > /dev/null 2>&1 || fail "a passing test.sh failed the test"
    rm "$W/fake/test.sh"
    echo "test: a headless build, and test.sh decides"

    # run and dev
    out="$(pn -C "$W/fake" run -- alpha "b c" 2>&1)"; printf '%s' "$out" | grep -q 'ARGS=alpha;b c;' || fail "run did not pass its arguments: $out"
    printf '%s' "$out" | grep -q 'URL=- DEBUG=-' || fail "run set the development variables: $out"
    pn -C "$W/fake" run alpha > /dev/null 2>&1 && fail "run took arguments without --"
    out="$(pn -C "$W/fake" dev --url http://localhost:5999/ 2>&1)"; printf '%s' "$out" | grep -q 'URL=http://localhost:5999/ DEBUG=1' || fail "dev did not set the address and the developer tools: $out"
    out="$(pn -C "$W/fake" dev 2>&1)"; printf '%s' "$out" | grep -q 'URL=http://localhost:5173/ DEBUG=1' || fail "dev has no default address: $out"
    for bad in http://example.com/ https://localhost/ http://localhost.evil.com/ http://192.168.0.5:5173/; do
        pn -C "$W/fake" dev --url "$bad" > /dev/null 2>&1 && fail "dev accepted $bad"
    done
    echo "run and dev: arguments, address, developer tools; an address that is not on this machine is refused"
    if command -v python3 > /dev/null 2>&1 || command -v python > /dev/null 2>&1; then
        PY="$(command -v python3 || command -v python)"
        mkdir -p "$W/site"; echo hi > "$W/site/index.html"
        PORT=$((20000 + RANDOM % 20000))
        out="$(pn -C "$W/fake" dev --url "http://127.0.0.1:$PORT/" --server "cd '$W/site' && exec '$PY' -m http.server $PORT --bind 127.0.0.1" 2>&1)"
        printf '%s' "$out" | grep -q "URL=http://127.0.0.1:$PORT/ DEBUG=1" || fail "dev --server did not start the program once the server answered: $out"
        sleep 1
        curl -s -o /dev/null -m 2 "http://127.0.0.1:$PORT/" && fail "the development server was left running after the program ended"
        echo "dev --server: started, waited for, and stopped"
    else
        echo "(no python: dev --server not tested)"
    fi

    # package
    pn -C "$W/fake" package --version x1 > /dev/null 2>&1 && fail "package accepted a version that does not start with a digit"
    out="$(pn -C "$W/fake" package --version 4.5.6 --out "$W/fake-dist" 2>&1)" || fail "package: $out"
    [ "$(cat "$W/fake/last-build-args.txt")" = "--release" ] && [ "$(cat "$W/fake/last-build-version.txt")" = "4.5.6" ] || fail "package did not build a release of version 4.5.6"
    if [ "$OS" = windows ]; then
        [ -f "$W/fake-dist/fake-4.5.6.exe" ] || fail "package made no fake-4.5.6.exe: $(ls "$W/fake-dist")"
        printf '%s' "$out" | grep -q 'NOT signed' || fail "package did not say the exe is unsigned"
    else
        [ -f "$W/fake-dist/fake_4.5.6_$(dpkg --print-architecture 2> /dev/null).deb" ] || fail "package made no fake_4.5.6_*.deb: $(ls "$W/fake-dist")"
    fi
    echo "package: version 4.5.6, a release build, the file for this system"

    # doctor
    out="$(pn doctor 2>&1)"; rc=$?
    printf '%s' "$out" | grep -q '^pronao doctor' && printf '%s' "$out" | grep -q 'CeeLing compiler' || fail "doctor prints nothing useful: $out"
    [ "$rc" -eq 0 ] || fail "doctor reports something missing on a machine that builds: $out"
    echo "doctor: ok on this machine"
}

# ---- the real part ----
real() {
    export OBJ_CACHE="$W/objcache"
    pn create "$W/real-app" > /dev/null || fail "create (real)"
    ( cd "$W/real-app" && WORK="$W/real-work-headless" bash "$PRONAO" test > "$W/real-test.log" 2>&1 ) || { tail -20 "$W/real-test.log"; fail "pronao test on a real project"; }
    echo "a real project: pronao test (headless build) ok"
    ( cd "$W/real-app" && WORK="$W/real-work-release" bash "$PRONAO" package --version 2.3.4 > "$W/real-package.log" 2>&1 ) || { tail -20 "$W/real-package.log"; fail "pronao package on a real project"; }
    if [ "$OS" = windows ]; then
        [ -f "$W/real-app/dist/real-app-2.3.4.exe" ] || fail "no dist/real-app-2.3.4.exe"
    else
        DEB="$W/real-app/dist/real-app_2.3.4_$(dpkg --print-architecture).deb"
        [ -f "$DEB" ] || fail "no .deb: $(ls "$W/real-app/dist")"
        INFO="$(dpkg-deb -I "$DEB")"
        printf '%s\n' "$INFO" | grep -q '^ Version: 2.3.4$' || fail "the .deb has the wrong version"
        printf '%s\n' "$INFO" | grep '^ Depends:' | grep -Eq 'libgtk-3-0' && printf '%s\n' "$INFO" | grep '^ Depends:' | grep -Eq 'libwebkit2gtk-4\.[01]' || fail "the .deb does not depend on GTK 3 and WebKitGTK"
        dpkg-deb -c "$DEB" | grep -q 'icons/hicolor/64x64/apps/real-app.png' || fail "the .deb has no icon (the project's icon.png)"
    fi
    echo "a real project: pronao package (release build) made the file for this system"
}

cheap
if [ "${CLI_TEST_CHEAP:-0}" != 1 ]; then real; fi
echo "cli test: ok"

# ---- controls ----
if [ -z "${CLI_UNDER_TEST:-}" ]; then
    C="${TMPDIR:-/tmp}/pronao-cli-control"
    rm -rf "$C"; mkdir -p "$C"
    control() { # name  old  new -> the cheap part must fail against a copy of pronao with `old` replaced by `new`
        local name="$1" old="$2" new="$3"
        python3 "$HERE/tests/cli_control.py" "$HERE/pronao" "$C/$name" "$HERE" "$old" "$new" || { echo "CONTROL SETUP FAILED ($name)"; exit 1; }
        if CLI_UNDER_TEST="$C/$name" CLI_TEST_CHEAP=1 CLI_WORK="$C/work-$name" bash "${BASH_SOURCE[0]}" > "$C/$name.out" 2>&1; then echo "CONTROL DID NOT FAIL: $name"; exit 1; fi
        echo "control: $name -> the test fails, as it must"
    }
    control no-project-check '|| die "$PROJECT is not a Pronao project (no' '|| : "$PROJECT is not a Pronao project (no'
    control no-address-check 'case "$URL" in http://localhost' 'case "$URL" in * ) ;; http://localhost'
    control package-ignores-version '--version) [ $# -ge 2 ] || bad "package: --version needs a number"; VERSION="$2"; shift 2 ;;' '--version) shift 2 ;;'
fi
