# programs

A page that runs programs from the application's fixed list, and opens an address in the browser.
Manual chapter 4 (`docs/manual/04-programs-and-links.md`): `allowProgram`, `enableProcessCommands`, `allowLinkSchemes`, `enableShellCommands`.

`hostname` takes no arguments. `git.log` has two typed slots, `{count:int:1:20}` and `{repo:folder}`; it is registered only if git is installed.
The page supplies values for the slots and nothing else: no program, no option, no command line. 500 commits, a folder outside the allowed one,
and an address with the scheme `javascript:` or `file:` are refused (`invalid_argument` or `forbidden`), and the system is asked for nothing.
Run it from a git repository (the allowed folder is ".") to see the log. `https://example.com/` really opens in the browser.

Run:
`PAGE=examples/programs/index.html EXTRA=examples/programs/commands.clg bash scripts/build.sh examples/programs/main.clg out/programs.exe`

Test: `tests/examples/programs_check.clg` (runs `hostname` and `git` for real; the browser is a test double that records the request, so that a browser really opens is not tested).
