# 1. Getting started

Fetch the dependencies, build the example, make a project of your own, run the tests.

## Build and test

```
bash scripts/fetch.sh                 # downloads webview 0.12.0 and the WebView2 SDK 1.0.2210.55 (pinned, sha256-checked) into third_party/
PAGE=examples/hello/index.html bash scripts/build.sh examples/hello/main.clg hello.exe

bash tests/run_headless.sh            # ~20 s, 18 programs, about 1,100 checks (1,084 on 2026-11-14); each prints "N passed, 0 failed" (SLOW=1: build at -O1)
bash tests/run_examples.sh           # ~8 min: builds each program in examples/ and runs its 14 to 23 headless checks (211 in all)
bash tests/run_distribution.sh       # release build: windowed program, version and icon in the exe, the runtime check with a box, bad version values refused
bash tests/run_signing.sh            # signing with a throwaway certificate: signed, runs, a changed file is caught, refusals
bash tests/run_layout_window.sh       # a real window: full screen, always on top, no title bar, fixed size, a place fitted to the monitors, a modal window; two controls that must fail
bash tests/run_notify_window.sh       # a real program: the shell takes a notification, a click reaches the program and the page, the icon goes away; two controls
bash tests/run_system_window.sh       # a real window: forced and followed light/dark (page and title bar), a played change of the system, files dragged onto it; four controls
bash tests/run_tray_window.sh         # a real program: the tray icon, hiding on close, the tray menu; two controls that must fail
bash tests/run_menu_window.sh         # a real window: the menu bar, choosing items, shortcuts, greyed items, ticks; two controls that must fail
bash tests/run_icon.sh                # the icon is in the exe and on the running window; the build without one is the control
bash tests/run_placement_window.sh    # a real window is moved, maximized and closed from outside; the next start must open it there; with a control that must fail
bash tests/run_instance_window.sh     # two real processes, one window: the second hands over its arguments and ends; with a control that must fail
bash tests/run_window_selftest.sh     # real WebView2 window, page -> commands -> checks -> exit 0
bash tests/run_sanitizers.sh          # ~80 s, WSL: ASan+UBSan, TSan on the queue, the watcher and the CeeLing tests, lock-removal controls
bash tests/negative_controls.sh       # ~65 s: removes one check at a time from src/ and shows the tests fail
```

