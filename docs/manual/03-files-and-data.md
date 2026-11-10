# 3. Files and data

What a page may read and write, the program's own folders and settings, folder watching, dialogs and dropped files.

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

## The application's own folders and settings

Where a program keeps its data (`src/settings.clg`; the native layer only makes a folder and renames a file):

```
app.allowAppFolder("my-app", "data");            // makes %LOCALAPPDATA%\my-app (XDG: ~/.local/share/my-app) and lets the page's fs.* commands reach it
SettingsStore settings = app.settings;           // the program's settings file, in the "config" folder (%APPDATA%\my-app\settings.json)
settings.declareString("theme", "light", 16);    // every key is declared: type, default and the values accepted
settings.declareInt("fontSize", 12, 8, 32);
settings.declareBool("compact", false);
settings.open("my-app");                         // makes the folder; "" if it worked, else why not
settings.load();                                 // reads the file (a missing one is fine); what was wrong in it is in settings.problem
app.enableSettingsCommands();                    // the page may now call settings.get / getAll / set, for these keys only
int size = settings.getInt("fontSize");          // the program reads and writes with getX / setX
```

```js
await cling.settings.get('theme');               // "light"
await cling.settings.set('fontSize', 14);        // saved at once; every window gets the event settings.changed {key, value}
await cling.settings.getAll();                   // {theme, fontSize, compact}
```

- **Kinds of folder:** `"data"` and `"cache"` can be allowed to the page; `"config"` cannot, because it holds `settings.json`, which a page may change only through the settings commands. There is no
  `"temp"` kind: a predictable name in the shared temporary folder can be taken over by another user. The application name is 1 to 64 letters, digits, `.`, `_` or `-` (so it can never be a path).
  macOS is treated like Linux (no `~/Library` table yet).
- **The page cannot reach `settings.json`** with `fs.read` / `fs.write`: the store has its own file gate that allows only its folder, and the page's gate does not include it. It sees only the keys the program declared;
  a key that was not declared is `unknown_setting`, a value of the wrong type or outside its range is `invalid_argument`.
- **Saving** writes `settings.json.tmp` and swaps it in over `settings.json` in one step (`FileGate.replace`), so a crash leaves the old file or the new one. A key in the file that the program does not
  declare is kept when it saves; a value in the file that does not fit its declaration is replaced by the default and reported in `settings.problem`; a file that is not JSON is reported and the defaults are used.
- **What this is not:** the file is not encrypted and not protected from other programs of the same user; two instances of the program writing at once are not coordinated (the last one wins); the
  settings commands run on the main thread, so a slow disk stalls the loop for the length of one small write.

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

## Files dragged onto a window

```
app.enableFileDrop(8, false);                          // up to 8 files from one drop; true as the second argument also accepts a dropped folder
app.disableFileDrop();
```

```js
cling.onDrop(async ({ x, y, files, skipped }) => {      // files: [{path, kind: "file" | "folder"}], x and y in CSS pixels
  for (const f of files) console.log(await cling.fs.read({ path: f.path }));       // each dropped file is already granted, as a dialog pick would be
});
cling.onDragEnter(({ x, y }) => {}); cling.onDragOver(({ x, y }) => {}); cling.onDragLeave(() => {});    // to draw a drop zone
```

- **What is granted:** exactly the dropped file (or folder, if allowed), as a dialog pick is: the file next to it is not reachable; at most 256 grants exist at a time (`app.revokeGrants()` takes them back). `skipped` counts the paths
  that were over the limit, are not plain text, do not exist (any more), or are folders when folders are not accepted.
- **Where the paths come from.** The browser engine owns the window's drop target, and the program cannot take it over (tried: a drop target registered on the program's own window is never asked, because the engine's windows sit
  between the pointer and it). So `src/bridge.js` listens to the page's drag events and, on a drop, hands the engine the files (`postMessageWithAdditionalObjects`); the engine names their paths for the program. This needs
  WebView2 SDK 1.0.2210.55 or later at build time (`scripts/fetch.sh`) and a current runtime (the interface is there from version 110).
- **Trust, stated exactly:** the paths are those of files the **user** put into the page, by dropping them or by choosing them in a file input of the page's own; a page cannot make a `File` that has a path. A page that holds such a
  file can ask for the same grant a drop would give, and nothing else (it still cannot name a path of its own). Without `enableFileDrop` the engine's default stays: the drop is refused by the navigation guard.
- **A page that handles drops itself** (it calls `preventDefault` in its own dragover/drop handler) is left alone. A drag of text or a picture is not a drop of files.
- **Not done:** an application-side handler in CeeLing (the event goes to the page), dragging files OUT of the window, other systems.
- **Test:** `tests/system_main.clg` (the grants, the limit, the folder rule, a path that does not exist, a late drop) and `tests/run_system_window.sh` (a real OLE drag with the left button held and the pointer moved over the window by
  `SendInput`, as the file manager does: one file, three in order, five cut to four, a folder, a drag of text that is not a drop of files, "off" and on again; a control in which the paths never reach the program). The drag is made by
  the test with the same OLE calls Explorer uses; it is not a real Explorer window. It refuses to start if another window covers the target (once it landed in a code editor), and a drag the engine refuses the first time is
  tried once more.
