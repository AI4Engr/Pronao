# One command, from the page to the reply

First version, 2026-10-29. The command is `fs.read`; the page asks for a file, and the answer comes back. Every piece of code below is quoted from the repository
(or from the C++ the compiler wrote from it, in `$TMPDIR/app-cling-headless/work_headless_main/` after `tests/run_headless.sh`). Line numbers in the generated code
are `.clg` lines: that is what a failed bounds or null check would print.

```
page                    native layer (C++, trusted)            CeeLing (checked)
 cling.fs.read({path})  ->  admit() into a queue   ->   Dispatcher.handle -> the command's closure
                                                              -> FileGate.read -> app_fs_read (native)
 the promise resolves   <-  app_return()           <-   a Reply {status, json}
```

## 1. The page

`src/bridge.js` is run before any page script. `cling.fs.read` is only a name for a call:

```js
var call = function (name) { return function (args) { return window.cling.invoke(name, args || {}); }; };
...
read: call('fs.read'), ...
```

`invoke` ends in `window.__cling_invoke(command, args)`, which returns a promise. The page has no other way to reach the program: no file API, no path of its own.

## 2. The native layer admits or refuses; it does not decide

`shim/app_shim.cpp` binds `__cling_invoke` to the window:

```cpp
w.bind("__cling_invoke", [raw, &w](const std::string& id, const std::string& req, void*) {
    const int refused = raw->calls.admit(id, req, raw->maxRequest, raw->maxQueued);
    if (refused == 1 || refused == 2) w.resolve(id, 1, refusalJson(refused));
}, nullptr);
```

`admit` only puts the request in a queue, and refuses a request that is too large, a queue that is too long, or an id the page could not have been given. It does not look
at the command name or the path. This runs on the window's thread; the program's main loop is another thread, and the queue is the only thing between them.

## 3. The main loop takes it (CeeLing)

`App.serveOne` in `src/app.clg` waits on every window at once and hands a request to the dispatcher:

```
int ready = app_wait_any(windows.handles, wait, from);
...
int r = app_next_call(from, 0, id, request);
...
Reply reply = dispatcher.handle(from, id, request);
if (reply.status != 2) {
    app_return(from, id, reply.status, reply.json);
}
```

`from` is the window the call came from: a window may call only the commands its own list allows (`rules.mayCall`), and the answer goes back to that window.

## 4. The dispatcher parses, checks, calls

`Dispatcher.handle` is plain CeeLing, about 70 lines. For `["fs.read",{"path":"notes.txt"}]` it: parses the text (a bad request is an error *value*, never a crash), checks that the
window may call `fs.read`, finds the handler, and calls it inside a `try`:

```
Command handler = handlers[index];
caller.handle = appHandle;
Item result = handler.call(args);
...
} catch (string message) {
    return new Reply(1, errorJson(errorCodeOf(message), errorTextOf(message)));
}
```

The only exception in CeeLing is a `string` of the form `code: text`. A command that fails throws one, and this `catch` turns it into the `{code, message}` the page's promise
rejects with.

## 5. The command: CeeLing beside the C++ it became

`enableFileCommands` registers it (`src/app.clg`; the helpers it uses are in `src/fscommands.clg`):

| CeeLing | generated C++ |
|---|---|
| `command("fs.read", closure Command [files, maxReadBytes] (Item args) {` | `class __closure_app_1 : public Command { std::shared_ptr<FileGate> files; int maxReadBytes; ... }` |
| `string path = requiredString(args, "path");` | `std::string path = requiredString(args, "path");` |
| `FsResult r = files.read(path, maxReadBytes);` | `std::unique_ptr<FsResult> r = cling::nn(this->files, "line 1866")->read(path, this->maxReadBytes);` |
| `fsCheck(r.status, "cannot read " + path);` | `fsCheck(cling::nn(r, "line 1867")->status, ("cannot read " + path));` |
| `if (!isUtf8(r.text)) { throw "not_text: ..."; }` | `if (!isUtf8(cling::nn(r, "line 1868")->text)) { throw std::string("not_text: ..."); }` |
| `Item result = createObject();` | `std::shared_ptr<Item> result = createObject();` |
| `addStringToObject(result, "text", r.text);` | `addStringToObject(result, "text", cling::nn(r, "line 1872")->text);` |

