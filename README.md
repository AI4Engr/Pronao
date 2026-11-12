# Pronao

![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg) ![Status: prototype](https://img.shields.io/badge/status-prototype-orange.svg)

**Desktop apps with a web UI and a checked CeeLing backend.**

Pronao is a framework for building small desktop programs: the window is the system's WebView, the page is HTML and JavaScript, and the commands the page calls are written in [CeeLing](https://github.com/AI4Engr/CeeLing), a memory-safe dialect of C++. The compiler rejects dangling references and use after move in your commands, and the page can reach only what the program chose to allow: a folder, a list of programs, a list of commands.

A program is one `.exe` with no installer. The one below is **1.2 MB** and builds from nothing in 14 seconds; the same four commands in Tauri are 3.1 to 8.6 MB and in Electron 110 MB ([measurements and how to repeat them](docs/comparison/README.md)).

> **Status: prototype.** Windows is the only platform with a window. There are no releases: build from source.

## Quick start

On Windows with MinGW-w64, the [CeeLing compiler](https://github.com/AI4Engr/CeeLing) and [cJSON-cling](https://github.com/AI4Engr/cJSON-cling) checked out next to this repository:

```bash
bash scripts/fetch.sh                      # once: webview and WebView2 headers, pinned and hash-checked
bash scripts/new_project.sh ../my-app && cd ../my-app && bash build.sh
```

That builds `out/my-app.exe`: a page, a `main.clg` with one command, and a build script. To try what is here first, `PAGE=examples/hello/index.html bash scripts/build.sh examples/hello/main.clg hello.exe`.

## A complete program

Four commands and a window. There are eleven more programs in [examples/](examples/), one for each part below.

```
#include "app.clh"

int main() {
    App app = new App();

    app.command("add", closure Command [] (Item args) {
        return createNumber(numberAt(args, 0) + numberAt(args, 1));
    });
    app.command("mean", closure Command [] (Item args) {
        int n = getArraySize(args);
        if (n == 0) {
            throw "invalid_argument: give at least one number";
        }
        double total = 0.0;
        int i = 0;
        while (i < n) {
            total = total + numberAt(args, i);
            i = i + 1;
        }
        return createNumber(total / n);
    });
    app.command("greet", closure Command [] (Item args) {
        string name = requiredString(args, "name");
        return createString("hello, " + name);
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

and the page it shows, calling them:

```js
await pronao.invoke('add', [2, 3]);                 // 5
await pronao.invoke('mean', [4, 8, 12]);            // 8
await pronao.invoke('greet', { name: 'Ada' });      // "hello, Ada"
try { await pronao.invoke('fail'); }
catch (e) { console.log(e); }                       // {code: "command_failed", message: "this command always fails"}
```

## What it does

Everything is off until the program turns it on, and a page can ask only for what the program allowed.

* **Commands and events:** named commands with checked arguments, events from CeeLing to the page, errors as `{code, message}`.
* **Files:** an allowed-folder policy decided on the open handle, not on the text of a path; read, write, list, stat, remove.
* **Settings and folders:** per-user data folders and a declared, typed settings file.
* **Dialogs and drag and drop:** a pick or a drop becomes a grant for exactly that file.
* **Watching folders:** change events, merged and re-checked against the permission.
* **Programs and links:** a fixed list of programs with typed slots and no shell; addresses and files opened by the system.
* **Windows:** title, size, position, full screen, frameless, modal, several windows, remembered places, one running copy, an icon.
* **Menus and tray:** menu bar and shortcuts, tray icon, pop-up menus, notifications, light and dark.
* **Background work:** async commands on a worker pool, with cancelling and deadlines.
* **Limits on a page:** request size and flood limits, a navigation guard, command lists, release builds without developer tools.
* **A front end of many files and shipping:** a site served from memory, a development server, a release `.exe`, version information, signing.

Each has a chapter in the [manual](docs/manual/README.md) and a program in [examples/](examples/README.md).

## How it works

```
page JS --pronao.invoke--> webview bind callback [UI thread] --> mutex queue (shim)
CeeLing main thread:  app_next_call()  <-- blocks on the queue
                      Dispatcher.handle(request)   parse JSON, find the command, call it
                      app_return(id, status, json) --> resolves / rejects the page's promise
```

* **`src/`** is CeeLing, checked by the compiler. Every decision is here: which files a page may reach, which programs it may start, what a dialog may say.
* **`shim/`** is hand-written C++ and is trusted, not checked (2,409 lines of code for Windows, 1,363 for Linux, counted by `scripts/shim_lines.sh`). It opens the window, queues calls, opens files and runs programs, as small primitives. It holds no application logic.

CeeLing runs the loop, so no callback has to cross into CeeLing. [How it works](docs/manual/02-how-it-works.md) has the details.

## Platforms

| | |
|---|---|
| Windows 10 and 11 | the platform: the headless suite, real-window tests, a release build, signing. Needs the WebView2 Runtime (Windows 11 has it; `app.checkRuntime` says so on a computer without it) |
| Linux | files, programs, folder watching and opening links are implemented and run under the headless suite and the sanitizers (WSL); no window, dialogs, menu or tray |
| macOS | not tried |

## What is and is not checked

CeeLing checks the command table and everything the commands do with their own data: no dangling references, no use after move, no raw pointers, bounds and null checks at run time. For threads the claim is narrow: in the CeeLing code that was checked, a spawned job can reach only its own copies and its own allocations, so there are no data races through `spawn`. **Not checked, and trusted:** the native layer (`shim/`), the `webview` library, the browser engine, every `ext_fn` declaration (a wrong signature is silently believed) and any other C++ you link in. Nothing here is "thread-safe". What needs a real screen (how a menu is drawn, a toast, a real mouse drag) is not seen by the automated tests; each manual chapter says what its tests do and do not see.

## FAQ

**Is it an alternative to Electron or Tauri?** For small programs, in one way: it is much smaller, and the backend is checked. It has far fewer features, no installer or auto-update, and has been tried only on Windows. If you need those, use Tauri or Electron.

**Who is it for?** People who want to write the backend of a desktop program in CeeLing and have the compiler reject its memory errors, and who want one small `.exe` to hand to someone.

**Why "Pronao"?** The *pronaos* is the porch in front of a Greek temple's inner room. The page stands in the porch and may ask; what happens inside is decided by checked code.

## Documentation

* **[User manual](docs/manual/README.md):** ten chapters and two appendices, from getting started to shipping, including [what a command does from the page to the reply](docs/manual/appendix-command-journey.md) and [the compile errors you will meet](docs/manual/appendix-compile-errors.md).
* **[Examples](examples/README.md)** and the [comparison with Tauri and Electron](docs/comparison/README.md).
* [Source map](docs/manual/source-map.md): where things are in the repository.
* [Chapter 16 of the CeeLing guide](https://github.com/AI4Engr/CeeLing/blob/main/docs/language-guide/16-case-study-pronao.md): what the language did to this program, with counts of the checks it inserted and what they cost.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT, see [LICENSE](LICENSE). webview and WebView2 keep their own licenses and are downloaded, not redistributed.
