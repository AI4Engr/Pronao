# 8. What a page may do, and what is checked

The limits on a page, and exactly what CeeLing checks and what it trusts.

## What a page may send, and where the window may go

```
app.setRequestLimits(4 * 1024 * 1024, 256);      // bytes per call, calls waiting; these are the defaults
app.allowNavigation("http://localhost:5173/");   // optional, e.g. a dev server; before open
app.allowOnlyCommands(names);                    // optional: this window's page may call only these (a std::vector<string>)
```
- **Size and flood limits.** A call larger than the limit, or arriving while that many are already waiting, is answered at once
  with `{code: "too_large"}` / `{code: "busy"}` and never reaches a command, so a page cannot make the application buffer
  without bound. A call whose id is empty or longer than 64 bytes is dropped (an empty id marks the shim's own records; a page
  could try to forge one by calling the browser's message channel directly).
- **Navigation.** The bridge is injected into every document the window shows, so a window that could be sent to another site
  would hand the application's commands to a stranger's page. After the application's own page, the window loads nothing except
  an address that starts with an allowed prefix; links, redirects, scripts and **frames** pointing elsewhere are cancelled, no
  new window can open, and the right-click menu is off in a release build (`debug` false). Each refusal reaches the page as the
  event `window.navigationBlocked {url}` (`pronao.window.onNavigationBlocked`). The real-window self-test tries a frame, a script
  navigation and a popup, and fails if the guard is removed. Windows only.
- **A window's own command list.** `app.allowOnlyCommands(names)` (after the commands are registered) limits what this window's
  page can call; anything else, registered or not, is answered `{code: "forbidden"}` (so names cannot be probed), and a name that
  is not registered makes the call return false and change nothing. (One `App` now serves any number of windows, each with a list of its own.)
- **What a second window can reach (checked 2026-11-16, `tests/isolation_main.clg`).** The folders the application allowed and the files the user gave (a dialog pick, a dropped
  file) belong to **the program**, not to a window: the file policy keeps no record of which window a grant came from. So when the user picks a file in window A, window B's page
  can read it too, if B may call `fs.read`; a file dropped on B is readable from A. Windows of one program share one site and one origin, so both pages are the application's own
  code, but if one of them shows something less trusted, give it a list of its own (`allowOnlyCommands`) that leaves the file commands out: the test shows such a window gets
  `forbidden` for both kinds of grant. `revokeGrants()` ends every grant at once. **Not done:** grants that belong to the window that received them (and end when it closes).
  The two checks that describe today's behaviour are marked KNOWN GAP in the test and are the ones that change when that is built.

## What is and is not checked

CeeLing checks the command table and everything the commands do with their own data: no dangling references, no
use-after-move, no raw pointers, bounds and null checks at run time. **Not checked, and trusted:** the native layer
(`shim/`), the `webview` library, the browser engine, every `ext_fn` declaration (a wrong signature is silently
believed), and any other C++ you link in.
