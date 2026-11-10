# 1. Getting started

Fetch the dependencies, build the example, make a project of your own, run the tests.

## Build and test

```
bash scripts/fetch.sh                 # downloads webview 0.12.0 and WebView2.h 1.0.1150.38 (pinned, sha256-checked) into third_party/
PAGE=examples/hello/index.html bash scripts/build.sh examples/hello/main.clg hello.exe

bash tests/run_headless.sh            # ~20 s, 17 programs, about 1,000 checks (1,021 on 2026-11-09); each prints "N passed, 0 failed"; the tauriport suite needs the `lore` tool (SLOW=1: build at -O1)
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
bash tests/run_tauriport_window.sh     # the tauriport port in a real window, against the real lore tool
bash tests/run_tauriport_frontend.sh   # tauriport's own compiled front end, served from memory, in a real window
```

Requirements: the CeeLing compiler, cJSON-cling, a C++ compiler (MinGW-w64 on Windows), the WebView2 runtime for window runs, WSL with
g++ for the sanitizers. Windows is the only platform tried. The scripts find the tools themselves when CeeLing, cJSON-cling and app-cling
sit side by side (`../CeeLing/build/cling.exe`, `../cJSON-cling`) and `g++` is on `PATH`; otherwise set `CLING`, `CJSON`, `CXX`
(`scripts/env.sh` says what is missing). `WSL_DISTRO` picks the WSL distribution for the sanitizer run (default: WSL's default);
`TMPDIR` is where the test scripts work.

**A new project:** `bash scripts/new_project.sh ../my-app` makes a folder with a page, a `main.clg` with one command, a `build.sh`
(`--release`, `--headless`) and a README; `cd ../my-app && bash build.sh` builds `out/my-app.exe`. `tests/run_template.sh` checks that
a fresh project builds in all three ways and its window starts.

`third_party/` is not committed; `fetch.sh` recreates it. The window build links statically. Sizes of the hello example: 4.3 MB before, 4.1 MB with unused code dropped (`--gc-sections`, now always on), 1.6 MB with `RELEASE=1` (which also strips the symbols; `STRIP=1` strips without the other release switches). A stripped program has no function names for a debugger or a backtrace, so keep a plain build for debugging.
