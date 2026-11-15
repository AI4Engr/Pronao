# Appendix D: Pronao on Linux

The window layer for Linux uses GTK 3 and WebKitGTK from the system. It is a first version, tested in three kinds of place: under WSL with WSLg (Ubuntu 22.04, WebKitGTK 2.50),
on a clean Ubuntu 26.04 server with a virtual X display (Xvfb) and the openbox window manager (WebKitGTK 2.52; `scripts/setup.sh` was run there from a fresh clone), and
under a headless Wayland compositor (weston) for a smoke test. **None of these is a full desktop session with a person at it**: the look of the menus, dialogs and tray icon on a real
KDE, GNOME or XFCE is the check that is missing. Everything below says what was run and what was not.

## Build and run

```bash
sudo apt install g++ cmake pkg-config zlib1g-dev libgtk-3-dev libwebkit2gtk-4.1-dev     # (-4.0-dev also works, without backend HTTP)
bash scripts/setup.sh          # clones CeeLing and cJSON-cling beside this repository, builds the compiler, fetches the webview library
bash scripts/build.sh src/main.clg out/app               # (or any program of yours; see chapter 9)
```

Nothing else changes in a program's source. The program needs GTK 3 and `libwebkit2gtk-4.1` (or `-4.0`) on the machine that runs it: they are not linked in, so a program for people has
to say so. `scripts/package_linux.sh` does that in a `.deb` (below).

## What works, and how it was checked

Every script below builds the program, drives it from outside, and has a control (a copy of the shim with one rule removed) that must make it fail.

