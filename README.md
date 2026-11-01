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

## Opening links, files and folders in the system

A link in the page does nothing by default (the navigation guard cancels it). The application decides what may leave the window (`src/links.clg`; the native layer only hands a target to the shell):

```
app.allowLinkSchemes(schemes);            // from "http", "https", "mailto" -- until this is called no address is opened
app.allowOpenExtensions(types);           // "pdf", "txt" ...: the file types shell.openFile may open; program and script types are refused here
app.openLinksInBrowser(true);             // a CLICK on a link (or target=_blank) opens it in the default browser; a script that sets location does not
app.enableShellCommands();                // shell.openUrl, shell.openFile, shell.reveal
```

```js
await cling.shell.openUrl({ url: 'https://example.com/docs' });   // default browser
await cling.shell.openFile({ path: 'C:/Users/me/Docs/report.pdf' });   // default program; the file must be reachable by the file commands and of a listed type
await cling.shell.reveal({ path: '...' });                          // shown in Explorer / the file manager; starts nothing
```

- **Addresses** must be http, https or mailto, allowed by the application, in printable ASCII (percent-encode the rest; no spaces, quotes, `<>` or backslashes), at most 2048 bytes, and for http / https a host with no `user@`.
  `file:`, `javascript:` and `data:` are never opened.
- **Files** go through the same file policy as `fs.*` (an allowed folder, a regular file, links resolved), and the extension must be on the application's list. Extensions that start a program or script
  (`exe`, `bat`, `cmd`, `lnk`, `js`, `ps1`, `hta`, `sh`, `py`, ... about 70 of them) are refused even if the application lists one. Opening a program is `proc.run`'s job, with its own table.
- **At most about three opens a second**, so a page cannot open a thousand tabs.
- **Not verified automatically:** that a browser, the default program or Explorer really appear. The tests check what the system is asked to do (a test double records it) and that refused targets ask for nothing; the native call
  itself was checked only for its refusals. Click it once by hand on a machine with a browser.
- **What it cannot know:** what the browser or the default program will do with what it gets. Listing `html` or `svg` for `openFile` is the application's choice.

## Giving the program to someone else

`docs/distribution.md` has the whole page; the short version for a portable exe:

```
RELEASE=1 ICON=app.ico APP_VERSION=1.4.0 APP_NAME="My app" APP_COMPANY="Example Co" bash scripts/build.sh main.clg out/my-app.exe     # or: bash build.sh --release in a new project
if (!app.checkRuntime("My app")) { return 1; }       // first thing in main: a message box, not a silent failure, on a computer without the WebView2 runtime
app.showMessage("My app", "It cannot start.");        // a message box for what a windowless program has to say (a release build has no console)
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sign.ps1 -File out\my-app.exe -Pfx cert.pfx -PasswordEnv SIGN_PASSWORD -Timestamp http://timestamp.digicert.com
```

- `RELEASE=1` links a **windowed** program (no console window on double-click); `GUI=0` keeps the console. `APP_VERSION` etc. become the version resource Explorer shows.
- **Tested:** the subsystem, the version resource and icon read back from the exe, the windowed program starting and ending, a computer without the runtime (the shim made to report it missing: box, no window, exit 3),
  signing with a throwaway certificate (who signed, a changed file reported as changed, a wrong password refused).
- **Not tested:** a computer that really lacks the runtime, a certificate from a recognized authority, a timestamp server, SmartScreen. No installer (a portable exe has none; see the page for what that leaves out).

## Menus: the window's menu bar and keyboard shortcuts

The application declares every menu (`src/menu.clg`); a page cannot add, change or show an item. It can only be told which item was chosen.

```
Menu bar = new Menu();
bar.begin("&File");                                  // a submenu (begin / end nest at most 4 deep); the letter after & is the keyboard mnemonic
bar.item("file.open", "&Open...", "Ctrl+O");         // id, label, shortcut ("" = none)
bar.item("file.save", "&Save", "Ctrl+S");
bar.separator();
bar.item("file.quit", "&Quit", "Ctrl+Q");
bar.end();
bar.begin("&View");
bar.check("view.grid", "&Grid", "F5", false);        // an item with a tick
bar.end();
app.setMenu(bar);                                    // the first window; Window.setMenu(bar) for another one
app.onMenu(closure MenuAction [app] (string id) { /* id is "file.open", ... */ });
app.menuEventsToPage(true);                          // optional: the page also gets the event menu.click {id}
Window w = app.windows.get(app.handle);
w.setEnabled("file.save", false);                    // grey an item out (its shortcut stops working too)
w.setChecked("view.grid", true);
```

