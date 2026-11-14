# excalidraw

The [Excalidraw](https://github.com/excalidraw/excalidraw) drawing component (MIT) in a Pronao window.

This is **not** a repackaging of excalidraw.com. It is a small React page (`app/`, about 150 lines) around the component, so there is nothing to cut out: no live collaboration, no cloud storage, no analytics, no account. What the page does with the computer is what `main.clg` enables, and that is all of it:

- **Open, Save, Save as and Export (PNG, SVG)** go through Pronao's dialogs and file commands. A pick in a dialog makes exactly that file reachable, nothing around it. The component's own open and save (which would use the browser's file picker) and its image tool (a file chooser) are switched off.
- **The window asks before closing with unsaved work.** The page tells the program when the drawing is not the saved one (`window.setDirty`), and the title shows `•`.
- **Nothing is loaded from anywhere but the program.** The fonts come from the program (`window.EXCALIDRAW_ASSET_PATH`), and a `Content-Security-Policy` (`main.clg`) refuses any address that is not the program's own, so the component's fallback to a CDN cannot work even if it tried.
- No network access (`allowHost` is never called), no program to run.

## Build

```
bash examples/excalidraw/build_site.sh                       # needs Node 18+ and npm; installs the pinned packages (app/package-lock.json), builds the page into examples/excalidraw/site/
RELEASE=1 ICON=docs/assets/pronao.ico ASSETS=examples/excalidraw/site bash scripts/build.sh examples/excalidraw/main.clg out/excalidraw.exe
```

**Size, measured on one machine:** the program is **17.4 MB** (17,355,264 bytes). 13.6 MB of that are fonts, 12.9 MB of them the Chinese hand-drawn font (209 small files, loaded only when a drawing needs them; they are compressed already, so the program's gzip cannot make them smaller). `LEAN=1 bash examples/excalidraw/build_site.sh` leaves that font out and the program is **4.7 MB** (4,668,416 bytes); Chinese text is then drawn in a system font, not the hand-drawn one. Everything else is the same.

## What is tested

- `node examples/excalidraw/test/test_files.mjs`: 11 checks of the page's file logic (`app/src/files.js`) against a fake `pronao`: what is asked, what is read, that a cancelled dialog does nothing, that a refused file is an error and not a crash, that binary files go through as base64 with the bytes intact.
- `python examples/excalidraw/test/run_window_test.py` (Windows; a real window, about a minute, needs `build_site.sh` first): builds a test copy of the page and a test program that allows one scratch folder, then a driver inside the page opens a drawing through the menu, exports PNG and SVG, deletes everything, saves, saves as, and tries to read a file outside the folder, and tries to fetch, load an image from and load a script from another address (the policy must refuse all three). 15 checks on what is on disk and what the page saw: the title, the unsaved mark, a valid PNG of the drawing's size, an SVG, the saved and copied files, the outside file refused, the three external requests refused. **The dialogs are replaced by fixed answers**, because a native dialog cannot be driven by a script; what the dialog's own look and behaviour do is not tested.
- This test found a real bug in the page: after a save, the window went back to "unsaved", because the saved version was compared against a list without the deleted elements. It is fixed, and the check "saving clears the mark" is what keeps it so.
- **Not tested:** the native dialogs themselves, pasting images, the Library, collaboration (it is not there), printing, and most of what the component does inside its canvas (that is Excalidraw's own behaviour). Only Windows and only the Windows dialogs have been tried.
- `--shot out.png` saves the program's own window as a picture (needs Pillow); the README's picture was made that way.

## Licence

Excalidraw is MIT (`site/LICENSE-excalidraw.txt` carries the notice). The program also contains the packages Excalidraw and this page are built from (React, Mermaid's parsers and others: 245 in the dependency tree, 198 of them MIT, the rest ISC, Apache-2.0, BSD and a few more). `build_site.sh` writes `site/THIRD-PARTY-NOTICES.txt`, which lists each with its licence and reproduces the text of the 205 that have a licence file, and the program carries it. The tree it lists is a little larger than what the bundler kept, which is the safe side. I have not had a lawyer read any of this, and `dompurify` is under "MPL-2.0 OR Apache-2.0" (the choice is the user's, not made here). "Excalidraw" is the project's name.

Manual: [chapter 3, Files and data](../../docs/manual/03-files-and-data.md). The same method as [examples/drawio](../drawio/README.md): a web application whose file access is replaced by the program's commands.