| Part | On Linux | Test |
|---|---|---|
| Window, page, commands, events | yes | `run_window_selftest_linux.sh`: the same page and checks as Windows (`selftest ok`) |
| Site served from memory (`openSite`) | yes, under `pronao://<host>/` (see below) | same script, second program (`tests/site_window.clg`) |
| Navigation guard, `window.open`, new windows | yes (WebKitGTK `decide-policy` and `create`) | same script |
| Native dialogs (open, save, folder, message boxes) | yes (GTK file chooser and message dialog) | same script: a message box and an Open dialog are shown and closed from outside |
| Close button asks (`confirmClose`) | yes | same script |
| Window control: minimize, maximize, hide, full screen, always on top, no frame, fixed size, modal windows, monitors, position | yes; a window manager may refuse a request | self-test, placement test; not every call has its own test |
| Remembered window place | yes; size always, position on X11 only | `run_placement_linux.sh`: three starts, no creep; maximized and restored (needs `wmctrl`); a place on no monitor is brought back |
| Menu bar, shortcuts, the page's pop-up menu | yes | `run_menu_linux.sh` |
| One running copy | yes (lock file and unix socket under `$XDG_RUNTIME_DIR/pronao`, mode 0700, same user only) | `run_instance_linux.sh` |
| Dropped files | yes (paths from the desktop's own drag data) | `run_system_linux.sh`: a real drag between two windows with `tests/xdrag.c` |
| Light / dark scheme (`setTheme`) and the desktop changing it | yes | `run_system_linux.sh`; with `XSETTINGSD=1` (and `xsettingsd`, on a display of its own) the desktop's own change is played and the page is told |
| Notifications | yes, through `org.freedesktop.Notifications` on the session bus | `run_notify_linux.sh`, against a stand-in service (`tests/notify_mock.py`) |
| Tray icon and menu | yes, a StatusNotifierItem with a dbusmenu | `run_tray_linux.sh`, against a stand-in panel (`tests/tray_mock.py`); by hand also against an independent host (see below) |
| Backend HTTP (`http.request`) | yes, over libsoup 3 | the 27 checks of `tests/http_shim_test.cpp` (run by `run_headless.sh`) against a loopback server; certificate failures by hand (below) |
| Files, programs, folder watching, links | yes | headless suite and sanitizers |
| Program icon | yes: `ICON=<file.png>` is the window's icon and the tray icon | built, and the window's `_NET_WM_ICON` read back |
| `.deb` | yes: `scripts/package_linux.sh` | `run_package_linux.sh` |
| Under Wayland | the site self-test and the menu-bar layout pass | `run_wayland_linux.sh` (weston, headless) |

## The tray

The icon is a StatusNotifierItem: it registers with `org.kde.StatusNotifierWatcher` on the session bus and offers its menu as `com.canonical.dbusmenu`. KDE, XFCE, Cinnamon, MATE, LXQt
and Budgie show it; GNOME shows it with the AppIndicator extension, which many distributions install by default. On a desktop that shows no tray there is no watcher on the bus,
and `openTray` returns 0: the program should stay usable without one (and may use `notify`, which does not need a tray). A left click on the icon is `"click"`; the protocol has no
double click. What the menu looks like is the desktop's.

Checked: the stand-in panel reads the declared menu (labels with their mnemonics, the separator, the tick), the tooltip and a 32 x 32 icon; choosing an item and clicking the icon reach the program;
the tick the program sets is what the panel then reads; a notification from the tray icon arrives at the service and its click comes back; Quit ends the program and its bus name leaves the bus
(a panel removes the icon on that); with no watcher the program is told. The stand-in is mine, so it can share my mistakes about the protocol: that is why the same program was also run, by
hand, against two independent programs, `status-notifier-watcher` and `gtk-sni-tray-standalone` (Debian packages): the icon appeared in its bar, a left click reached the program, a right click opened
the 5-item menu, and Down, Down, Return chose "Mute". Not seen: a real KDE or GNOME panel.

## Backend HTTP

`http.request` goes through libsoup 3 (the library WebKitGTK 4.1 itself uses): no redirect followed, no cookie kept, no credentials added, the system's proxy and certificate store. What the
page may ask is decided in checked CeeLing, as on Windows. By hand, with real servers: `https://example.com/` and Open-Meteo answered 200; `expired.badssl.com`, `self-signed.badssl.com`
and `wrong.host.badssl.com` all came back as a TLS failure (`tls_failed`); an unknown name as `connect_failed`. TLS is not in the suite (no certificate is kept in the repository). With WebKitGTK 4.0
(libsoup 2.4) the layer answers `unsupported`.

## Packaging

```bash
pronao package --version 1.0.0        # in a project: a release build, then dist/my-app_1.0.0_amd64.deb (uses the project's icon.png)
bash scripts/package_linux.sh out/app my-app 1.0.0 icon.png dist/      # what it calls, for a program that is not a project; APP_TITLE, APP_DESCRIPTION, APP_MAINTAINER, APP_CATEGORIES are read from the environment
sudo apt install ./dist/my-app_1.0.0_amd64.deb
```

The `.deb` holds the program (`/opt/my-app/my-app`, linked from `/usr/bin`), a launcher entry (`/usr/share/applications/my-app.desktop`, checked with `desktop-file-validate`) and the icon, and depends on
the packages that own the libraries the program was linked against (GTK 3, WebKitGTK, libsoup, GLib, zlib, ...), found with `readelf` and `dpkg -S`, so build on the distribution you package for. It was
built, installed with apt, started from the launcher name and removed on Ubuntu 26.04. Not done: an AppImage or a Flatpak (they would have to carry WebKitGTK), a signed repository, an update mechanism,
RPM.

## What is not there, or not tested

- **A real desktop session.** Everything above ran under WSLg, Xvfb + openbox or a headless compositor. The look of menus, dialogs and the tray icon, high-DPI drawing, a screen reader, and a tiling window
  manager (which ignores moving and resizing) were not tried.
- **Wayland input.** `xdotool` cannot drive a Wayland session, so dialogs, shortcuts, drops and placement were not run there. A Wayland program cannot ask where its window is or place it, so a saved
  position is meaningless there (the size is kept); `GDK_BACKEND=x11` makes it work as on X11 through XWayland.
- **Clicking a menu title with a synthetic mouse** did not open the menu under WSLg's compositor, so the test opens it with the keyboard (Alt and the mnemonic). Under openbox a synthetic click on a
  title, and on an item, did choose it (tried by hand, not in the script).
- **macOS** is not started: it needs WKWebView and a Mac to build and test on.
- **The code is trusted, not checked**: `shim/posix/` is C++ like `shim/win/`.

## Differences a page can see

- **The origin.** WebKitGTK cannot intercept an `http://` address, so a site served with `openSite("...", "http://app.localhost")` is shown under `pronao://app.localhost/`.
  Relative addresses, the Content-Security-Policy (`'self'` is the same origin), `fetch` of your own files and storage work as on Windows; an **absolute** `http://app.localhost/...`
  address written in a page does not. `allowNavigation` and the guard use the same mapping.
- **Drag of files** reaches the page as a drag of `text/uri-list` (WebKitGTK lists no `Files` type); `bridge.js` accepts that, and the program gets the real paths from GTK.
  The first `drop.enter` reports `x: 0, y: 0`.
- **Context menu** (right click) is off in a release build, as on Windows.

## Notes from testing

- A window that was resized and maximized before it was shown was restored by the window manager to 200 x 200 (its first size), not to the remembered one. The shim now maximizes 300 ms after it has
  applied the size and place (found on the second machine).
- Setting the dark preference made GTK call back into code that held the same lock: a deadlock (found by the system test).
- `xdotool` 3.2016 sent F5 *by name* with Alt held on a keymap where F5 appears at many levels, so the shortcut did not match. That was the test tool, not the program; the menu test sends F5 by its
  keycode (71).
- WebKitGTK 2.50 aborts the process when its context is released by the C library's exit handlers (found with AddressSanitizer). The shim holds one extra reference to the context for the life of the process.
- `gtk_check_menu_item_set_active` activates the item, which would be told to the application as a click. The shim marks its own changes of a tick and does not report them.

## Running the Linux tests

```bash
bash tests/run_headless.sh                   # no window
bash tests/run_window_selftest_linux.sh      # needs a display (X11, WSLg with GDK_BACKEND=x11, or Xvfb + a window manager such as openbox), xdotool, gcc, libx11-dev
bash tests/run_menu_linux.sh
bash tests/run_placement_linux.sh            # maximize is tested when wmctrl is installed
bash tests/run_system_linux.sh               # XSETTINGSD=1 also plays a change of the desktop's scheme (xsettingsd, a display of its own)
bash tests/run_instance_linux.sh
bash tests/run_notify_linux.sh               # also needs dbus-run-session and python3 with PyGObject
bash tests/run_tray_linux.sh                 # the same
bash tests/run_package_linux.sh              # needs dpkg-deb
bash tests/run_wayland_linux.sh              # needs weston; no display of yours is used
```