- **What is checked when the menu is built** (each call returns false and sets `bar.problem`): an id is 1 to 64 letters, digits, `.`, `_`, `-` and used once; a label is 1 to 200 bytes of text without control characters and at most one `&`; at most 200 entries;
  a shortcut is Ctrl / Alt / Shift and a key (letters, digits, F1 to F12, Enter, Esc, Space, Tab, Backspace, Delete, Insert, Home, End, PageUp / PageDown, the arrows) and is used once. **A key that types into a page needs Ctrl or Alt**, or the menu would take it away from the page;
  F1 to F12 may stand alone. A menu bar's top level must be submenus only.
- **A click** (or a shortcut pressed while the page has the keyboard) reaches `onMenu` with the id. An id the menu does not have, a submenu and a greyed item are ignored even if the native layer reported them.
- **Shortcuts** are matched in the browser engine before the page sees the key, so they work while the page holds the keyboard; the key is not passed on. Held-down keys do not repeat the action.
- **Where it works:** Windows. Elsewhere `setMenu` returns false and the program runs without a bar.
- **Test:** `tests/menu_main.clg` (53 checks: the rules, the flat spec the native layer gets, click handling with a test double) and `tests/run_menu_window.sh` (a real window; items are chosen and keys typed from outside with `tests/window_poke.cpp`:
  the bar has the declared items, a chosen item and a shortcut reach the program, F5 ticks and unticks, a greyed item and its shortcut do nothing, the page fits under the bar, two controls that must fail).
- **Not verified automatically:** opening the menu with the mouse and looking at it (drawing, high-DPI, keyboard navigation inside the menu), the Alt mnemonics, and that a screen reader can use the menu.

## The tray icon, and a window that hides instead of closing

```
Window w = app.windows.get(app.handle);
w.hideOnClose(true);                                  // the X (and Alt+F4) hides the window; the program and its page keep running
Menu popup = new Menu();                              // the tray menu: items at the top level are fine
popup.item("tray.show", "&Show window", "");
popup.check("tray.mute", "&Mute", "", false);
popup.separator();
popup.item("tray.quit", "&Quit", "");
int tray = app.openTray("My app", popup);             // the program's icon (ICON= in build.sh), a tooltip of at most 127 bytes; 0 if it could not be made
app.onTray(closure TrayAction [app, shown] (string kind) { /* "click" or "double" */ app.showWindow(shown); });
app.onMenu(closure MenuAction [app] (string id) { if (id == "tray.quit") { app.quit(); } });     // the same handler as the window's menu bar
app.trayChecked(tray, "tray.mute", true);             // also trayEnabled, trayTooltip; closeTray(tray) removes the icon
```

- **Quit and the end of the program:** the last window closing ends the program, as always; a *hidden* window is still open, so hiding does not. A program that lives only in its tray (no window at all) says `app.keepRunning(true)` and
  ends with `app.quit()`. `App.close()` removes the icons: **call it at the end of `main`**, or the icon stays in the notification area until the mouse passes over it.
- **The right click** opens the tray menu; **the left click and the double click** are for `onTray`. A greyed item and an id the tray menu does not have do nothing.
- **Windows 11 note:** icons the user has not pinned sit in the overflow flyout; the program cannot change that.
- **Test:** `tests/run_tray_window.sh` with a real program: the shell holds the icon, the close button hides the window and the program goes on, a click brings it back, the right click opens the menu (Esc closes it), tray items are chosen,
  Quit ends the program and the tray window is gone; two controls (hiding does nothing; tray items reach nobody). The test posts the messages the shell sends and does not move the mouse onto the real icon.
- **Not verified:** the icon's picture and tooltip as the user sees them, a real click on the icon, the shell being restarted (the icon is added again on `TaskbarCreated`: written, not exercised), the icon removal when the process is killed (the shell then keeps a ghost until the mouse passes over it).

## Window layout: position, full screen, always on top, no title bar, fixed size, modal windows

What the application decides (a `Window`, `app.windows.get(app.handle)`):

