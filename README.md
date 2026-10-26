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

The native layer (`shim/`, trusted C++, under 1000 lines for Windows) holds no application logic: it opens the window,
queues calls, opens files, runs programs, shows dialogs and waits for file changes, as small primitives that take and return
ints and strings. **Every decision is in `src/`**, which CeeLing checks: which files a page may reach, which programs it may
start, what a dialog may say, what a folder watch reports. CeeLing runs the loop, so no callback ever has to cross into
CeeLing. JSON is handled by [cjson-cling](../cjson-cling), reply texts are always built with it, never by string
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

**Where the policy lives, and why.** The allowed-folder policy is CeeLing code (`src/files.clg`, class `FileGate`), but it
never decides on the text of a path, because whether a path is inside a folder cannot be told from its text (`..`, symlinks
and junctions, `C:foo`, `\\?\` prefixes, `file:stream`, device names such as `NUL`, trailing dots). Every operation opens
first through a native primitive (`app_fs_open`), asks the operating system where the OPEN HANDLE really points, compares
that with the canonical allowed folders, and only then reads or writes through the same handle; a link that leads out of an
allowed folder is refused (and removing a link removes the link, never its target). The text is checked too, but only to
refuse early and without saying whether the path exists; it never allows anything by itself. A `forbidden` answer is the
same whether or not the file exists, so a page cannot probe the rest of the disk. Text only: `fs.read` returns the file as a
JSON string and refuses anything that is not valid UTF-8. Tested with 128 checks on Windows and 112 under ASan/UBSan on
Linux (`tests/files_main.clg`), and `tests/negative_controls.sh` removes the final-path check, the root guard and the
grant rules in turn and shows the tests then fail (so they can fail).
Not covered: a local attacker who can create links inside an allowed folder at the very moment of a write (a file may be
created and then removed again if it turns out to lie outside), non-ASCII case differences in a path's text on Windows
(the text pre-check refuses them; the OS's own answer is what allows), and whatever an allowed folder itself contains.

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

How an event gets to the page: a watcher thread in the native layer (ReadDirectoryChangesW on Windows, inotify on Linux) puts
one raw record per change in the same queue the page's calls arrive in, with an EMPTY id (a page's call always has one, and
the shim drops an empty-id call from the page). The CeeLing main loop (`App.run`, or `serveOne` if you write your own) merges
them, checks the permission, and turns them into `fs.change` events; the JSON is built with the JSON library. If the main
loop falls behind, records beyond 2000 queued are dropped and an `overflow` follows.

## The window

```
app.enableWindowCommands();
app.confirmClose("Unsaved work", "Close without saving?");   // optional, see below
```
```js
await cling.window.setTitle({ title: 'Report - draft' });
await cling.window.setSize({ width: 900, height: 600 });     // 200..10000 by 150..10000
await cling.window.minimize();  await cling.window.maximize();  await cling.window.restore();  await cling.window.focus();
await cling.window.setDirty({ dirty: true });                  // the page has unsaved work
await cling.window.close();
cling.window.onCloseDeclined(() => { ... });                   // the user pressed the X, was asked, and chose to stay
```
With `confirmClose`, pressing the window's X (or Alt+F4) no longer closes the window by itself: the native layer only queues a
record. If the page has said it has unsaved work (`setDirty`), CeeLing asks with a native box whose text is the
application's, never the page's, and closes only on "Yes"; with nothing unsaved the window just closes. `window.close` is the
page closing its own window and is not asked about. Title and size values are checked in CeeLing before they reach the
window. Windows only for now; the real-window self-test presses the X, answers the box and checks the title, size, maximize
and restore.

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

How a program is found and run: CeeLing (`resolveProgram`, 18 checks in `tests/programs_main.clg`) accepts an absolute path or a
bare name searched for on `PATH` only, never the current folder (a `.bat` or `.cmd` is refused, because `cmd.exe` would
re-read the arguments). The native runner (`shim/{win,posix}/proc.cpp`, 24 checks) quotes arguments with the C runtime's rules,
tested with every character a command line treats specially; the child has no standard input, no console window and
inherits only its three pipes; it runs inside a job object (a process group on Linux), so on a timeout, or if it leaves
something behind, the whole tree is killed; output is capped per stream and drained past the cap; text that is not valid
UTF-8 is repaired in CeeLing (`lossy: true`).
Not covered: what the program itself does (`git` with a hostile repository, for instance), environment variables (they
are inherited), and a program that parses its command line in a non-standard way.

## What is and is not checked

CeeLing checks the command table and everything the commands do with their own data: no dangling references, no
use-after-move, no raw pointers, bounds and null checks at run time. **Not checked, and trusted:** the native layer
(`shim/`), the `webview` library, the browser engine, every `ext_fn` declaration (a wrong signature is silently
believed), and any other C++ you link in.

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

bash tests/run_headless.sh            # 176 end-to-end checks + 128 file policy + 18 program lookup + 24 process + 26 watcher, no window
bash tests/run_window_selftest.sh     # real WebView2 window, page -> commands -> checks -> exit 0
bash tests/run_sanitizers.sh          # WSL: ASan+UBSan, TSan on the queue, the watcher and the CeeLing tests, lock-removal controls
bash tests/negative_controls.sh       # removes one check at a time from src/ and shows the tests fail
```

Requirements: the CeeLing compiler (`CLING`, default `c:/project/CeeLing/build/cling.exe`), cjson-cling (`CJSON`), MinGW
g++, the WebView2 runtime for window runs, WSL with g++ for the sanitizers. Windows is the only platform tried.

`third_party/` is not committed; `fetch.sh` recreates it. The window build links statically (about 3 MB).

## Layout

| path | what |
|---|---|
| `src/text.clg` | UTF-8 checking and repair, splitting, small parsers |
| `src/core.clg` | replies and structured errors, events to the page, argument helpers, `Command` / `AsyncCommand` / `Job` |
| `src/files.clg` | `FileGate`: the allowed-folder policy and every file operation, on top of the native open-and-ask primitives |
| `src/app.clg` | `Dispatcher`, `App`, the commands (files, programs, dialogs, watches, window), program lookup and slots, watch merging |
| `src/bridge.js` | defines `window.cling.*`; embedded into the program at build time |
| `shim/` | the native layer: `app_native.h` lists every primitive; `app_shim.cpp` window and queue; `win/` and `posix/` the OS parts |
| `examples/hello/` | window with three buttons |
| `tests/` | headless suites, real-window self-test, queue stress test, sanitizer and negative-control scripts; `tests/doubles/` stands in for the window and the dialogs |

## License

MIT, see `LICENSE`. webview and WebView2 keep their own licenses and are downloaded, not redistributed.
