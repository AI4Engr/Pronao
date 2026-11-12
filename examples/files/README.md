# files

One folder the page may reach, and the file commands that work inside it.

- `app.allowAppFolder("pronao-files", "data")` makes the program's own data folder and allows it. No path is written in the program.
- `app.enableFileCommands(...)` registers `fs.read`, `fs.write`, `fs.list`, `fs.remove` and the others.
- The page writes, reads, lists and removes `note.txt` with relative paths (inside the allowed folder), and a path that leaves the folder is refused with `forbidden`.

Manual: [chapter 3, Files](../../docs/manual/03-files-and-data.md). Run it from the repository root:

    PAGE=examples/files/index.html EXTRA=examples/files/commands.clg bash scripts/build.sh examples/files/main.clg out/files.exe
