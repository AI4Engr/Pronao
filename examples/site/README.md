# examples/site

A front end of several files served from memory: [manual chapter 9](../../docs/manual/09-web-ui-and-shipping.md). `dist/` holds `index.html`, `app.js`,
`style.css` and a second page, `about.html`; `ASSETS=` packs the folder into the program and `main.clg` opens it with
`app.openSite("site example", 800, 560, false, "http://app.localhost", "")`. The window loads `http://app.localhost/`, answered by the program and never
by the network: `/` is `index.html`, `/about` and `/about/` are `about.html`, an address the folder does not have is a 404. `commands.clg` registers the
two commands `app.js` calls (`greet`, `sum`).

```
ASSETS=examples/site/dist EXTRA=examples/site/commands.clg bash scripts/build.sh examples/site/main.clg out/site.exe
```

For someone else: the release build of [chapter 10](../../docs/manual/10-distribution.md) (the placeholder icon: `python scripts/make_icon.py app.ico`).

```
RELEASE=1 ICON=app.ico APP_VERSION=1.4.0 APP_NAME="My app" APP_COMPANY="Example Co" APP_DESCRIPTION="What it does" \
    ASSETS=examples/site/dist EXTRA=examples/site/commands.clg bash scripts/build.sh examples/site/main.clg out/site.exe
```

Measured on one machine: a `RELEASE=1` build is 2.0 MB (2,042,880 bytes, with a placeholder icon and version resource; 2,027,008 without both). The same program built without
`RELEASE=1` is 4.9 MB. Headless check (no window): `tests/examples/site_check.clg`, built with `ASSETS=examples/site/dist`.
