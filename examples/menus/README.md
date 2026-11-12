# menus

A menu bar with shortcuts, a pop-up menu the page may ask for, a notification and the colour scheme. Manual chapter 6 ([docs/manual/06-menus-tray-notifications.md](../../docs/manual/06-menus-tray-notifications.md)).

- Program: `Menu` / `setMenu` (File: Save Ctrl+S, Quit Ctrl+Q; View: a ticked Grid on F5, and Reset, which unticks it from `onMenu` with `setChecked`), `menuEventsToPage`, `allowPopup` + `enablePopupCommand`, `enableNotifications`, `enableNotificationCommand`, `enableThemeCommands`.
- Page: `pronao.menu.popup('ctx')` (right click), `pronao.notify`, `pronao.theme.get / set / onChange`. A menu name the program did not register is refused.
- A tray icon (`openTray`, `onTray`) with Show and Quit; a program with a tray icon notifies from it, so `enableNotifications` then changes nothing.

Run (needs the WebView2 runtime):

    PAGE=examples/menus/index.html EXTRA=examples/menus/commands.clg bash scripts/build.sh examples/menus/main.clg out/menus.exe
