# background

A command that must not block the window: it runs on a worker thread, reports progress, has a deadline and stops when the page gives up. Manual chapter 7 ([docs/manual/07-background-work.md](../../docs/manual/07-background-work.md)).

- `app.asyncCommand` with an `AsyncCommand`: `start` copies its arguments into plain values and calls `spawn(closure Job [limit, id, handle] () {...})`. The compiler admits only plain values into the capture list.
- The job counts primes, sends `emitEvent(handle, "progress", ...)` ten times and answers with `replyOk`. It checks `callCancelled(handle, id)` as it goes.
- `app.setAsyncTimeout("primes.count", 20000)` is the deadline; the page's `AbortController` is the cancel button.

Run (needs the WebView2 runtime):

    PAGE=examples/background/index.html EXTRA=examples/background/commands.clg bash scripts/build.sh examples/background/main.clg out/background.exe
