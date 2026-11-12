# watch

A page that watches a folder, writes a file into it through the file commands and shows the change events.
Manual chapter 3, "Watching folders" (`docs/manual/03-files-and-data.md`).

The page's only folder is the program's own data folder (`app.allowAppFolder("pronao-watch", "data")`: `%LOCALAPPDATA%\pronao-watch`,
XDG `~/.local/share/pronao-watch`). The events carry the path relative to the watched folder, never where the folder is.
The page can watch only folders the file commands reach, so `..` or another folder is `forbidden`.

Run:
`PAGE=examples/watch/index.html EXTRA=examples/watch/commands.clg bash scripts/build.sh examples/watch/main.clg out/watch.exe`

Test: `tests/examples/watch_check.clg` (drives a real watcher thread in the headless build; it makes the data folder if it is missing and removes it again).
