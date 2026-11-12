# Size and build time against Tauri and Electron

The same program in each framework: four commands (`add`, `mean`, `greet`, `fail`) and a page that calls them and prints the results. The sources are in this folder; Pronao's `main.clg` is the program at the top of the [README](../../README.md).

## Result

Measured on 2026-11-11 on one Windows 11 machine with 32 cores.

| | Pronao | Tauri 2.12.5 (`--release`) | Tauri, size-tuned | Electron 45.3.0 (one portable `.exe`) |
|---|---|---|---|---|
| program | **1.2 MB** (1,241,600 bytes) | 8.6 MB (8,643,072) | 3.1 MB (3,146,752) | 110 MB (110,049,365); 373 MB unpacked |
| build from nothing | 14 s (14.1, 14.2, 14.6) | 56 s | 93 s | 103 s (103.0, 103.8, 103.1) |
| edit one command, rebuild | 12 s (11.9, 12.0, 12.1) | 13 to 16 s (13.3, 14.9, 16.4) | 51 s (50.4, 50.6, 51.3) | 103 s (103.3, 105.4) |

- A Pronao program that prints with `std::cout` is 2.0 MB: `iostream` brings about 0.8 MB. The program above does not print.
- Electron packaged into a folder with `@electron/packager` takes 4 s (3.7, 4.7, 4.7) and gives 373 MB of files. That is not an `.exe`, so it is not in the table.
- All of them were launched and stayed running for several seconds (Electron's first launch exited within five seconds and the second stayed up; not investigated).

## What this does not show

One trivial program, one machine, one build of each. Start-up time and memory were not measured. Tauri's debug profile and `tauri dev` (what Tauri developers use all day) were not measured; its rebuild there is probably much faster than the release rebuilds above. Tauri's runtime brings a plugin system and a permission system that Pronao does not have, which is part of the size. Electron's default compression was used; another compression level or an installer changes both its size and its time. A larger program changes Pronao's build time (every module is compiled again) and Tauri's (only your crate is).

## How each was built

All three need the Microsoft WebView2 Runtime except Electron, which carries its own browser. Times are wall-clock, taken with Python's `time.time()` around the build command, from an empty build folder, with dependencies already downloaded.

**Pronao** (from the repository root, with the CeeLing compiler and MinGW-w64 on the path):

```
PAGE=docs/comparison/pronao/index.html RELEASE=1 bash scripts/build.sh docs/comparison/pronao/main.clg out/app.exe
```

**Tauri** 2.12.5, Rust 1.98 (MSVC). The icon is generated with `python scripts/make_icon.py docs/comparison/tauri/icons/icon.ico tauri-hello`:

```
cd docs/comparison/tauri
cargo build --release
```

For the size-tuned column, uncomment the `[profile.release]` block in `Cargo.toml` (Tauri's documented options) and build again.

**Electron** 45.3.0, electron-builder 26.18.0 (Node 25):

```
cd docs/comparison/electron
npm install
npx electron-builder --win portable --x64
```

The first run downloads Electron and the builder's helper tools; it is not counted.
