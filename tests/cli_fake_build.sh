#!/usr/bin/env bash
# A stand-in for a Pronao project's build.sh, for tests/run_cli.sh (the real one calls scripts/build.sh). It records how it was called and makes a tiny program that prints what
# it was started with (the development-server address, the developer-tools switch and its arguments), so `pronao run`, `dev` and `package` can be checked without a window.
# (This mentions scripts/build.sh so that pronao takes the folder for a project.)
APP_NAME="fake"; APP_VERSION="${APP_VERSION:-0.3.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac
for a in "$@"; do case "$a" in --release|--headless) ;; *) echo "unknown option $a" >&2; exit 2 ;; esac; done
echo "$@" > "$HERE/last-build-args.txt"
echo "${APP_VERSION}" > "$HERE/last-build-version.txt"
mkdir -p "$HERE/out"
cat > "$HERE/out/fake.cpp" <<'CPP'
#include <cstdio>
#include <cstdlib>
int main(int c, char** v) {
    const char* url = std::getenv("APP_DEV_URL");
    const char* debug = std::getenv("APP_DEBUG");
    std::printf("URL=%s DEBUG=%s ARGS=", url ? url : "-", debug ? debug : "-");
    for (int i = 1; i < c; i++) std::printf("%s;", v[i]);
    std::printf("\n");
}
CPP
g++ "$HERE/out/fake.cpp" -o "$HERE/out/fake$EXE" || exit 1
