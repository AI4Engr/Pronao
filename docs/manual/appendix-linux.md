# Appendix D: Pronao on Linux

The window layer for Linux uses GTK 3 and WebKitGTK from the system. It is a first version, written and tested under WSL with WSLg (X11 through `GDK_BACKEND=x11`),
**not yet tried on a real Linux desktop**; the first run on one is the test that counts. Everything below says what was run and what was not.

## Build and run

```bash
sudo apt install g++ cmake pkg-config zlib1g-dev libgtk-3-dev libwebkit2gtk-4.1-dev     # (-4.0-dev also works)
bash scripts/setup.sh          # clones CeeLing and cJSON-cling beside this repository, builds the compiler, fetches the webview library
bash scripts/build.sh src/main.clg out/app               # (or any program of yours; see chapter 9)
```

Nothing else changes in a program's source. The program needs `libwebkit2gtk-4.1` (or `-4.0`) and GTK 3 on the machine that runs it: they are not linked in, so
a program for people has to say so (a `.deb` depends on them, an AppImage or Flatpak carries them). Under Wayland run it as usual; the notes below say where X11 matters.

## What works, and how it was checked

| Part | On Linux | Test (all under WSLg, each with a control that must fail) |
|---|---|---|
| Window, page, commands, events | yes | `tests/run_window_selftest_linux.sh`: the same page and checks as Windows (`selftest ok`) |
| Site served from memory (`openSite`) | yes, under `pronao://<host>/` (see below) | same script, second program (`tests/site_window.clg`) |
| Navigation guard, `window.open`, new windows | yes (WebKitGTK `decide-policy` and `create`) | same script |
| Native dialogs (open, save, folder, message boxes) | yes (GTK file chooser and message dialog) | same script: a message box and an Open dialog are shown and closed from outside |
| Close button asks (`confirmClose`) | yes | same script |
| Window control: minimize, maximize, hide, full screen, always on top, no frame, fixed size, modal windows, monitors, position | yes; a window manager may refuse a request | self-test (state, size); not every call has its own test |
| Remembered window place | yes, size always; position only on X11 | `tests/run_placement_linux.sh`: three starts in a row, no creep; a place on no monitor is brought back |
| Menu bar, shortcuts, the page's pop-up menu | yes (GTK menu bar; the page is told its size under it) | `tests/run_menu_linux.sh` |
| One running copy of a program | yes (a lock file and a unix socket under `$XDG_RUNTIME_DIR/pronao`, mode 0700, same user only) | `tests/run_instance_linux.sh` |
| Dropped files | yes (paths from the desktop's own drag data) | `tests/run_system_linux.sh`: a real drag between two windows with `tests/xdrag.c` |
| Light / dark scheme (`setTheme`) | forcing works and the page sees it | `tests/run_system_linux.sh` |
| Notifications | yes, through `org.freedesktop.Notifications` on the session bus (no tray icon) | `tests/run_notify_linux.sh`, against a stand-in service (`tests/notify_mock.py`) |
| Files, programs, folder watching, links, HTTP (E22) | yes (earlier; HTTP answers `unsupported`) | headless suite and sanitizers |
| Program icon | yes: `ICON=<file.png>` puts it in the window | built and the window's `_NET_WM_ICON` read back |

## What is not there

- **Tray icon and tray menu.** `openTray` returns false. There is no tray API common to Linux desktops.
- **HTTP from the backend** (`http.request`): the Windows implementation uses WinHTTP; the Linux file answers `unsupported`.
- **Following the desktop's own scheme change while the program runs.** The code listens for GTK's theme settings, but no test plays a change of the desktop's scheme.
- **Maximize and restore of the remembered place** was not run by a test (no window-manager tool is assumed); the code path is the same as `setMenu`'s and the self-test's.
- **Wayland.** A program cannot ask where its window is or put it somewhere under Wayland, so a saved position is meaningless there (the size is kept); `GDK_BACKEND=x11` makes
  it work as on X11 through XWayland. `startDrag` (a custom title bar) needs the compositor's help and works only while the button is down.
- **Tiling window managers** ignore moving and resizing requests.
- **Clicking a menu title with a synthetic mouse** did not open the menu under WSLg, so the test opens it with the keyboard (Alt and the mnemonic). A person with a mouse is the check that is missing.
- **Look of the menu and dialogs**, high-DPI drawing, a screen reader.
- **The code is trusted, not checked**: `shim/posix/` is C++ like `shim/win/` (see the README's platform table for its size).

## Differences a page can see

- **The origin.** WebKitGTK cannot intercept an `http://` address, so a site served with `openSite("...", "http://app.localhost")` is shown under `pronao://app.localhost/`.
  Relative addresses, the Content-Security-Policy (`'self'` is the same origin), `fetch` of your own files and storage work as on Windows; an **absolute** `http://app.localhost/...`
  address written in a page does not. The address bar of a window is not shown, so nothing else changes. `allowNavigation` and the guard use the same mapping.
- **Drag of files** reaches the page as a drag of `text/uri-list` (WebKitGTK lists no `Files` type); `bridge.js` accepts that, and the program gets the real paths from GTK.
  The first `drop.enter` reports `x: 0, y: 0`.
- **Context menu** (right click) is off in a release build, as on Windows.

## Two workarounds worth knowing

- WebKitGTK 2.50 aborts the process when its context is released by the C library's exit handlers (found with AddressSanitizer). The shim holds one extra reference to the context
  for the life of the process. Every run used to end with `SIGABRT` after the program had printed its result.
- `gtk_check_menu_item_set_active` activates the item, which would be told to the application as a click. The shim marks its own changes of a tick and does not report them.

## Running the Linux tests

```bash
bash tests/run_headless.sh                   # no window
bash tests/run_window_selftest_linux.sh      # needs a display (X11, or WSLg: GDK_BACKEND=x11), xdotool, gcc, libx11-dev
bash tests/run_menu_linux.sh
bash tests/run_placement_linux.sh
bash tests/run_system_linux.sh
bash tests/run_instance_linux.sh
bash tests/run_notify_linux.sh               # also needs dbus-run-session and python3 with PyGObject
```