```
w.setBounds(100, 80, 900, 600);       // fitted to the monitors that exist (never off the screen, never bigger than the biggest monitor); false in full screen
std::vector<int> b = w.bounds();      // x, y, width, height, state (a sum: 1 maximized, 2 minimized, 4 full screen, 8 always on top, 16 no title bar, 32 not resizable, 64 hidden)
w.setFullscreen(true);                // the whole monitor the window is on, no frame; false puts it back exactly as it was (a maximized window comes back maximized)
w.setAlwaysOnTop(true);
w.setFrameless(true);                 // no title bar and no border line; the thin resize border stays while the window may be resized
w.setResizable(false);                // a fixed-size window (no resize border, no maximize button)
child.setModalFor(owner);             // while child is open the owner is greyed out and ignores the mouse and the keyboard; it is usable again when child closes, however it closes
```

What a page may ask (off until the application registers it: `app.enableWindowLayoutCommands(allowFullscreen, allowAlwaysOnTop)`):

```js
const b = await cling.layout.getBounds();                  // {x, y, width, height, maximized, minimized, fullscreen, alwaysOnTop, frameless, resizable}
await cling.layout.setBounds({ x: 100, y: 80, width: 900, height: 600 });     // fitted, as above
// a window with no title bar: the page draws its own and starts the system's move or resize from a mouse down on it
titlebar.onmousedown = function () { cling.layout.startDrag(); };
grip.onmousedown = function () { cling.layout.startResize('bottomright'); };     // left, right, top, bottom, topleft, topright, bottomleft, bottomright
await cling.layout.setFullscreen(true);                     // only if the application allowed full screen
await cling.layout.setAlwaysOnTop(true);                    // only if the application allowed always on top
await cling.invoke('window.open', { start: '/dialog.html', modal: true });    // a window the page opens that disables the page that opened it
```

- **What the page cannot do:** take away its own title bar or resize border (`setFrameless` / `setResizable` are the application's), cover the screen or stay in front unless the application said it may (a page that can do both can imitate the system's
  own windows), or start a move out of nothing: `startDrag` / `startResize` act only while the left mouse button is held down.
- **A frameless window and the page's own title bar:** the buttons are the page's: `cling.window.minimize()`, `maximize()`, `close()` already exist. Windows' snap layouts and the shadow of a window with a title bar are not kept.
- **Numbers the page sends** are checked: `x`, `y` within -32000 to 32000, width 200 to 10000, height 150 to 10000.
- **Test:** `tests/layout_main.clg` (32 checks with a double) and `tests/run_layout_window.sh` (a real window; after each change the test asks Windows what the window looks like: the rectangle, topmost, caption, resize border, enabled, whether it covers its
  monitor; a drag with no button held does nothing; a modal window disables its owner and gives it back when the application closes it and when the user does; two controls that must fail).
- **Not done:** transparent windows, owned (non-modal) child windows that stay on top of their parent and minimize with it, a window bigger than one monitor on purpose (full screen across all monitors), more than one real monitor (the fitting is tested with a double; this
  machine has one), dragging and resizing with a real mouse button (the test checks the guard, not the drag).

## System notifications

```
app.enableNotifications("My app");                     // a program with no tray icon: a notification-only icon, in the notification area only while a notification is up
                                                       // (a program with a tray icon (openTray) needs nothing: its notifications come from that icon)
string why = app.notify("Download finished", "report.pdf is ready", "info", "open-report");   // title, text, kind (info, warning, error), id; "" if the system took it
app.onNotificationClick(closure NotificationAction [app] (string id) { /* id is "open-report" */ });
app.enableNotificationCommand();                       // the page may ask too: notification.show {title, text, kind?, id?}
app.setNotificationGap(5000);                          // the least time between two notifications (default 2 s; 200 ms to 10 minutes); the page cannot change it
```

```js
await cling.notify('Saved', 'Your changes are safe', { kind: 'info', id: 'saved' });      // rejected with {code: "too_many"} if it comes too soon
cling.onNotificationClick(function (id) { /* the user clicked a notification THIS page asked for */ });
```

- **What is checked:** the title is 1 to 63 bytes of text on one line, the text 1 to 255 bytes (line breaks allowed), no control characters, the kind is one of three, the id is a name (1 to 64 letters, digits, `.`, `_`, `-`).
  The page chooses nothing else: no icon, no sound, no link, no buttons. At most one notification every 2 seconds, for the application and the page together.
- **How it is shown:** Windows' balloon / toast from a notification-area icon (no registration, no installer, no Start-menu shortcut needed). Under Windows 10 and 11 it appears as a toast and goes to the notification
  centre. The notification-only icon is added when there is something to say and removed when the notification is clicked or has timed out (and, whatever happens, after 15 seconds).
