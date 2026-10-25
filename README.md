# app-cling

Desktop apps with a web UI and a CeeLing backend, in the spirit of Wails/Tauri:
the window is the system WebView (WebView2 on Windows), the page is HTML/JS, and the commands the page calls are
CeeLing code.

This is a **prototype**. It answers three questions: can the callback direction be reversed (CeeLing cannot
receive a function pointer), are commands-as-closures pleasant to write, and what would thread rules need.

```
app.command("add", closure Command [] (Item args) {
    return createNumber(numberAt(args, 0) + numberAt(args, 1));
});
```
```js
const sum = await cling.invoke('add', [2, 3]);       // 5
try { await cling.invoke('fail'); } catch (e) { e }  // {code: "command_failed", message: "..."}
```

## How it works

```
page JS --cling.invoke--> webview bind callback [UI thread] --> mutex queue (shim)
CeeLing main thread:  app_next_call()  <-- blocks on the queue
                      Dispatcher.handle(request)   parse JSON, find the command, call it
                      app_return(id, status, json) --> resolves / rejects the page's promise
```

The shim (`shim/`, about 250 lines of C++) owns the window thread and the queue and holds no application logic.
CeeLing runs the loop, so no callback ever has to cross into CeeLing. Everything crossing the boundary is an int or a
copied string. JSON is handled by [cjson-cling](../cjson-cling), reply texts are always built with it, never by string
concatenation.

Errors reach the page as a rejected promise carrying `{code, message}`: `bad_request` (not JSON, not an array, command
name not a string), `unknown_command`, `command_failed` (the handler threw, e.g. through `numberAt`/`stringAt`/`fieldOf`
when the page sent the wrong shape). A malformed request never stops the application.

## What is and is not checked

CeeLing checks the command table and everything the commands do with their own data: no dangling references, no
use-after-move, no raw pointers, bounds and null checks at run time. **Not checked, and trusted:** the shim, the
`webview` library, the browser engine, the six `ext_fn` declarations (a wrong signature is silently believed), and
any other C++ you link in.

**Background work and the thread rule.** A command that must not block the window is an `AsyncCommand`: its `start`
copies what it needs out of the request into plain values and calls `spawn(closure Job [payload, id, handle] () {...})`.
The compiler lets only plain values into the capture list (primitives, `string`, vectors of those, and `struct`s made
only of those) are copied, and a flat class object created with `new` in the same function can be MOVED in (the main
thread cannot touch it afterwards); an `Item` or any other object that does not meet the rules is a compile error
(make the data a `struct`, or pass a string). The exact rules are in CeeLing's CLAUDE.md. The worker
rebuilds its own JSON tree from the string and answers the page itself with `replyOk` / `replyError`, which go through
the mutex-protected shim. What this gives, stated exactly: *in the CeeLing code that was checked, a spawned job can
reach only its own copies and its own allocations, so there are no data races through `spawn`.* What it does not
give: anything about `ext_fn`s, the shim, the browser engine, the file system, deadlocks, or how many threads a
page can cause (one per async call, no cap). Nothing here is "thread-safe".

An in-flight job keeps the process alive: on exit the runtime joins every spawned thread, so a job that never
finishes hangs the exit. A job that throws is reported on stderr and its promise stays pending, so a job should
catch its own errors and call `replyError`.

## Build and test

```
bash scripts/fetch.sh                 # downloads webview 0.12.0 and WebView2.h 1.0.1150.38 (pinned, sha256-checked) into third_party/
PAGE=examples/hello/index.html bash scripts/build.sh examples/hello/main.clg hello.exe

bash tests/run_headless.sh            # 27 end-to-end checks, no window
bash tests/run_window_selftest.sh     # real WebView2 window, page -> commands -> checks -> exit 0
bash tests/run_sanitizers.sh          # WSL: ASan+UBSan, TSan on the queue, TSan negative control
```

Requirements: the CeeLing compiler (`CLING`, default `c:/project/CeeLing/build/cling.exe`), cjson-cling (`CJSON`), MinGW
g++, the WebView2 runtime for window runs, WSL with g++ for the sanitizers. Windows is the only platform tried.

`third_party/` is not committed; `fetch.sh` recreates it. The window build links statically (about 3 MB).

## Layout

| path | what |
|---|---|
| `src/app.clg` | `Command`, `Dispatcher`, `App`, argument helpers, the six `ext_fn`s |
| `src/bridge.js` | defines `window.cling.invoke`; embedded into the program at build time |
| `shim/` | the C++ shim (window thread, queue, handle table); `APP_HEADLESS` swaps the window for a test queue |
| `examples/hello/` | window with three buttons |
| `tests/` | headless suite, real-window self-test, queue stress test, sanitizer scripts |

## License

MIT, see `LICENSE`. webview and WebView2 keep their own licenses and are downloaded, not redistributed.
