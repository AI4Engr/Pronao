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
const r = await pronao.proc.run({ program: 'git.log', args: { count: 20, repo: '.', file: 'src/a.txt' } });
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
await pronao.shell.openUrl({ url: 'https://example.com/docs' });   // default browser
await pronao.shell.openFile({ path: 'C:/Users/me/Docs/report.pdf' });   // default program; the file must be reachable by the file commands and of a listed type
await pronao.shell.reveal({ path: '...' });                          // shown in Explorer / the file manager; starts nothing
```

- **Addresses** must be http, https or mailto, allowed by the application, in printable ASCII (percent-encode the rest; no spaces, quotes, `<>` or backslashes), at most 2048 bytes, and for http / https a host with no `user@`.
  `file:`, `javascript:` and `data:` are never opened.
- **Files** go through the same file policy as `fs.*` (an allowed folder, a regular file, links resolved), and the extension must be on the application's list. Extensions that start a program or script
  (`exe`, `bat`, `cmd`, `lnk`, `js`, `ps1`, `hta`, `sh`, `py`, ... about 70 of them) are refused even if the application lists one. Opening a program is `proc.run`'s job, with its own table.
- **At most about three opens a second**, so a page cannot open a thousand tabs.
- **Not verified automatically:** that a browser, the default program or Explorer really appear. The tests check what the system is asked to do (a test double records it) and that refused targets ask for nothing; the native call
  itself was checked only for its refusals. Click it once by hand on a machine with a browser.
- **What it cannot know:** what the browser or the default program will do with what it gets. Listing `html` or `svg` for `openFile` is the application's choice.

## HTTP requests from the program

A page that calls a web API from its own code has to hold the API key, which the page must not be trusted with, and it is subject to CORS. Here the program sends the request (`src/http.clg`); the native layer (`shim/win/http.cpp`, over WinHTTP) sends exactly one request and returns what came back. It follows no redirect, keeps no cookie and sends no Windows logon credentials of its own accord.

```
app.allowHost("api.example.com");                                  // https only; a name, or name:port. No wildcards, no addresses.
app.allowInsecureHost("localhost");                                // http, and only to localhost, 127.0.0.1 or [::1]
app.addHttpHeader("api.example.com", "Authorization", "Bearer ...");   // added to every request to that host; the page never sees it
app.allowHttpMethods(methods);                                     // a std::vector<string> such as POST, PUT; GET and HEAD are always allowed; also PATCH, DELETE
app.httpLimits(1048576, 8388608, 15000);                           // request body, response body (bytes), time for one request with its redirects (ms)
app.enableHttpCommands();                                          // registers http.request
```

```js
const r = await pronao.http.request({ url: 'https://api.example.com/v1/items?limit=2', headers: { Accept: 'application/json' } });
// { status: 200, headers: { 'content-type': 'application/json', ... }, body: '{"n":2}' }   -- body is text; responseType: 'bytes' gives bodyBase64 instead
await pronao.http.request({ url: '...', method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(x) });
```

- **Only listed hosts**, matched exactly (the name, or `name:port` for a port other than 443). A status of 400 or more is an answer, not an error. An address with `user@`, a space, a quote, a backslash or a non-ASCII byte is refused, as is a name that is an IP address (loopback aside).
- **The page can send only** `Accept`, `Accept-Language`, `Content-Type`, `If-None-Match` and `If-Modified-Since`, with values of printable ASCII. It cannot send `Authorization` or `Cookie` or anything it made up; a header the application added for a host replaces a page header of the same name. `Set-Cookie` is not shown to the page.
- **Redirects are followed here, one hop at a time, at most five**, and each hop is checked like the first request: a server cannot send the request to a host that is not listed (`redirect_not_allowed`) or down from https to http, and a header added for one host is not sent to another. A 303 (and a 301 or 302 after a POST) asks again with GET and no body; 307 and 308 keep the method and the body.
- **Limits:** the request body (default 1 MiB), the response (default 8 MiB; a longer one is refused with `too_large`, not cut) and the time (default 15 s, a page may ask for less). At most 20 requests start in a second (`too_many`; `app.httpRate(n)` changes it). The request runs on a worker, so the program keeps serving, and the page can cancel it with an `AbortSignal` (`cancelled`; the tests cancel through `app_cancel`, which is what an abort calls).
- **Error codes** the page can switch on: `host_not_allowed`, `redirect_not_allowed`, `invalid_url`, `invalid_argument`, `method_not_allowed`, `too_large`, `too_many`, `timeout`, `connect_failed`, `tls_failed`, `too_many_redirects`, `not_text`, `cancelled`, `unsupported`. A certificate that is expired, for another host or self-signed is `tls_failed`: Windows' own certificate checks are not relaxed.
- **Tested:** 82 headless checks through the real command pipeline with a scripted network (what a page may and may not make the program send, where a redirect may go, limits, cancelling), each rule proven by a negative control that removes it; and 27 checks of the real WinHTTP layer against a server on the loopback interface (redirects not followed, no cookie sent back, the byte limit, the time limit, a cancel, a header with a line break refused, a server that sends a standard deflate stream). TLS was checked by hand against `example.com` (answered) and three public test hosts with a bad certificate (all refused as `tls_failed`). The weather example was run in a real window against the real Open-Meteo service. **That run found a bug the loopback tests had missed:** with "gzip, deflate" offered, Open-Meteo answers with a standard zlib deflate stream, which WinHTTP fails to read (`E_ABORT`, status 200 and no body), so only gzip is offered now; the loopback server reproduces it with a stream of that form and the test fails without the change.
- **Not covered:** what a listed host does with the request; DNS and routing (a listed name that resolves to a private address is still reached); proxies other than the system's; cookies, WebSockets, streaming and uploads of large files; Linux (the layer answers `unsupported`). The key lives in the program's memory and in its code: it is hidden from the page, not from someone who has the `.exe`.

## The environment of a program

- **The environment of a program** is withheld: a program gets only `PATH`, `SystemRoot`, `SYSTEMDRIVE`, `windir`, `TEMP`, `TMP`,
  `TMPDIR`, `LANG`, `LC_ALL`, `HOME` and `USERPROFILE` (those that are set), plus whatever `app.passEnvironment("NAME")` adds.
  A token in the application's own environment is not handed to `git`. A program linked dynamically needs `PATH` to find its
  libraries, which is why it is in the default list.
