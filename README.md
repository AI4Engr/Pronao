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

## Events: CeeLing to the page

```
app.emit("progress", "{\"done\":3}");            // main thread
emitEvent(handle, "progress", text);              // any thread, e.g. a worker job (capture the int handle)
```
```js
const off = cling.on('progress', p => bar.value = p.done);   // off() removes it
const next = await cling.once('finished');                   // Promise for the next one
```
The payload must be valid JSON text. The script sent to the page is built only with the JSON printer, so a name or a
string containing quotes, backslashes or script text stays a string literal; an invalid payload, or one with trailing
text, makes `emit` return false and sends nothing. An event sent before the page has loaded and registered its listener
is dropped; events from one thread arrive in order.

## Files

```
app.allowFolder("C:/Users/me/Documents/notes");   // nothing is reachable until a folder is allowed
app.enableFileCommands(16 * 1024 * 1024);          // fs.read refuses files larger than this
```
```js
await cling.invoke('fs.write', { path: 'todo.txt', text: 'buy milk', mode: 'append' });   // mode: replace | append | create
const { text } = await cling.invoke('fs.read', { path: 'todo.txt' });
const entries = await cling.invoke('fs.list', { path: '.' });     // [{name, kind: 'file'|'dir'|'link', size, modified}]
await cling.invoke('fs.stat', { path: 'todo.txt' });
await cling.invoke('fs.exists', { path: 'todo.txt' });            // true / false
await cling.invoke('fs.mkdir', { path: 'a/b/c', parents: true });
await cling.invoke('fs.remove', { path: 'a/b/c' });               // a file, a link, or an EMPTY folder
```
A relative path means "inside the first allowed folder". Errors reject with a code the page can switch on:
`forbidden`, `not_found`, `too_large`, `not_text`, `invalid_path`, `invalid_argument`, `already_exists`, `not_empty`,
`is_directory`, `not_directory`, `permission_denied`, `io_error`.

