# Pronao

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg) ![Status: prototype](https://img.shields.io/badge/status-prototype-orange.svg)

**Desktop apps with a web UI and a [CeeLing](https://github.com/AI4Engr/CeeLing) backend, in the spirit of Wails and Tauri.** The window is the system WebView (WebView2 on Windows), the page is HTML and JavaScript, and the commands the page calls are CeeLing code: checked for dangling references and use after move, compiled to plain C++.

A complete program (`examples/hello` has a fuller one with a page; build it with the Quick start below):

```
#include "app.clh"

int main() {
    App app = new App();
    app.command("add", closure Command [] (Item args) {
        return createNumber(numberAt(args, 0) + numberAt(args, 1));
    });
    app.command("fail", closure Command [] (Item args) {
        throw "this command always fails";
    });
    if (!app.openPage("My app", 640, 420, false)) {
        return 1;
    }
    app.run();
    app.close();
    return 0;
}
```

and the page it shows (`PAGE=index.html`), calling those commands:

```js
const sum = await pronao.invoke('add', [2, 3]);                 // 5
try { await pronao.invoke('fail'); } catch (e) { console.log(e) }   // {code: "command_failed", message: "this command always fails"}
```

The commands are CeeLing, so the compiler checks them: a dangling reference or a use after move in a command is a compile error, not a crash in front of a user. The page can call only what the program registered.

## Small, and quick to build

The program above, built with `RELEASE=1`, is **one 1.2 MB `.exe`**: no installer, no runtime of its own, no DLLs next to it. The window is the system's WebView, so the program carries only your commands and the native layer, linked statically with unused code dropped. The only thing it needs from the computer is the Microsoft WebView2 Runtime (Windows 11 has it; an older Windows 10 may not, and `app.checkRuntime` says so with a message box instead of failing silently).

| | Measured on 2026-11-11 (the program above) |
|---|---|
| `RELEASE=1`: windowed, symbols stripped | **1.2 MB** (1,238,016 bytes) |
| default build (`-O1`, symbols kept for a debugger) | 2.9 MB |
| a build from nothing: CeeLing to C++ to `g++`, all 20 modules and the native layer | **about 14 s** (three runs: 13.8, 14.0, 13.8) on a 32-core machine |
| the same with a warm object cache | 11.7 s (the CeeLing step and the link are not cached) |

Against Tauri 2.12.5 (Rust 1.98, MSVC), the same two commands and the same page, same machine, built from an empty target folder, dependencies already downloaded (420 crates compiled):

| | Pronao | Tauri, default `--release` | Tauri, size-tuned (`opt-level = "s"`, `lto`, `codegen-units = 1`, `panic = "abort"`, `strip`) |
|---|---|---|---|
| executable | **1.2 MB** | 8.6 MB | 3.1 MB |
| build from nothing | **14 s** | 56 s | 85 s |
| after editing the program itself, rebuild | 12 s | 13 s | 47 s |

All three were launched and stayed running for five seconds. The size and the clean build are where Pronao is clearly ahead; for the edit-and-rebuild loop it is about level with Tauri's default release build (Tauri's debug profile, which `tauri dev` uses, was not measured). Read the comparison for what it is: one trivial program, one machine (32 cores), one build of each; start-up time and memory were not measured. The Pronao program is smaller partly because it has less in it: Tauri's runtime brings a plugin system, a permission system and its own event and asset machinery. Electron ships a copy of Chromium and Node and is far larger; I did not build one.

> **Status: prototype.** It answers three questions: can the callback direction be reversed (CeeLing cannot receive a function pointer), are commands written as closures pleasant, and what rules do threads need. Windows is the only platform tried; there are no releases, build from source.

## How it works

```
page JS --pronao.invoke--> webview bind callback [UI thread] --> mutex queue (shim)
CeeLing main thread:  app_next_call()  <-- blocks on the queue
                      Dispatcher.handle(request)   parse JSON, find the command, call it
                      app_return(id, status, json) --> resolves / rejects the page's promise
```

Two layers, and the line between them is the point:

* **`src/`** is CeeLing, checked by the compiler. Every decision is here: which files a page may reach, which programs it may start, what a dialog may say, what a folder watch reports.
* **`shim/`** is hand-written C++ and is trusted, not checked (2,409 lines of code for a Windows build, 1,363 for Linux, counted by `scripts/shim_lines.sh`). It opens the window, queues calls, opens files, runs programs and shows dialogs, as small primitives that take and return numbers and strings. **It holds no application logic.**

CeeLing runs the loop, so no callback ever has to cross into CeeLing. The page never touches the operating system: it can only call the commands the application registered, and each command checks its own arguments. [How it works](docs/manual/02-how-it-works.md) has the details; [what is checked and what is trusted](docs/manual/08-security.md) says exactly where the guarantee stops.

## Quick start

Windows with MinGW-w64, the [CeeLing compiler](https://github.com/AI4Engr/CeeLing) and [cJSON-cling](https://github.com/AI4Engr/cJSON-cling) checked out next to this repository:

```bash
bash scripts/fetch.sh            # webview 0.12.0 and WebView2.h (pinned, sha256-checked) into third_party/
PAGE=examples/hello/index.html bash scripts/build.sh examples/hello/main.clg hello.exe
./hello.exe                      # a window with three buttons

bash scripts/new_project.sh ../my-app && cd ../my-app && bash build.sh      # a project of your own: a page, a main.clg, a build script
bash tests/run_headless.sh       # ~20 s, 17 programs, 982 checks, no window needed
```

[Getting started](docs/manual/01-getting-started.md) lists every test script and what each one proves.

## What it can do

Everything below is off until the application turns it on, and a page can ask only for what the application allowed.

| Area | What | Manual |
|---|---|---|
| Commands and events | named commands with checked arguments, events from CeeLing to the page, structured errors | [2](docs/manual/02-how-it-works.md) |
| Files | an allowed-folder policy checked against the open handle, not the path text; read, write, list, stat, remove | [3](docs/manual/03-files-and-data.md) |
| Settings, folders | per-user data folders and a declared, typed settings file | [3](docs/manual/03-files-and-data.md) |
| Dialogs, drag and drop | open, save, folder, message; a pick or a drop becomes a grant for exactly that file | [3](docs/manual/03-files-and-data.md) |
| Watching folders | change events, merged and re-checked against the permission | [3](docs/manual/03-files-and-data.md) |
| Programs and links | a fixed list of programs with typed slots, no shell; links and files opened by the system | [4](docs/manual/04-programs-and-links.md) |
| Windows | title, size, position, full screen, always on top, frameless, modal, several windows, remembered places, one running copy, an icon | [5](docs/manual/05-windows.md) |
| Menus and tray | menu bar and shortcuts, tray icon, pop-up menus, notifications, light and dark | [6](docs/manual/06-menus-tray-notifications.md) |
| Background work | async commands on a worker pool, cancelling, deadlines, `Channel` and pumps | [7](docs/manual/07-background-work.md) |
| Limits on a page | request size and flood limits, navigation guard, command lists, release builds without developer tools | [8](docs/manual/08-security.md) |
| A front end of many files, shipping | a site served from memory, a dev server, a release exe, signing | [9](docs/manual/09-web-ui-and-shipping.md) |

## What is and is not checked

CeeLing checks the command table and everything the commands do with their own data: no dangling references, no use after move, no raw pointers, bounds and null checks at run time. For threads the claim is narrow: in the CeeLing code that was checked, a spawned job can reach only its own copies and its own allocations, so there are no data races through `spawn`. **Not checked, and trusted:** the native layer (`shim/`), the `webview` library, the browser engine, every `ext_fn` declaration (a wrong signature is silently believed) and any other C++ you link in. Nothing here is "thread-safe".

Behaviour that needs a real screen (how a menu is drawn, a toast, a real mouse drag) cannot be seen by the automated tests. Each manual chapter says what its tests do see and what they do not.

## Documentation

* **[User manual](docs/manual/README.md)**: ten chapters and two appendices, from getting started to shipping.
* [Giving a program to someone else](docs/manual/10-distribution.md): the runtime check, the release build, signing, a checklist.
* [One command, from the page to the reply](docs/manual/appendix-command-journey.md): `fs.read` with the generated C++ beside the CeeLing.
* [Compile errors you will meet](docs/manual/appendix-compile-errors.md) and the fix for each.
* [Source map](docs/manual/source-map.md): where things are in the repository.
* [Chapter 16 of the CeeLing guide](https://github.com/AI4Engr/CeeLing/blob/main/docs/language-guide/16-case-study-pronao.md): what the language did to this program, with counts of the checks it inserted and what they cost.

## License

MIT, see [LICENSE](LICENSE). webview and WebView2 keep their own licenses and are downloaded, not redistributed.
