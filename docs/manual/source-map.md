# Source map

Where things are in the repository.

| path | what |
|---|---|
| `src/text.clg` | UTF-8 checking and repair, splitting, small parsers |
| `src/core.clg` | replies and structured errors, events to the page, argument helpers, `Command` / `AsyncCommand` / `Job` |
| `src/files.clg` | `FileGate`: the allowed-folder policy and every file operation, on top of the native open-and-ask primitives |
| `src/assets.clg` | the program's own web files: types, names, page aliases, `publishSite` |
| `src/menu.clg` | menus: `Menu` (the rules for ids, labels, shortcuts) and the flat spec the native layer builds a menu from |
| `scripts/sign.ps1`, `scripts/check_signature.ps1` | sign an exe with a certificate file or a certificate in the store; say what the signature of an exe is |
| `docs/manual/` | the user manual: ten chapters, the command journey and the compile errors |
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
| `scripts/pointer_demands.sh` | what the modules need from each other's classes (see "One program, many files" in chapter 16 of the CeeLing guide) |
| `scripts/ptr_table.py` | compares the pointer kinds and function bodies of two builds (used to check that a refactor did not change what the compiler inferred) |
| `src/bridge.js` | defines `window.pronao.*`; embedded into the program at build time |
| `shim/` | the native layer: `app_native.h` lists every primitive; `app_shim.cpp` window and queue; `win/` and `posix/` the OS parts |
| `examples/hello/` | window with three buttons |
| `tests/` | headless suites, real-window self-test, queue stress test, sanitizer and negative-control scripts; `tests/doubles/` stands in for the window and the dialogs |
