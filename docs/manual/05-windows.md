# 5. Windows

Title, size, position, full screen, several windows, remembering places, one running copy, the icon.

## The window

```
app.enableWindowCommands();
app.confirmClose("Unsaved work", "Close without saving?");   // optional, see below
```
```js
await pronao.window.setTitle({ title: 'Report - draft' });
await pronao.window.setSize({ width: 900, height: 600 });     // 200..10000 by 150..10000
await pronao.window.minimize();  await pronao.window.maximize();  await pronao.window.restore();  await pronao.window.focus();
await pronao.window.setDirty({ dirty: true });                  // the page has unsaved work
await pronao.window.close();
pronao.window.onCloseDeclined(() => { ... });                   // the user pressed the X, was asked, and chose to stay
```
With `confirmClose`, pressing the window's X (or Alt+F4) no longer closes the window by itself: the native layer only queues a
record. If the page has said it has unsaved work (`setDirty`), CeeLing asks with a native box whose text is the
application's, never the page's, and closes only on "Yes"; with nothing unsaved the window just closes. `window.close` is the
page closing its own window and is not asked about. Title and size values are checked in CeeLing before they reach the
window. Windows and Linux (appendix D); the real-window self-test presses the X, answers the box and checks the title, size, maximize
and restore.

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
const b = await pronao.layout.getBounds();                  // {x, y, width, height, maximized, minimized, fullscreen, alwaysOnTop, frameless, resizable}
await pronao.layout.setBounds({ x: 100, y: 80, width: 900, height: 600 });     // fitted, as above
// a window with no title bar: the page draws its own and starts the system's move or resize from a mouse down on it
titlebar.onmousedown = function () { pronao.layout.startDrag(); };
grip.onmousedown = function () { pronao.layout.startResize('bottomright'); };     // left, right, top, bottom, topleft, topright, bottomleft, bottomright
await pronao.layout.setFullscreen(true);                     // only if the application allowed full screen
await pronao.layout.setAlwaysOnTop(true);                    // only if the application allowed always on top
await pronao.invoke('window.open', { start: '/dialog.html', modal: true });    // a window the page opens that disables the page that opened it
```

- **What the page cannot do:** take away its own title bar or resize border (`setFrameless` / `setResizable` are the application's), cover the screen or stay in front unless the application said it may (a page that can do both can imitate the system's
  own windows), or start a move out of nothing: `startDrag` / `startResize` act only while the left mouse button is held down.
- **A frameless window and the page's own title bar:** the buttons are the page's: `pronao.window.minimize()`, `maximize()`, `close()` already exist. Windows' snap layouts and the shadow of a window with a title bar are not kept.
- **Numbers the page sends** are checked: `x`, `y` within -32000 to 32000, width 200 to 10000, height 150 to 10000.
- **Test:** `tests/layout_main.clg` (32 checks with a double) and `tests/run_layout_window.sh` (a real window; after each change the test asks Windows what the window looks like: the rectangle, topmost, caption, resize border, enabled, whether it covers its
  monitor; a drag with no button held does nothing; a modal window disables its owner and gives it back when the application closes it and when the user does; two controls that must fail).
- **Not done:** transparent windows, owned (non-modal) child windows that stay on top of their parent and minimize with it, a window bigger than one monitor on purpose (full screen across all monitors), more than one real monitor (the fitting is tested with a double; this
  machine has one), dragging and resizing with a real mouse button (the test checks the guard, not the drag).

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
  keyed by window as well: `app.later(handle, id, l)` (an async command is given its `handle`), cancel tags, deadlines.
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
pronao.onSecondInstance(function (e) { /* e = { cwd: 'C:\\work', args: ['report.txt', '--flag'] } */ });
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

## The program's icon

```
ICON=app.ico bash scripts/build.sh main.clg out.exe          # a project made by scripts/new_project.sh already has icon.ico and uses it
python3 scripts/make_icon.py app.ico my-app [RRGGBB]         # a placeholder: a coloured rounded square with a ring, 16 / 32 / 48 px, the colour from the name
```

- **Where it shows (Windows):** in Explorer (the icon is compiled into the exe as resource 1), in the window's title bar, in Alt+Tab and on the taskbar (the window takes the same resource, big and small).
  Without `ICON` the system's default icon stays. The build refuses a file that is not an `.ico` and one that does not exist.
- **Replace the placeholder** with a real `.ico` that has several sizes (16, 24, 32, 48, 256). The placeholder is a plain 32-bit image so that no tool is needed to make it.
- **On Linux** `ICON=<file.png>` is compiled in as bytes and used for the window (the title bar and the task switcher); a desktop shows an installed program's icon from its `.desktop` file, which is the packager's. `ICON` must be a PNG there.
- **Not done:** the tray icon (it will use the same resource), macOS icons, a version resource (the "Details" tab of the exe's properties).
- **Test:** `tests/run_icon.sh` builds with and without an icon and reads the colour in the middle of the icon back from the exe and from the running window, big and small; the build without an icon must not show it.