Things the source never says and the compiler decided:
- The closure lists what it captures, `[files, maxReadBytes]`, by value; there is no way to write "capture everything by reference". The closure became a class with those as fields.
- `files` is a `shared_ptr`, because the app and every command that uses the file policy hold it. `r` is a `unique_ptr`, because only this function owns that result.
- Each `.` on an object became `cling::nn(x, "line N")->`: if `r` were empty the program would print `null dereference at line 1867` and stop, instead of reading address 0.
- A failed `requiredString` throws `invalid_argument: \`path\` must be a string`; nothing else in this function can reach a missing field.

## 6. The file policy decides (CeeLing again)

`FileGate.read` in `src/files.clg` (shortened, the comments are added here). This is where a path becomes permission:

```
FsResult n = normalize(path, true, true);          // text only: absolute form; no devices, no streams, must be under an allowed place
if (n.status != 0) { return n; }
Opened o = openAllowed(n.text, 0);                  // open it, then ask the OS where the handle really points
...
int kind = kindOf(o.h);
if (kind != 102) { return new FsResult(finish(o.h, 1), ""); }     // a file, not a folder or a device
string data = "";
int st = app_fs_read(o.h, maxBytes, data);
app_fs_close(o.h);
return new FsResult(st, data);
```

The generated version:

```cpp
std::unique_ptr<FsResult> FileGate::read(const std::string& path, int maxBytes) {
    std::unique_ptr<FsResult> n = normalize(path, true, true);
    if ((cling::nn(n, "line 597")->status != 0)) {
        return std::move(n);                       // the compiler saw this was the last use of n
    }
    ...
    int st = app_fs_read(cling::nn(o, "line 612")->h, maxBytes, data);
    app_fs_close(cling::nn(o, "line 613")->h);
    return std::make_unique<FsResult>(st, data);
}
```

`openAllowed` opens first and compares what the *operating system* says the handle points to (`allowedFinal`) with the allowed places, so a link that leads out is refused
even if its name looked inside. The opened handle is then the only thing used: the check and the read cannot be about two different files. Every refusal is a number
(`1` forbidden, `2` not found, `3` too large ...), turned by `fsCheck` into `forbidden: cannot read /etc/passwd` at step 5.

## 7. The native read: one primitive, no policy

`app_fs_read` in `shim/win/fs.cpp` is what step 6 reaches. It is declared in CeeLing as an `ext_fn`, which is why the checked code may call it at all:

```cpp
int app_fs_read(int h, int max_bytes, std::string& out) {
    ...
    if (max_bytes < 0 || size.QuadPart > max_bytes) return 3;
    ... ReadFile(...) in pieces of at most 1 MiB ...
    out = std::move(data);
    return 0;
}
```

It reads the handle it is given; it does not know what a path is or whether this one was allowed.

## 8. The reply goes back

`Dispatcher.handle` returns `Reply(0, "{\"text\":\"...\"}")`; `serveOne` calls `app_return(from, id, 0, json)`, which resolves the page's promise from the right window.
For a refused read the same line carries `Reply(1, "{\"code\":\"forbidden\",\"message\":\"cannot read ...\"}")` and the promise rejects with that object.

## What this journey shows

- **Where the decisions are:** steps 3 to 6, all CeeLing, all compiled with the checks on. The two native steps (2 and 7) hold no policy.
- **What the page can influence:** the command name, the arguments, and nothing else. The window list, the allowed folders, the size limit are chosen by the program.
- **What the compiler contributed that is not visible in the source:** the pointer kinds, one `std::move`, and the `cling::nn` on each dereference.
- **What is outside the checking:** `app_fs_read` (the shim and the operating system), WebView2, and the correctness of the policy itself: the compiler can say that the code
  has no dangling reference, not that `allowedFinal` compares the right strings. That is what `tests/files_main.clg` (139 checks) and `tests/negative_controls.sh` are for.
