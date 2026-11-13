# drawio

The [draw.io](https://github.com/jgraph/drawio) diagram editor (diagrams.net) running in a Pronao window, as one `.exe`.

The editor is the web application, unmodified except for one `<script>` line added to its `index.html`. A page can read and write only the files the user picks in an open or save dialog (a pick is a grant for exactly that file); it can open `http` and `https` links in the browser; closing the window with unsaved changes asks first. There is no other file access, no program to run, no network from the program itself.

## Build

draw.io is not in this repository. `fetch.sh` downloads a pinned release (32.6.10), checks its SHA-256, drops what a desktop editor does not need, and adds the bridge `pronao-electron.js`:

```
bash examples/drawio/fetch.sh
RELEASE=1 ICON=docs/assets/pronao.ico ASSETS=examples/drawio/site bash scripts/build.sh examples/drawio/main.clg out/diagrams.exe
```

`fetch.sh` leaves `examples/drawio/site/` (about 59 MB, gitignored). The release build embeds it uncompressed, so the program is about 56 MB (measured on one machine; a Pronao program without a site is about 1 to 2 MB). The embedded files are what takes the space: the editor's script bundles are 21 MB, the shape libraries' icons 12 MB, templates 6 MB.

## How it works

draw.io's own desktop edition is Electron: the editor page asks a main process to open, save and read files through an `electron` object. `pronao-electron.js` is that object, built on Pronao's commands (`dialog.open`, `dialog.save`, `fs.read`, `fs.write`, `fs.readBytes`, `fs.writeBytes`, `fs.stat`, `shell.openUrl`, `window.setDirty`). It was written from what the editor calls (`js/diagramly/ElectronApp.js`, Apache-2.0); **no code from drawio-desktop's Electron shell is used, because that part is GPL-3.0.** Three things make the editor use it:

- the browser identifies itself as `... electron/32 draw.io/32`, which `bootstrap.js` checks;
- `window.process.versions.electron` exists, which turns off the browser's own file picker (it would bypass the grants) and a second, online load of the script bundles;
- after the editor reports it has loaded, it is told to start with a new, unsaved document.

What the bridge answers with nothing, because this program has no such feature: automatic drafts and backups, file watching, update checks, local fonts, spell-check and zoom menu items (the last three do nothing).

## What is tested

- `node examples/drawio/test_bridge.js`: 12 checks of the bridge against a fake `pronao` (the dialogs, text and binary round trips, a path outside the grants refused, the conflict check on save, nothing saved when the data is empty, links, closing).
- In a real window, with the real Pronao commands: the editor starts in desktop mode with a new document; writing, reading and saving a file through the bridge; a path outside the allowed folder refused; the editor's "unsaved changes" state reaching the window and clearing again. These were run by hand during development, not by a script in this repository.
- **Not tested:** the native open and save dialogs (they need a person), export to PNG, SVG and PDF, opening `.vsdx` and images, printing, drag and drop of files onto the window, and most of the editor's menus. Anything that the page does without the bridge is draw.io's own behaviour, which this repository does not test.
- The packed release build (55,788,544 bytes) was started and its window read back: the editor shows its desktop interface with "All changes saved" and no error box. Before `window.process` was provided, the editor loaded its script bundles twice and stopped on an "already defined" error.

## Licence and name

draw.io (the editor, `src/main/webapp`) is Apache-2.0; its licence file is copied next to the site as `LICENSE-drawio.txt` by `fetch.sh`. "draw.io" and "diagrams.net" are jgraph's names; this example calls the window "Diagrams" and does not use their logo as its own icon. Check jgraph's terms before shipping a program built from this.
