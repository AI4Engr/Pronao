# 6. Menus, tray, notifications and theme

The menu bar, the tray icon, pop-up menus, system notifications and light or dark.

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
