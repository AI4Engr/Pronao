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

The native layer (`shim/`, trusted C++; 1059 lines of code for a Windows build, 1258 counting comments and blank lines, `scripts/shim_lines.sh`) holds no application logic: it opens the window,
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

## A web UI of many files

One embedded HTML string was enough for a prototype; a real front end is hundreds of files (scripts, styles, images, fonts, data) and
wants a real origin (relative URLs, `localStorage`, CORS). So the program carries a folder and serves it from memory:

```
ASSETS=path/to/dist bash scripts/build.sh app.clg app.exe        # the folder is packed into the program
```
```
app.openSite("My app", 1100, 700, false, "http://app.localhost", "");     // origin, optional Content-Security-Policy
```
The window loads `http://app.localhost/`; that origin is answered by the program, never by the network, and an address that is not in
the folder is a 404 (it does not fall through to the network). `index.html` is `/`, `docs/index.html` is `/docs` and `/docs/`, `local.html` is
`/local` and `/local/` (how a static export such as Next.js's is linked). Types come from the extension. The bridge, the
navigation guard and the request limits apply to these pages exactly as to a single embedded page. `src/assets.clg` decides
names, types and addresses; the native side (`shim/assets.h`, `shim/win/assets.cpp`) only looks an address up in a table.
`EXTRA_JS="a.js b.js"` adds scripts to the init script that runs in every page before its own scripts.

Measured, and what it does NOT do: the files are stored as they are (about 4 MB for tauriport's whole front end). Compression was tried
and does not work: WebView2 does not unpack a `Content-Encoding: gzip` on a response a program makes itself, the page received the gzip
bytes. Compressing would need an inflate in the program (zlib-cling has one). One site per process, Windows only (a custom scheme in
WebKitGTK is the Linux way), no range requests, no streaming. The origin is yours to choose: tauriport uses `http://tauri.localhost`
because its server's CORS list already contains it.

## Cancelling, deadlines and closing

```
app.setAsyncTimeout("proc.run", 120000);         // "" = every async command without a setting of its own; 0 = no deadline (the default)
app.setShutdownGrace(5000);                       // the default
```
```js
const c = new AbortController();
const p = cling.invoke('proc.run', { program: 'git.log', args: { ... } }, { signal: c.signal });
c.abort();                                        // p rejects at once with {code: 'cancelled'}; the program is killed
```
- **A page call is answered once.** The native layer remembers which calls are waiting; the first answer settles the page's promise and any
  later one (a job that kept going after a deadline, a second `replyOk`, an id a worker made up) is dropped. So an answer sent by a timeout
  and the job's own late answer cannot both reach the page.
- **Abandoning.** `cling.invoke(..., {signal})` tags the call; aborting rejects the promise immediately and sends `$cancel`. The call
  counts as *cancelled*: a program the call started (`proc.run`, anything run with a `ToolRun`) is killed together with everything it started,
  within about 50 ms, and a job of your own can check `callCancelled(handle, id)` between its steps and stop. Cancelling is a request, not an
  interruption: CeeLing cannot stop a loop that never checks, and a dialog stays open until the person answers. Only async commands can be
  abandoned; a sync command has finished before the cancel could arrive.
- **Deadlines.** After `setAsyncTimeout` milliseconds the call is answered `{code: 'timed_out'}`, counts as cancelled, and its late answer is
  dropped. Do not set one on the dialog commands, which wait for a person.
- **Closing.** When the window closes (or `app.close()` runs) every call of it is cancelled, in flight and later: running programs are
  killed, watchers are stopped, and jobs that check `callCancelled` stop. After `main` returns the jobs still running get `setShutdownGrace`
  milliseconds; then the process ends without them (a message on stderr says how many, status 0; nothing is unwound). Without the grace
  period a job that never ends keeps the process alive for ever, which is CeeLing's default (`cling::setExitGraceMs`).
- **Not done**: a job that blocks in a call this layer does not know about (an `ext_fn` of yours) cannot be interrupted, only abandoned at
  exit; there is no cancel for a running dialog; `Channel<T>` (results without going through an `ext_fn`) needs the compiler.

## What a page may send, and where the window may go

```
app.setRequestLimits(4 * 1024 * 1024, 256);      // bytes per call, calls waiting; these are the defaults
app.allowNavigation("http://localhost:5173/");   // optional, e.g. a dev server; before open
app.allowOnlyCommands(names);                    // optional: this window's page may call only these (a std::vector<string>)
```
- **Size and flood limits.** A call larger than the limit, or arriving while that many are already waiting, is answered at once
  with `{code: "too_large"}` / `{code: "busy"}` and never reaches a command, so a page cannot make the application buffer
  without bound. A call whose id is empty or longer than 64 bytes is dropped (an empty id marks the shim's own records; a page
  could try to forge one by calling the browser's message channel directly).
- **Navigation.** The bridge is injected into every document the window shows, so a window that could be sent to another site
  would hand the application's commands to a stranger's page. After the application's own page, the window loads nothing except
  an address that starts with an allowed prefix; links, redirects, scripts and **frames** pointing elsewhere are cancelled, no
  new window can open, and the right-click menu is off in a release build (`debug` false). Each refusal reaches the page as the
  event `window.navigationBlocked {url}` (`cling.window.onNavigationBlocked`). The real-window self-test tries a frame, a script
  navigation and a popup, and fails if the guard is removed. Windows only.
- **A window's own command list.** `app.allowOnlyCommands(names)` (after the commands are registered) limits what this window's
  page can call; anything else, registered or not, is answered `{code: "forbidden"}` (so names cannot be probed), and a name that
  is not registered makes the call return false and change nothing. One `App` is one window; a second window will need its own.
- **Developer tools off in release.** `RELEASE=1 scripts/build.sh ...` compiles the window with `-DAPP_RELEASE`: `open`'s `debug`
  argument is then ignored (no developer tools, no right-click menu) because the switch is not in the binary, not because a
  variable is unset. Without `RELEASE`, `debug` works as before.
- **The environment of a program** is withheld: a program gets only `PATH`, `SystemRoot`, `SYSTEMDRIVE`, `windir`, `TEMP`, `TMP`,
  `TMPDIR`, `LANG`, `LC_ALL`, `HOME` and `USERPROFILE` (those that are set), plus whatever `app.passEnvironment("NAME")` adds.
  A token in the application's own environment is not handed to `git`. A program linked dynamically needs `PATH` to find its
  libraries, which is why it is in the default list.

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

## Porting tauriport (the real test)

`examples/tauriport/` is tauriport's Tauri backend (17 commands: a folder under version control with the `lore` tool, history, diff,
revision bytes, a sync-link file, a watcher, two dialogs) rewritten as CeeLing, with `tauri-bridge.js` giving a page the same
`tauriBridge` object its frontend already uses. `tests/tauriport_main.clg` runs 50 checks against the real `lore` in a scratch
folder, and `tests/run_tauriport_window.sh` drives the same commands from a real page in a real WebView2 window, including a
file change coming back through the watcher.

What it took, and what it showed:
- **The Tauri version enforced no file policy at all** (`write_file_bytes` wrote anywhere, any `root` was accepted). Here the
  opened folder is a grant and every `root` and `path` the page names goes through `FileGate`; a `path` that goes up, or a
  path / message / revision that `lore` could read as an option, is refused before `lore` starts.
- **Two new pieces of API came out of it**: `ToolRun` / `runTool` / `runToolOrThrow` (an application command runs its own fixed
  program, as opposed to the page choosing from the `allowProgram` table), and `Later` (a worker that needs the file policy
  after its program ends sends the main loop a record: `App.later` + `finishLater`). Also `fs.writeBytes` / `fs.readBytes`
  (binary through JSON as base64, 10 MiB in about 90 ms) and `fs.remove {recursive}`.
- **The tests found a real bug on the way**: a recursive remove of the opened folder emptied it and only then refused the folder
  itself. It now refuses first (`tests/negative_controls.sh` removes that check and the test fails).
- **Friction that is the language's, recorded in CeeLing's TODO**: a method cannot be called on a call's result
  (`f(x).length()`, which cost a local variable in about thirty places); a closure nested in a closure cannot capture the outer
  one's captures; an object passed to a function is moved, so a recursive walk has to be a method; `std::map` has no `size()` and
  I did not get iteration to work, so merging two lists is a loop inside a loop; decoding page arguments is still `requiredString` / `getObjectItem` by
  hand (`write_link` checks its shape field by field where serde did it from a struct).
- **tauriport's own compiled front end runs, unchanged.** `TAURIPORT_FRONTEND=<its Next.js out folder> bash examples/tauriport/build.sh app.exe`
  serves all 120 files from memory and injects `tauri-internals.js`, which provides the little of `__TAURI_INTERNALS__` that
  `@tauri-apps/api` needs (invoke, callbacks, the event plugin). `tests/run_tauriport_frontend.sh` opens it in a real window and checks that
  the app routes itself to its login page, hydrates and raises no script error. Past the login page it needs an account on the server, which
  I did not use; the commands behind it are the ones tested above.
- **Differences from the Tauri behaviour**: a revision's bytes are extracted into `<folder>/.tauriport/extract-N` (and removed)
  because the file policy must be able to read them back, not into the system temp folder; `lore` is found by `LORE_CLI_PATH`
  or on `PATH` only (Tauri also looked next to its own executable; there is no primitive for that yet); the watcher is stopped
  when the folder changes (Tauri's thread ran for the life of the process).

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

bash tests/run_headless.sh            # 205 end-to-end + 139 file policy and text + 18 program lookup + 50 tauriport (needs lore) + 33 process + 26 watcher, no window
bash tests/run_window_selftest.sh     # real WebView2 window, page -> commands -> checks -> exit 0
bash tests/run_sanitizers.sh          # WSL: ASan+UBSan, TSan on the queue, the watcher and the CeeLing tests, lock-removal controls
bash tests/negative_controls.sh       # removes one check at a time from src/ and shows the tests fail
bash tests/run_tauriport_window.sh     # the tauriport port in a real window, against the real lore tool
bash tests/run_tauriport_frontend.sh   # tauriport's own compiled front end, served from memory, in a real window
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
| `src/assets.clg` | the program's own web files: types, names, page aliases, `publishSite` |
| `src/app.clg` | `Dispatcher`, `App`, the commands (files, programs, dialogs, watches, window), program lookup and slots, watch merging |
| `src/bridge.js` | defines `window.cling.*`; embedded into the program at build time |
| `shim/` | the native layer: `app_native.h` lists every primitive; `app_shim.cpp` window and queue; `win/` and `posix/` the OS parts |
| `examples/hello/` | window with three buttons |
| `tests/` | headless suites, real-window self-test, queue stress test, sanitizer and negative-control scripts; `tests/doubles/` stands in for the window and the dialogs |

## License

MIT, see `LICENSE`. webview and WebView2 keep their own licenses and are downloaded, not redistributed.