**Where the policy lives, and why.** The allowed-folder check is in the C++ shim (`shim/app_fs.cpp`), not in CeeLing,
because whether a path is inside a folder cannot be decided from its text (`..`, symlinks and junctions, `C:foo`,
`\\?\` prefixes, `file:stream`, device names such as `NUL`, trailing dots). Every operation opens first, asks the
operating system where the open handle really points, compares that with the canonical allowed folders, and only then
reads or writes through the same handle; a link that leads out of an allowed folder is refused (and removing a link
removes the link, never its target). A `forbidden` answer is the same whether or not the file exists, so a page cannot
probe the rest of the disk. Text only: `fs.read` returns the file as a JSON string and refuses anything that is not valid
UTF-8. This file code is trusted C++; it is tested with 96 checks on Windows and 80 under ASan/UBSan on Linux, and a
control run with the final-path check disabled fails the link-escape tests (so they can fail).
Not covered: a local attacker who can create links inside an allowed folder at the very moment of a write (a file may be
created and then removed again if it turns out to lie outside), and whatever an allowed folder itself contains.

## Watching folders

```
app.enableWatchCommands();
```
```js
const { id } = await cling.fs.watch({ path: 'notes', recursive: true });
const off = cling.fs.onChange(e => { /* {watch, kind, path, from?} */ });
await cling.fs.unwatch({ id });
```
`kind` is `created`, `modified`, `removed`, `renamed` (`path` is the new name, `from` the old), `overflow` (events were lost: read
the folder again) or `stopped` (`reason`: `closed`, `access_revoked`, `folder_gone`, `failed`; nothing more comes from that
watch). A page may watch only a folder the file commands can reach, at most 32 at a time. `path` is **relative to the watched
folder**, never absolute, so an event cannot reveal where the folder is. Rapid repeats are merged (sixty quick writes to one
file are a few events, not sixty), and the permission is re-checked before every delivery: withdrawing it with
`app.revokeGrants()` ends the watch with `access_revoked` and the change that followed is not reported. Changes made through a
link that leads out of the watched folder are not reported.

How an event gets to the page: a watcher thread in the shim (ReadDirectoryChangesW on Windows, inotify on Linux) puts a record
in the same queue the page's calls arrive in, with an EMPTY id (a page's call always has one, and the shim drops an empty-id
call from the page), the CeeLing main loop (`App.run`, or `serveOne` if you write your own) turns it into an `fs.change` event,
and the JSON is built with the JSON library, not in the shim. If the main loop falls behind, events beyond 1000 queued are
dropped and an `overflow` follows.

## Dialogs

```
app.enableDialogCommands();
```
```js
const picked = await cling.dialog.open({ title: 'Open a note', filters: [{ name: 'Text', extensions: ['txt', 'md'] }, { name: 'Everything' }] });
if (picked) { const { text } = await cling.fs.read({ path: picked.path }); ... }        // null when the user cancels
const saved  = await cling.dialog.save({ title: 'Export', defaultName: 'notes.txt' });  // {path} of a file that may not exist yet
const folder = await cling.dialog.folder({ title: 'Choose a project folder' });
const { button } = await cling.dialog.message({ title: 'Delete?', message: 'Delete the note?', kind: 'question', buttons: 'yesno' });  // 'yes' | 'no' | 'ok' | 'cancel'
```
**A pick is a grant.** What the user chooses in a native dialog becomes reachable through the file commands, exactly that
file (or, for a folder pick, that folder), and nothing around it: the neighbour files and the parent are still refused,
and `app.revokeGrants()` withdraws all of it. So a page cannot reach a file the application did not give it and the user
did not choose. The returned `path` is in the form the file commands accept (links resolved); use it as is. Dialogs are
asynchronous commands, so the rest of the application keeps running while one is open. Filters are checked (at most 20,
extensions of letters, digits, `_` and `-`), as are titles and messages (200 and 4000 bytes). Windows only for now (the
standard Vista dialogs); elsewhere a dialog rejects with `unsupported`.

## Running programs

The application lists which programs may run and with what arguments; the page can only pick one by name and fill in the
named slots. It never supplies a program, a command line or an option, and there is no shell.

```
std::vector<string> log;
log.push_back("log");  log.push_back("-n");  log.push_back("{count:int:1:500}");
log.push_back("--format=%H%x09%s");  log.push_back("--");  log.push_back("{file:file}");
log.push_back("@cwd={repo:folder}");                      // the working folder
app.allowProgram("git.log", "git", log, 10000, 1000000);   // name, program, template, timeout ms, output cap in bytes
app.enableProcessCommands();
```
```js
const r = await cling.proc.run({ program: 'git.log', args: { count: 20, repo: '.', file: 'src/a.txt' } });
// { exitCode, stdout, stderr, timedOut, truncated: {stdout, stderr}, lossy }   -- a non-zero exit code is a result, not an error
```
Slots: `{n:int:min:max}` a whole number in range; `{n:token}` letters, digits and `. _ - / @ ~ ^ + =`; `{n:text:maxLen}` any
plain text; `{n:file}` `{n:folder}` `{n:path}` an existing file / folder / either that the file commands could reach,
passed as its absolute path. A slot that fills a whole argument may not start with `-` (so a value can never be taken for
an option); one slot per argument; every slot is required; a parameter the template does not have is refused. `@cwd={n:folder}`
names the working folder, otherwise it is the first allowed folder.

What the shim does with it (`shim/app_proc.cpp`, trusted C++, 43 checks on Windows and 39 under ASan/UBSan on Linux):
the program is an absolute `.exe` found on `PATH` only, never the current folder (a `.bat` or `.cmd` is refused, because
`cmd.exe` would re-read the arguments); arguments are quoted with the C runtime's rules and tested with every character a
command line treats specially; the child has no standard input, no console window and inherits only its three pipes; it
runs inside a job object (a process group on Linux), so on a timeout, or if it leaves something behind, the whole tree is
killed; output is capped per stream and drained past the cap; text that is not valid UTF-8 is repaired (`lossy: true`).
Not covered: what the program itself does (`git` with a hostile repository, for instance), environment variables (they
are inherited), and a program that parses its command line in a non-standard way.

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
give: anything about `ext_fn`s, the shim, the browser engine, the file system or deadlocks. Nothing here is
"thread-safe".

Jobs run on a capped worker pool (default max(4, cores); `app.setWorkerLimit(n)` changes it; extra jobs queue, oldest
first, and the command loop never blocks). A job that throws rejects the page's promise with `command_failed` and the
message: the dispatcher installs a default failure handler around every `AsyncCommand.start` (and clears it after), and
a `spawn` can carry its own as a second closure. An in-flight job keeps the process alive: on exit the runtime
finishes every queued job and joins the pool, so a job that never finishes hangs the exit, and enough of them occupy
every slot and starve the queue.

## Build and test

```
bash scripts/fetch.sh                 # downloads webview 0.12.0 and WebView2.h 1.0.1150.38 (pinned, sha256-checked) into third_party/
PAGE=examples/hello/index.html bash scripts/build.sh examples/hello/main.clg hello.exe

bash tests/run_headless.sh            # 147 end-to-end checks, no window
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
