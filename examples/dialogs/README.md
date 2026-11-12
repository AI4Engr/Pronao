# dialogs

A page that opens a native file dialog and reads the file the user picked, and shows files dropped on the window.
Manual chapter 3, "Dialogs" and "Files dragged onto a window" (`docs/manual/03-files-and-data.md`).

The program allows no folder. A pick (and a drop) is a grant for exactly that file, so the page can read it and nothing else.

What the page cannot reach: the neighbour of the picked file. The "Try the file next to it" button reads
`neighbour.txt` from the same folder and is refused with `forbidden`, as is a listing of the folder.

Run (Windows only for the dialog; elsewhere it rejects with `unsupported`):
`PAGE=examples/dialogs/index.html EXTRA=examples/dialogs/commands.clg bash scripts/build.sh examples/dialogs/main.clg out/dialogs.exe`

Test: `tests/examples/dialogs_check.clg` (scripted dialog answers and simulated drops; a real dialog or OS drop is not tested).
