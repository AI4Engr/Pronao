# windows

What a page may do with its window, and what only the program decides. Manual chapter 5 ([docs/manual/05-windows.md](../../docs/manual/05-windows.md)).

- Page: `window.setTitle`, `window.setSize` (200..10000 by 150..10000), `window.setDirty`, `layout.getBounds`, and `window.open` for a second window of the program's own site.
- Program: `confirmClose` asks "Close without saving?" when the X is pressed after `setDirty(true)`; `Window.setBounds` places the first window; `Window.setResizable` is behind the program's own command `app.lockSize`.
- A second window needs a site, so the program uses `openSite` and the pages are the files of `dist/` (`ASSETS`), not `PAGE` with `openPage`.

Run (needs the WebView2 runtime):

    ASSETS=examples/windows/dist EXTRA=examples/windows/commands.clg bash scripts/build.sh examples/windows/main.clg out/windows.exe