- **A click** runs the handler of `onNotificationClick` with the id (`""` if none was given); for a notification the page asked for it also reaches that window's page as the event `notification.click {id}`.
- **Whether it is shown at all is the system's decision:** Focus Assist / Do Not Disturb, the user's notification settings, and a full-screen program can all hide it, and then no click will ever come. A program must not rely on a
  notification being seen.
- **Not done:** action buttons, images, long-lived toasts that can be clicked after the program has ended (that needs a registered application identity, so an installer), a sound of your own, other systems (`notify` returns `unsupported: ...`).
- **Test:** `tests/notify_main.clg` (33 checks with a double: the rules, the rate limit, the page command, clicks) and `tests/run_notify_window.sh` (a real program: the shell takes the notification and shows the icon while it is up, a click reaches the program and the page and the icon goes,
  an unclicked notification's icon goes away; two controls). It cannot see the toast on the screen, and it posts the message the shell sends for a balloon click.

## Light and dark: the system's colour scheme

```
string now = app.systemTheme();                        // "light", "dark" or "unknown"
app.setTheme("system");                                // follow the user's choice (the title bar, and the page's prefers-color-scheme) -- or "light" / "dark" to stay as they are
app.enableThemeCommands(true);                         // the page may ask: theme.get {} and, with true, theme.set {mode}
```

```js
const { mode, system } = await cling.theme.get();      // mode: "system" | "light" | "dark"; system: "light" | "dark" | "unknown"
cling.theme.onChange(({ mode, system }) => { /* the choice or the system changed */ });
await cling.theme.set('dark');                         // only if the application allowed it
```

- **Without any of this** a window already follows the system: its page (the browser engine's `prefers-color-scheme`) and its title bar (the window library). What this adds is the application knowing, a **forced** light or dark
  for the title bar and the page, going back to "system", and the event `theme.changed {mode, system}` for pages that draw their own colours (CSS alone needs no event).
- **Once `setTheme` was called**, a change of the user's scheme in Windows reaches the program as one event per change (Windows sends the notice several times), and a forced scheme is given back to the title bar after it.
- **The page's scheme is the browser engine's setting for the whole program** (all windows share one profile): with several windows the page of all of them follows the last choice. The title bar is per window.
- **A page can choose only among the three words;** it cannot make a window look like anything else.
- **Not done:** other systems (`systemTheme` is "unknown", `setTheme` changes nothing), accent colour, high-contrast mode, a handler in CeeLing for a change (the event goes to the pages; read `systemTheme()` when you need it).
- **Test:** `tests/system_main.clg` (the rules, the events, the commands, with a double) and `tests/run_system_window.sh` (a real window: the page's media query and, from Windows itself, the title bar's dark flag, for forced dark, forced
  light and "system"; the system's change is played by flipping the registry value Windows keeps and sending only this window the notice, and the value is put back, also if the test is cut short; three controls). It does not
  change the user's setting for other programs and it does not see the pixels of the title bar.

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

## Menus a page may ask for

```
Menu ctx = new Menu();
ctx.item("ctx.copy", "&Copy", "");
ctx.item("ctx.paste", "&Paste", "");
app.allowPopup("ctx", ctx);            // registers the menu under a name (at most 16)
app.enablePopupCommand();              // off until this is called
```

```js
document.addEventListener('contextmenu', function (e) {
  e.preventDefault();
  cling.menu.popup('ctx').then(function (id) { /* "ctx.copy", or null if the menu was dismissed */ });
});
```

The page can ask for a menu **by name** and gets the id of the item chosen; it cannot send items, labels or shortcuts (anything but the name is ignored), and a name the application did not register is `unknown_menu`.
The chosen item also runs the handler of `onMenu`. The menu opens at the mouse. Test: `menu_main` (13 checks) and the real window in `tests/run_menu_window.sh` (a right click in the page, Down and Enter).

## The program's icon

```
ICON=app.ico bash scripts/build.sh main.clg out.exe          # a project made by scripts/new_project.sh already has icon.ico and uses it
python3 scripts/make_icon.py app.ico my-app [RRGGBB]         # a placeholder: a coloured rounded square with a ring, 16 / 32 / 48 px, the colour from the name
```

- **Where it shows (Windows):** in Explorer (the icon is compiled into the exe as resource 1), in the window's title bar, in Alt+Tab and on the taskbar (the window takes the same resource, big and small).
  Without `ICON` the system's default icon stays. The build refuses a file that is not an `.ico` and one that does not exist.
- **Replace the placeholder** with a real `.ico` that has several sizes (16, 24, 32, 48, 256). The placeholder is a plain 32-bit image so that no tool is needed to make it.
- **Not done:** the tray icon (it will use the same resource), Linux and macOS icons, a version resource (the "Details" tab of the exe's properties).
- **Test:** `tests/run_icon.sh` builds with and without an icon and reads the colour in the middle of the icon back from the exe and from the running window, big and small; the build without an icon must not show it.

## Remembering where windows were

```
app.rememberWindows("my-app");        // before the first window is opened; windows then open where they were and the place is saved as the user moves them
```

- **What is kept:** per window, the restored rectangle (so a maximized window keeps the size it goes back to) and whether it was maximized, in `windows.json` in the application's config folder, next to `settings.json`.
  The first window is `main`, another window is named by the page it starts on; at most 16 names. The page never sees or changes these numbers (the file is outside the page's file gate).
- **When it is saved:** after the user finishes moving or resizing the window, and when it is maximized or restored. Not while a drag is going on, so one drag is one write.
- **Fitted to the screens that exist now** (`fitToMonitors` in `src/placement.clg`): never smaller than 200 x 150, never larger than the biggest monitor, wholly inside the work area (the screen without the taskbar) of the monitor
  it overlaps most, and centred on the first monitor if it overlaps none (the monitor it was on is unplugged). A window that spanned two monitors is pulled onto one.
- **Limits:** the window is created where the program asked and moved at once, so it can be seen to jump; a minimized window is not remembered as minimized. The numbers are in pixels of the screen
  they were saved on: a change of scaling between runs is not corrected for. A maximized window on a second monitor is remembered by its normal size and position, the maximized flag then maximizes it on whichever monitor that place is on.
- **Test:** `tests/run_placement_window.sh` moves, resizes, maximizes, restores and closes a real window from outside (`tests/window_poke.cpp`), starts the program again and checks that the window comes back; a place on
  no monitor and one far too big must come back fitted. A physical mouse drag is not simulated (the message that ends a drag is sent). With a control that must fail.

## One running copy of the program

Two copies writing the same files corrupt them. A program can say it wants one (Windows; elsewhere nothing is enforced and the call says so):

```
if (!app.singleInstance("my-app")) { return 0; }      // before the first window; another copy runs and was told -- end
std::vector<string> given = app.arguments();           // the arguments THIS process was started with (without the program name)
```

`app.claimInstance("my-app")` returns the reason: `0` this is the instance, `1` another one runs and was told, `2` another one holds the name but could not be
told (it may be starting or stuck), `3` not supported here, `4` the name is not a plain name (1 to 64 letters, digits, `.`, `_`, `-`).

What the running copy does when a later launch hands over its working folder and arguments:

```js
cling.onSecondInstance(function (e) { /* e = { cwd: 'C:\\work', args: ['report.txt', '--flag'] } */ });
```

- The first window comes to the front (a minimized one is restored first), and its page gets the event `app.secondInstance`.
- **The arguments are text from another process.** At most 64 of them, each plain text of at most 4096 bytes, the others are left out; nothing in them is allowed or opened by this.
  If an argument names a file, whether the page may reach it is the application's decision.
- How it is done (`shim/win/instance.cpp`): a named mutex in the user's session decides who is first; a later launch writes its folder and arguments to a named pipe whose name contains the user's SID,
  created with an access list for this user and SYSTEM only and refusing remote clients; a message is at most 64 KiB and at most 16 are kept before the first window exists.
- **Not covered:** two *users* are separate instances (the name is per user); a process that takes the name first without being this program stops it from starting (the mutex is a name, not a proof);
  the event goes to the first window, and if that one is closed while others stay open it is lost; on POSIX nothing is enforced yet. Whether the window really comes to the front
  (Windows may refuse to move a window to the front for a process that did not just receive input) is not checked by the tests: `tests/run_instance_window.sh` checks the whole
  story with two real processes and a real window except the focus itself.

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
  exit; there is no cancel for a running dialog; `Channel<T>` (below) lets a worker ask the main thread for something and wait for the answer.

## More than one window

One `App` is the application (commands, the file policy, programs, pumps); it can have any number of windows.

```
App app = new App();
app.openSite("Main", 900, 600, false, "http://app.localhost", "");     // the first window is the primary one: app.handle
int settings = app.openWindow("Settings", 480, 360, false, "/settings.html");   // another window of the same site; 0 if it could not open
Window w = app.windows.get(settings);                                   // a Window: handle, emit, close, requestClose, setTitle, confirmClose,
                                                                       // allowNavigation, allowOnlyCommands, its own watches
app.run();                                                              // serves every window; returns when the LAST one has closed
```
- **One site, one origin.** The program's embedded web files are served under one origin; each window starts on its own page of it
  (`openWindow(..., "/settings.html")`, `""` for the embedded page, or a development-server address). A second origin is not supported.
- **Per window:** the page's calls, the command list (`Window.allowOnlyCommands`; `App.allowOnlyCommands` is the list of every window without
  one), the watches (32 each; one window cannot stop another's), the close button's state (`window.setDirty`, `confirmClose`), the events (`Window.emit`;
  `App.emit` reaches all). `window.*` commands act on the window that called them.
- **A call id is only unique within its window.** Two pages may have a call "1" in flight at the same time, so everything keyed by a call id is
  keyed by window as well: `app.later(handle, id, l)` (tauriport passes the `handle` its async command was given), cancel tags, deadlines.
  A synchronous command that needs to know its window reads `app.dispatcher.callerHandle`; an async command is given `handle`.
- **Closing.** Closing one window cancels the work it started (programs killed, `callCancelled` true), forgets its pending calls and, if it was the
  primary, the oldest remaining window becomes the primary (`app.handle`). The program ends when the last window closes (`app.quit()` closes
  them all). `Window.requestClose()` closes as the X does (a window with unsaved work asks first), `Window.close()` at once.
- **Opening is slow for everybody**: `openWindow` runs on the thread that serves the windows and waits for WebView2 to start (a few hundred
  milliseconds), so the other windows' calls queue meanwhile.
- **A page may open a window only if the program says so.** `app.enableWindowOpenCommand(max)` registers `window.open {start, title?, width?, height?}`:
  `start` must be a path inside the program's own site (plain text, no `..`, no scheme), at most `max` windows are open at once, and the new window
  gets the opener's own command list. If it cannot open, the opener receives the event `window.openFailed {start}`. Without that call (the default) a page
  cannot open windows, and `window.open()` in the page is cancelled by the navigation guard either way.
- Tests: 42 headless checks and 9 on the site (the same call id in three windows, tags, laters, command lists, watches, events, closing one by one,
  a page opening a window), a real three-window program (`tests/run_multiwindow.sh`: a page opens the third window, one is closed the way the X closes it,
  with a control that removes the `WM_DESTROY` fix and must fail), and a wait-any stress test under ASan/UBSan/TSan with a control that removes the
  wake-up notification.

## Workers that need the main thread: Channel and pumps

State such as the file policy belongs to the main thread, so a worker cannot read a file itself. `Channel<T>` (a CeeLing type; see CeeLing's
CLAUDE.md) is a queue of plain values that threads share: a worker sends a request on one channel and waits for the answer on another that
travelled with the request. The main loop sleeps in its own queue, so after sending the worker calls `wakeMain(handle, "name")` and the loop
runs the pump registered with `app.onPump("name", closure Pump [channel] (FileGate gate) { ... })`:

```
struct FileAsk { string path; Channel<string> reply; FileAsk(string p, Channel<string> r) { path = p; reply = r; } }

Channel<FileAsk> toMain;
app.onPump("files", closure Pump [toMain] (FileGate gate) {
    while (toMain.wait(0)) {                       // poll; a pump must never wait
        FileAsk ask = toMain.take();
        ask.reply.send(gate.read(ask.path, 1048576).text);
    }
});
// in an async command, inside spawn(closure Job [asks, path, id, handle] () {...}):
Channel<string> reply;
asks.send(new FileAsk(path, reply));
wakeMain(handle, "files");
if (reply.wait(5000)) { replyOk(handle, id, "\"" + reply.take() + "\""); }
```
Limits to know: a pump runs on the thread that serves the window, so it must not wait or throw (an error has no page call to reject and is dropped);
`send` never blocks and fails when the channel holds 1024 values; a pool worker that sits in `wait` takes one of the pool's threads
(`cling::setWorkerLimit`), so give every `wait` a timeout. Nothing here is "thread-safe" in general: what is checked is that threads share only
channels, which carry copies of plain values. The headless test reads a file through a pump and runs a producer thread against the consumer
under TSan, with a control that removes the channel's lock and must be reported.

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
- **Arguments into a struct.** A command with several fields can read them with one call: declare a `struct` with a constructor that
  takes its fields in declaration order and write `Args a = decodeArgs(args);`. There is no `decodeArgs` function in your program; the
  compiler writes it (see CeeLing's `struct_json`). Fields may be `int`, `int64`, `double`, `float`, `bool`, `string`, `byte`, vectors of
  those, structs and vectors of structs. A missing field or one of the wrong type is refused as `invalid_argument` naming the field
  (`spot.y`, `tags[1]`); an `int` must be a whole number in range; extra keys are ignored. It checks shape, not meaning: lengths, paths and
  names still go through your own checks.
- **Development server.** While the front end is being written, run its dev server (Vite, webpack, `python -m http.server`) and start the
  program with `APP_DEV_URL=http://localhost:5173/` (`app.devServerFromEnvironment()` reads it, `app.openDevServer(title, w, h, url)`
  opens it): the window loads that server, the bridge is injected into every document (so it survives a reload), the developer tools are
  on, and the server's own hot reload works as usual. Only `http` on `localhost`, `127.0.0.1` or `[::1]` is accepted (a page from
  anywhere else must never get the commands), and the window may navigate within that origin only. A `RELEASE=1` build refuses it
  altogether. `tests/run_dev_window.sh` runs a real window against a local server and checks that the release build refuses.
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

bash tests/run_headless.sh            # ~20 s: 295 end-to-end + 147 file policy and text + 54 folders and settings + 49 links and shell + 19 single instance + 41 window places + 100 menus, tray and pop-ups + 33 notifications + 32 layout + 18 program lookup + 38 site + 50 tauriport (needs lore) + 29 process + 27 watcher + 18 two-process single instance, no window (SLOW=1: build at -O1)
bash tests/run_distribution.sh       # release build: windowed program, version and icon in the exe, the runtime check with a box, bad version values refused
bash tests/run_signing.sh            # signing with a throwaway certificate: signed, runs, a changed file is caught, refusals
bash tests/run_layout_window.sh       # a real window: full screen, always on top, no title bar, fixed size, a place fitted to the monitors, a modal window; two controls that must fail
bash tests/run_notify_window.sh       # a real program: the shell takes a notification, a click reaches the program and the page, the icon goes away; two controls
bash tests/run_system_window.sh       # a real window: forced and followed light/dark (page and title bar), a played change of the system, files dragged onto it; four controls
bash tests/run_tray_window.sh         # a real program: the tray icon, hiding on close, the tray menu; two controls that must fail
bash tests/run_menu_window.sh         # a real window: the menu bar, choosing items, shortcuts, greyed items, ticks; two controls that must fail
bash tests/run_icon.sh                # the icon is in the exe and on the running window; the build without one is the control
bash tests/run_placement_window.sh    # a real window is moved, maximized and closed from outside; the next start must open it there; with a control that must fail
bash tests/run_instance_window.sh     # two real processes, one window: the second hands over its arguments and ends; with a control that must fail
bash tests/run_window_selftest.sh     # real WebView2 window, page -> commands -> checks -> exit 0
bash tests/run_sanitizers.sh          # ~80 s, WSL: ASan+UBSan, TSan on the queue, the watcher and the CeeLing tests, lock-removal controls
bash tests/negative_controls.sh       # ~65 s: removes one check at a time from src/ and shows the tests fail
bash tests/run_tauriport_window.sh     # the tauriport port in a real window, against the real lore tool
bash tests/run_tauriport_frontend.sh   # tauriport's own compiled front end, served from memory, in a real window
```

Requirements: the CeeLing compiler, cjson-cling, a C++ compiler (MinGW-w64 on Windows), the WebView2 runtime for window runs, WSL with
g++ for the sanitizers. Windows is the only platform tried. The scripts find the tools themselves when CeeLing, cjson-cling and app-cling
sit side by side (`../CeeLing/build/cling.exe`, `../cjson-cling`) and `g++` is on `PATH`; otherwise set `CLING`, `CJSON`, `CXX`
(`scripts/env.sh` says what is missing). `WSL_DISTRO` picks the WSL distribution for the sanitizer run (default: WSL's default);
`TMPDIR` is where the test scripts work.

**A new project:** `bash scripts/new_project.sh ../my-app` makes a folder with a page, a `main.clg` with one command, a `build.sh`
(`--release`, `--headless`) and a README; `cd ../my-app && bash build.sh` builds `out/my-app.exe`. `tests/run_template.sh` checks that
a fresh project builds in all three ways and its window starts.

`third_party/` is not committed; `fetch.sh` recreates it. The window build links statically. Sizes of the hello example: 4.3 MB before, 4.1 MB with unused code dropped (`--gc-sections`, now always on), 1.6 MB with `RELEASE=1` (which also strips the symbols; `STRIP=1` strips without the other release switches). A stripped program has no function names for a debugger or a backtrace, so keep a plain build for debugging.

## Layout

| path | what |
|---|---|
| `src/text.clg` | UTF-8 checking and repair, splitting, small parsers |
| `src/core.clg` | replies and structured errors, events to the page, argument helpers, `Command` / `AsyncCommand` / `Job` |
| `src/files.clg` | `FileGate`: the allowed-folder policy and every file operation, on top of the native open-and-ask primitives |
| `src/assets.clg` | the program's own web files: types, names, page aliases, `publishSite` |
| `src/menu.clg` | menus: `Menu` (the rules for ids, labels, shortcuts) and the flat spec the native layer builds a menu from |
| `scripts/sign.ps1`, `scripts/check_signature.ps1` | sign an exe with a certificate file or a certificate in the store; say what the signature of an exe is |
| `docs/distribution.md` | giving the program to someone else: the runtime check, the release build, signing, a checklist |
| `scripts/make_icon.py` | a placeholder `.ico` (the build option `ICON=`) |
| `src/theme.clg` | the colour scheme: `ThemeState` (system / light / dark), its event and commands |
| `src/drops.clg` | files dragged onto a window: `DropRules` (the grants, the limit) |
| `src/placement.clg` | where windows were: `WindowMemory` and `fitToMonitors` |
| `shim/win/instance.cpp` | one running copy per user: the mutex, the pipe and the hand-over of a later launch's arguments |
| `src/links.clg` | what a page may ask the system to open: addresses, files, show in the file manager |
| `src/settings.clg` | the per-user folders of an application and `SettingsStore`, its declared, typed settings file |
| `src/app.clg` | `App`: the command table and the commands (files, programs, dialogs, watches, window), `serveOne` and `run`; the pieces below are included, not written here |
| `src/dispatch.clg` | `CallContext`, `CommandRules` and `Dispatcher`: a request in, a reply out, structured errors |
| `src/fscommands.clg` | the file commands' helpers: error codes, entry shapes, extension and name checks |
| `src/dialogs.clg` | the dialog arguments (`DialogArgs`, `MessageArgs`, `FilterArgs`) and their checks; `readDialogArgs`, `readMessageArgs` |
| `src/notify.clg` | `NotifyArgs`, `Notifier`; `readNotifyArgs` |
| `src/programs.clg` | the registered programs, their slots, lookup and `runTool` |
| `src/later.clg` | answers that come later (`Later`, `LaterBook`) and work only the main thread may do (`Pump`, `PumpBook`, `wakeMain`) |
| `src/watches.clg` | `Watches`: folder watches, event merging and the permission re-check before delivery |
| `src/windows.clg` | `Window`, `WindowBook`, `PopupBook`, `OpenWindowArgs`; `readOpenWindowArgs` |
| `scripts/modules.sh` | the list of these modules, in one place for the build, the sanitizers, WSL runs and the statistics |
| `scripts/pointer_demands.sh` | what the modules need from each other's classes (see "One program, several files" in `docs/ceeling-in-app-cling.md`) |
| `scripts/ptr_table.py` | compares the pointer kinds and function bodies of two builds (used to check that a refactor did not change what the compiler inferred) |
| `src/bridge.js` | defines `window.cling.*`; embedded into the program at build time |
| `shim/` | the native layer: `app_native.h` lists every primitive; `app_shim.cpp` window and queue; `win/` and `posix/` the OS parts |
| `docs/ceeling-in-app-cling.md` | what the language and its compiler do in this program, with numbers (`scripts/ceeling_stats.sh`, `scripts/bench_checks.sh` for the cost of the run-time checks), what they found and where they got in the way |
| `docs/command-journey.md` | one command (`fs.read`) from the page to the reply, with the generated C++ beside the CeeLing |
| `docs/compile-errors.md` | the compile errors you will meet, what each meant, and the fix |
| `examples/hello/` | window with three buttons |
| `tests/` | headless suites, real-window self-test, queue stress test, sanitizer and negative-control scripts; `tests/doubles/` stands in for the window and the dialogs |

## License

MIT, see `LICENSE`. webview and WebView2 keep their own licenses and are downloaded, not redistributed.
