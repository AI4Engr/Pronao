# 4. Programs and links

Starting programs from a fixed list, and opening addresses and files in the system.

## Running programs

The application lists which programs may run and with what arguments; the page can only pick one by name and fill in the
named slots. It never supplies a program, a command line or an option, and there is no shell.

```
std::vector<string> log;
log.push_back("log");  log.push_back("-n");  log.push_back("{count:int:1:500}");
log.push_back("--format=%H%x09%s");  log.push_back("--");  log.push_back("{file:file}");
log.push_back("@cwd={repo:folder}");                      // the working folder
app.allowProgram("git.log", "git", log, 10000, 1000000);   // name, program, template, timeout ms, output cap in bytes
app.enableProcessCommands();
```
```js
const r = await cling.proc.run({ program: 'git.log', args: { count: 20, repo: '.', file: 'src/a.txt' } });
// { exitCode, stdout, stderr, timedOut, truncated: {stdout, stderr}, lossy }   -- a non-zero exit code is a result, not an error
```
Slots: `{n:int:min:max}` a whole number in range; `{n:token}` letters, digits and `. _ - / @ ~ ^ + =`; `{n:text:maxLen}` any
plain text; `{n:file}` `{n:folder}` `{n:path}` an existing file / folder / either that the file commands could reach,
passed as its absolute path. A slot that fills a whole argument may not start with `-` (so a value can never be taken for
an option); one slot per argument; every slot is required; a parameter the template does not have is refused. `@cwd={n:folder}`
names the working folder, otherwise it is the first allowed folder.

How a program is found and run: CeeLing (`resolveProgram`, 18 checks in `tests/programs_main.clg`) accepts an absolute path or a
bare name searched for on `PATH` only, never the current folder (a `.bat` or `.cmd` is refused, because `cmd.exe` would
re-read the arguments). The native runner (`shim/{win,posix}/proc.cpp`, 24 checks) quotes arguments with the C runtime's rules,
tested with every character a command line treats specially; the child has no standard input, no console window and
inherits only its three pipes; it runs inside a job object (a process group on Linux), so on a timeout, or if it leaves
something behind, the whole tree is killed; output is capped per stream and drained past the cap; text that is not valid
UTF-8 is repaired in CeeLing (`lossy: true`).
Not covered: what the program itself does (`git` with a hostile repository, for instance), environment variables (they
are inherited), and a program that parses its command line in a non-standard way.

## Opening links, files and folders in the system

A link in the page does nothing by default (the navigation guard cancels it). The application decides what may leave the window (`src/links.clg`; the native layer only hands a target to the shell):

```
app.allowLinkSchemes(schemes);            // from "http", "https", "mailto" -- until this is called no address is opened
app.allowOpenExtensions(types);           // "pdf", "txt" ...: the file types shell.openFile may open; program and script types are refused here
app.openLinksInBrowser(true);             // a CLICK on a link (or target=_blank) opens it in the default browser; a script that sets location does not
app.enableShellCommands();                // shell.openUrl, shell.openFile, shell.reveal
```

```js
await cling.shell.openUrl({ url: 'https://example.com/docs' });   // default browser
await cling.shell.openFile({ path: 'C:/Users/me/Docs/report.pdf' });   // default program; the file must be reachable by the file commands and of a listed type
await cling.shell.reveal({ path: '...' });                          // shown in Explorer / the file manager; starts nothing
```

- **Addresses** must be http, https or mailto, allowed by the application, in printable ASCII (percent-encode the rest; no spaces, quotes, `<>` or backslashes), at most 2048 bytes, and for http / https a host with no `user@`.
  `file:`, `javascript:` and `data:` are never opened.
- **Files** go through the same file policy as `fs.*` (an allowed folder, a regular file, links resolved), and the extension must be on the application's list. Extensions that start a program or script
  (`exe`, `bat`, `cmd`, `lnk`, `js`, `ps1`, `hta`, `sh`, `py`, ... about 70 of them) are refused even if the application lists one. Opening a program is `proc.run`'s job, with its own table.
- **At most about three opens a second**, so a page cannot open a thousand tabs.
- **Not verified automatically:** that a browser, the default program or Explorer really appear. The tests check what the system is asked to do (a test double records it) and that refused targets ask for nothing; the native call
  itself was checked only for its refusals. Click it once by hand on a machine with a browser.
- **What it cannot know:** what the browser or the default program will do with what it gets. Listing `html` or `svg` for `openFile` is the application's choice.

## The environment of a program

- **The environment of a program** is withheld: a program gets only `PATH`, `SystemRoot`, `SYSTEMDRIVE`, `windir`, `TEMP`, `TMP`,
  `TMPDIR`, `LANG`, `LC_ALL`, `HOME` and `USERPROFILE` (those that are set), plus whatever `app.passEnvironment("NAME")` adds.
  A token in the application's own environment is not handed to `git`. A program linked dynamically needs `PATH` to find its
  libraries, which is why it is in the default list.