Requirements: the CeeLing compiler, cJSON-cling, a C++ compiler (MinGW-w64 on Windows) and zlib's library (`-lz`; MinGW-w64 has it), the WebView2 runtime for window runs, WSL with
g++ for the sanitizers. Windows is the platform with the full window layer; Linux has a first version on GTK 3 and WebKitGTK ([appendix D](appendix-linux.md); needs `libgtk-3-dev` and `libwebkit2gtk-4.1-dev` to build) and also runs the headless suite and the sanitizers (see the README's Platforms table). The scripts find the tools themselves when CeeLing, cJSON-cling and Pronao
sit side by side (`../CeeLing/build/cling.exe`, `../cJSON-cling`) and `g++` is on `PATH`; otherwise set `CLING`, `CJSON`, `CXX`
(`scripts/env.sh` says what is missing). `WSL_DISTRO` picks the WSL distribution for the sanitizer run (default: WSL's default);
`TMPDIR` is where the test scripts work.

**A new project:** `bash pronao create ../my-app` (the script behind it is `scripts/new_project.sh`) makes a folder with a page, a `main.clg` with one command, a `build.sh`
(`--release`, `--headless`), a README and two placeholder icons (`icon.ico` for Windows, `icon.png` for Linux); `cd ../my-app && bash build.sh` builds `out/my-app.exe` (`out/my-app` on Linux).
`tests/run_template.sh` checks that a fresh project builds in all three ways and its window starts.

## The pronao command

`pronao` (the file at the top of the repository) is one bash script for the things you do to a project. It has no logic of its own beyond finding the project and its name: every
command calls a script that already exists, so what is described in the other chapters is what runs. Run it as `bash /path/to/pronao <command>`, or put the Pronao folder on `PATH`.
It works on the project in the current folder; `-C <folder>` before the command picks another one. A folder is a project when its `build.sh` calls Pronao's build (`pronao create` makes one).

| command | what it does | script behind it |
|---|---|---|
| `pronao doctor` | says what is missing (compiler, cJSON-cling, a C++ compiler, the webview library, GTK 3 and WebKitGTK or the WebView2 SDK) and the command that fixes it; exits 1 if something is | `scripts/env.sh` and checks of its own |
| `pronao setup` | clones CeeLing and cJSON-cling next to Pronao, builds the compiler, fetches the webview library | `scripts/setup.sh` |
| `pronao create <folder> [name]` | a new project | `scripts/new_project.sh` |
| `pronao build [--release] [--headless]` | builds `out/<name>`; `--release` has no developer tools and no development-server switch; `--headless` only checks that it compiles | the project's `build.sh` |
| `pronao run [-- args]` | builds, then starts the program; what comes after `--` is the program's | `build.sh` |
| `pronao dev [--url URL] [--server "cmd"]` | builds, then starts the program on a front-end development server (default `http://localhost:5173/`, **an address on this machine only**) with the developer tools on. `--server "npm run dev"` starts the front end's own server first, waits until it answers (60 s at most) and stops it when the program ends | `build.sh`; `APP_DEV_URL`, `APP_DEBUG` ([chapter 9](09-web-ui-and-shipping.md)) |
| `pronao test` | a headless build (the program must transpile and compile) and then the project's `test.sh`, if it has one; a failure in either fails the command. A project's own tests are yours to write: `test.sh` can run the program's headless checks as `tests/headless_main.clg` does | `build.sh --headless` |
| `pronao package [--version V] [--out DIR]` | a release build, then the file for this system in `dist/`: on **Windows** a portable `name-V.exe` (with its size and SHA-256, and whether it was signed: `SIGN_COMMAND` in the environment signs it, [chapter 10](10-distribution.md)); on **Linux** a `.deb` with its dependencies, launcher entry and the project's `icon.png` ([appendix D](appendix-linux.md)). macOS: refused, there is no window layer yet | `build.sh --release`, `scripts/package_linux.sh` |
| `pronao selftest [headless\|window\|template\|cli\|all]` | Pronao's own tests, for working on Pronao itself | `tests/run_*.sh` |
| `pronao version`, `pronao help` | | |

The version of a package is `--version`, else `APP_VERSION` in the environment, else the default in the project's `build.sh` (0.1.0). The package name on Linux is the project name in
lower case. **What it does not do:** it does not install a front-end toolchain, it does not publish anything, and it does not sign unless you give it a signing command.

`tests/run_cli.sh` tests the command: on fake projects (usage and exit codes, `create` and its refusals, a folder that is not a project, `build` options, `test` with a failing and a
passing `test.sh`, `run` with arguments, `dev` with its address check and a development server that must be started, waited for and stopped, `package` with its version and file), then on
a real project (`pronao test`, `pronao package`, and on Linux what is inside the `.deb`), and three controls (a copy of `pronao` without the project check, without the address check, or
that ignores `--version`: each must fail the test). Not tested: a window started by `run` or `dev` on a real display (the template test starts one), `pronao setup` (it downloads), and `doctor` on a machine where something is missing.

`third_party/` is not committed; `fetch.sh` recreates it. The window build links statically. Sizes, measured on 2026-11-11: the program in the README (four commands, no `std::cout`) is 1.2 MB with `RELEASE=1` (which also strips the symbols; `STRIP=1` strips without the other release switches); `examples/hello`, which prints with `std::cout`, is 2.0 MB, because `iostream` brings about 0.8 MB with it, and 4.9 MB without `RELEASE=1`. Unused code is always dropped (`--gc-sections`). A stripped program has no function names for a debugger or a backtrace, so keep a plain build for debugging.
