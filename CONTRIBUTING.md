# Contributing

## Build and test

You need the [CeeLing compiler](https://github.com/AI4Engr/CeeLing) and [cJSON-cling](https://github.com/AI4Engr/cJSON-cling) checked out next to this repository, MinGW-w64 (`g++` on `PATH`) on Windows, and the WebView2 Runtime for the tests that open a window. `scripts/env.sh` says what is missing.

```bash
bash scripts/fetch.sh                  # once: webview and WebView2 headers into third_party/ (pinned, hash-checked)
bash tests/run_headless.sh             # ~20 s, 18 programs, 1,083 checks, no window
bash tests/run_examples.sh             # ~8 min: builds every program in examples/ and runs its 207 checks
bash tests/negative_controls.sh        # removes one check at a time from src/ and shows that the tests fail
```

The real-window tests (`tests/run_*_window.sh`, `run_window_selftest.sh`, `run_multiwindow.sh`, `run_tray_window.sh` ...) open a window and send keystrokes and mouse messages from outside; they are sensitive to which window has the focus, so run them with nothing else in front. `tests/run_sanitizers.sh` runs the headless suites under AddressSanitizer, UBSan and ThreadSanitizer in WSL. [docs/manual/01-getting-started.md](docs/manual/01-getting-started.md) lists every script and what it proves.

## The rule for changes

Every decision about what a page may do lives in `src/` (CeeLing), and a change to one comes with a test that fails without it:

* a headless check in `tests/*_main.clg` (see `tests/support.clg` for `roundTrip`), including the input the change must refuse; and
* if it removes a guard, a line in `tests/negative_controls.sh` that deletes the guard and shows the check fail.

`shim/` holds no policy: it opens, reads, runs, shows. A new native primitive is declared in `shim/app_native.h` and given a test double in `tests/doubles/`. A new feature needs a manual chapter section and, if it is a new part of the framework, an example in `examples/` with a check in `tests/examples/`.

## Source layout

[docs/manual/source-map.md](docs/manual/source-map.md) lists every file. In short: `src/` is the CeeLing modules, `shim/` the native layer (`win/` and `posix/` under it), `src/bridge.js` the page's `pronao` object, `tests/` the suites, `examples/` one program per part of the manual, `docs/manual/` the manual.

Commit messages say what changed and why in a few lines; the first line is a summary.
