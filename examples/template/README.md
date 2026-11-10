# __NAME__

An [app-cling](__APP_CLING__) program: the page is `index.html`, the commands it calls are written in CeeLing in `main.clg`.

```
bash build.sh               # out/__NAME__(.exe)
bash build.sh --release     # same, with the developer tools and the development-server switch compiled out
bash build.sh --headless    # no window; only checks that it transpiles and compiles
```

`icon.ico` is a placeholder (a coloured square made from the project's name): replace it with your own icon, a Windows `.ico` with several sizes.
It is compiled into the exe and shown by Explorer, the title bar and the taskbar.

While writing the front end, run its dev server and start the program with `APP_DEV_URL=http://localhost:5173/ out/__NAME__`.
`APP_DEBUG=1` turns the developer tools on.

Add a command in `main.clg` with `app.command("name", closure Command [] (Item args) { ... });` and call it from the page with
`cling.invoke("name", args)`. What a page may do beyond that (files, programs, dialogs, watching folders, window control) is off until
`main.clg` turns it on; see app-cling's user manual (`docs/manual/`).
