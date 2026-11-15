# 9. The web UI and giving the program away

Serving a front end of many files, the development server, and building a program for someone else.

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

Size: `scripts/pack_assets.sh` stores a file as a gzip stream when that makes it at least 10% smaller, and the program inflates it when the window asks for it
(`shim/assets.h`); images and other files that are already compressed are stored as they are. The window receives the file's own bytes and no
`Content-Encoding`, because WebView2 does not unpack an encoding on a response a program makes itself (the page received the gzip bytes when that was tried).
Measured on the draw.io editor (see `examples/drawio`): 54 MB of files made a 55.8 MB program before, and 22.6 MB with this (after also leaving out about 6 MB
of files that example does not use). A static Next.js export of about 120 files was 4 MB before this change; it was not measured again. This adds zlib to the native layer's
link line (`-lz`; MinGW-w64 and Linux have it), and each request for a compressed file inflates it again, with no cache. One site per process, on Windows and on Linux (there as a custom scheme, `pronao://<host>/`, because WebKitGTK cannot intercept `http`: see appendix D), no range requests, no streaming. The origin is yours to choose: a front end ported from Tauri may expect `http://tauri.localhost`,
because its server's CORS list already contains it.

## Giving the program to someone else

[chapter 10](10-distribution.md) has the whole page; the short version for a portable exe:

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

## Development server and release builds

- **Development server.** While the front end is being written, run its dev server (Vite, webpack, `python -m http.server`) and start the
  program with `APP_DEV_URL=http://localhost:5173/` (`app.devServerFromEnvironment()` reads it, `app.openDevServer(title, w, h, url)`
  opens it): the window loads that server, the bridge is injected into every document (so it survives a reload), the developer tools are
  on, and the server's own hot reload works as usual. Only `http` on `localhost`, `127.0.0.1` or `[::1]` is accepted (a page from
  anywhere else must never get the commands), and the window may navigate within that origin only. A `RELEASE=1` build refuses it
  altogether. `tests/run_dev_window.sh` runs a real window against a local server and checks that the release build refuses.
- **Developer tools off in release.** `RELEASE=1 scripts/build.sh ...` compiles the window with `-DAPP_RELEASE`: `open`'s `debug`
  argument is then ignored (no developer tools, no right-click menu) because the switch is not in the binary, not because a
  variable is unset. Without `RELEASE`, `debug` works as before.
