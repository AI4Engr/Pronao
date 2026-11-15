# __NAME__

An [Pronao](__PRONAO__) program: the page is `index.html`, the commands it calls are written in CeeLing in `main.clg`.

With the `pronao` command (in the Pronao folder) the same things are `pronao build`, `pronao build --release`, `pronao dev`, `pronao test` and `pronao package`; see the user manual, chapter 1.

```
bash build.sh               # out/__NAME__(.exe)
bash build.sh --release     # same, with the developer tools and the development-server switch compiled out
bash build.sh --headless    # no window; only checks that it transpiles and compiles
```

`icon.ico` and `icon.png` are placeholders (a coloured square made from the project's name): replace them with your own icons, a Windows `.ico` with several sizes and a Linux `.png` of 48 x 48 or more.
The `.ico` is compiled into the exe and shown by Explorer, the title bar and the taskbar; on Linux the `.png` is the window's icon (and the tray icon, and the launcher icon of a `.deb`).

While writing the front end, run its dev server and start the program with `APP_DEV_URL=http://localhost:5173/ out/__NAME__`.
`APP_DEBUG=1` turns the developer tools on.

Add a command in `main.clg` with `app.command("name", closure Command [] (Item args) { ... });` and call it from the page with
`pronao.invoke("name", args)`. What a page may do beyond that (files, programs, dialogs, watching folders, window control) is off until
`main.clg` turns it on; see Pronao's user manual (`docs/manual/`).
