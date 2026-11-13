# Examples

Each folder is a complete program for one part of the [manual](../docs/manual/README.md): the commands in `commands.clg`, the program in `main.clg`, the page in `index.html` (or `dist/`), and a short README. Every example has a headless check in `tests/examples/` that drives its commands through the real dispatcher, including the inputs it must refuse.

| Example | Shows | Manual |
|---|---|---|
| [hello](hello/) | the smallest program: a window and three commands | [1](../docs/manual/01-getting-started.md) |
| [commands](commands/) | named commands, a struct read from the arguments, a custom error code, events to the page | [2](../docs/manual/02-how-it-works.md) |
| [files](files/) | one allowed folder and the file commands; a path outside it is refused | [3](../docs/manual/03-files-and-data.md) |
| [settings](settings/) | declared, typed settings; a wrong value or an undeclared key is refused | [3](../docs/manual/03-files-and-data.md) |
| [dialogs](dialogs/) | a file dialog whose pick becomes a grant for that file; dropped files | [3](../docs/manual/03-files-and-data.md) |
| [watch](watch/) | watching a folder; change events | [3](../docs/manual/03-files-and-data.md) |
| [programs](programs/) | a fixed list of programs with typed slots; links the page may open | [4](../docs/manual/04-programs-and-links.md) |
| [windows](windows/) | title, size, a second window, confirm-before-close, a fixed-size window | [5](../docs/manual/05-windows.md) |
| [menus](menus/) | a menu bar with shortcuts, a pop-up menu, a notification, light and dark | [6](../docs/manual/06-menus-tray-notifications.md) |
| [background](background/) | an async command on a worker: progress events, a deadline, cancelling | [7](../docs/manual/07-background-work.md) |
| [limits](limits/) | request size and flood limits, a command list | [8](../docs/manual/08-security.md) |
| [site](site/) | a front end of several files served from memory, and a release build | [9](../docs/manual/09-web-ui-and-shipping.md), [10](../docs/manual/10-distribution.md) |
| [cpp](cpp/) | the same kind of program written in C++ against the generated headers | [C](../docs/manual/appendix-cpp.md) |
| [weather](weather/) | a five-day forecast in a frosted-glass page; a struct argument, a custom error code (sample data: no network yet) | [2](../docs/manual/02-how-it-works.md) |
| [drawio](drawio/) | the draw.io diagram editor as one exe: open and save through dialogs, unsaved-changes warning (needs `fetch.sh`; not built by `run_examples.sh`) | [3](../docs/manual/03-files-and-data.md) |
| [template](template/) | what `scripts/new_project.sh` copies for a project of your own | [1](../docs/manual/01-getting-started.md) |

Build one from the repository root (the exact line is in each README), for example:

```
PAGE=examples/files/index.html EXTRA=examples/files/commands.clg bash scripts/build.sh examples/files/main.clg out/files.exe
```

`bash tests/run_examples.sh` builds every example and runs every check (207 checks, about 8 minutes: each example is compiled twice). The pages' scripts are only checked for syntax, and what a real window shows (a menu bar, a toast, a tray click, a drag from the file manager) is not tested here; the real-window scripts in `tests/` cover part of that.
