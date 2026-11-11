# 7. Background work

Cancelling, deadlines, closing, workers that need the main thread, and the rule for background commands.

## Cancelling, deadlines and closing

```
app.setAsyncTimeout("proc.run", 120000);         // "" = every async command without a setting of its own; 0 = no deadline (the default)
app.setShutdownGrace(5000);                       // the default
```
```js
const c = new AbortController();
const p = pronao.invoke('proc.run', { program: 'git.log', args: { ... } }, { signal: c.signal });
c.abort();                                        // p rejects at once with {code: 'cancelled'}; the program is killed
```
- **A page call is answered once.** The native layer remembers which calls are waiting; the first answer settles the page's promise and any
  later one (a job that kept going after a deadline, a second `replyOk`, an id a worker made up) is dropped. So an answer sent by a timeout
  and the job's own late answer cannot both reach the page.
- **Abandoning.** `pronao.invoke(..., {signal})` tags the call; aborting rejects the promise immediately and sends `$cancel`. The call
  counts as *cancelled*: a program the call started (`proc.run`, anything run with a `ToolRun`) is killed together with everything it started,
  within about 50 ms, and a job of your own can check `callCancelled(handle, id)` between its steps and stop. Cancelling is a request, not an
  interruption: CeeLing cannot stop a loop that never checks, and a dialog stays open until the person answers. Only async commands can be
  abandoned; a sync command has finished before the cancel could arrive.
- **Deadlines.** After `setAsyncTimeout` milliseconds the call is answered `{code: 'timed_out'}`, counts as cancelled, and its late answer is
  dropped. Do not set one on the dialog commands, which wait for a person.
- **Closing.** When the window closes (or `app.close()` runs) every call of it is cancelled, in flight and later: running programs are
  killed, watchers are stopped, and jobs that check `callCancelled` stop. After `main` returns the jobs still running get `setShutdownGrace`
  milliseconds; then the process ends without them (a message on stderr says how many, status 0; nothing is unwound). Without the grace
  period a job that never ends keeps the process alive for ever, which is CeeLing's default (`cling::setExitGraceMs`).
- **Not done**: a job that blocks in a call this layer does not know about (an `ext_fn` of yours) cannot be interrupted, only abandoned at
  exit; there is no cancel for a running dialog; `Channel<T>` (next section) lets a worker ask the main thread for something and wait for the answer.

## Workers that need the main thread: Channel and pumps

State such as the file policy belongs to the main thread, so a worker cannot read a file itself. `Channel<T>` (a CeeLing type; see [the CeeLing guide, chapter 10](https://github.com/AI4Engr/CeeLing/blob/main/docs/language-guide/10-threads.md)) is a queue of plain values that threads share: a worker sends a request on one channel and waits for the answer on another that
travelled with the request. The main loop sleeps in its own queue, so after sending the worker calls `wakeMain(handle, "name")` and the loop
runs the pump registered with `app.onPump("name", closure Pump [channel] (FileGate gate) { ... })`:

```
struct FileAsk { string path; Channel<string> reply; FileAsk(string p, Channel<string> r) { path = p; reply = r; } }

Channel<FileAsk> toMain;
app.onPump("files", closure Pump [toMain] (FileGate gate) {
    while (toMain.wait(0)) {                       // poll; a pump must never wait
        FileAsk ask = toMain.take();
        ask.reply.send(gate.read(ask.path, 1048576).text);
    }
});
// in an async command, inside spawn(closure Job [asks, path, id, handle] () {...}):
Channel<string> reply;
asks.send(new FileAsk(path, reply));
wakeMain(handle, "files");
if (reply.wait(5000)) { replyOk(handle, id, "\"" + reply.take() + "\""); }
```
Limits to know: a pump runs on the thread that serves the window, so it must not wait or throw (an error has no page call to reject and is dropped);
`send` never blocks and fails when the channel holds 1024 values; a pool worker that sits in `wait` takes one of the pool's threads
(`cling::setWorkerLimit`), so give every `wait` a timeout. Nothing here is "thread-safe" in general: what is checked is that threads share only
channels, which carry copies of plain values. The headless test reads a file through a pump and runs a producer thread against the consumer
under TSan, with a control that removes the channel's lock and must be reported.

## Background commands and the thread rule

**Background work and the thread rule.** A command that must not block the window is an `AsyncCommand`: its `start`
copies what it needs out of the request into plain values and calls `spawn(closure Job [payload, id, handle] () {...})`.
The compiler lets only plain values into the capture list (primitives, `string`, vectors of those, and `struct`s made
only of those) are copied, and a flat class object created with `new` in the same function can be MOVED in (the main
thread cannot touch it afterwards); an `Item` or any other object that does not meet the rules is a compile error
(make the data a `struct`, or pass a string). The exact rules are in [chapter 10 of the CeeLing guide](https://github.com/AI4Engr/CeeLing/blob/main/docs/language-guide/10-threads.md). The worker
rebuilds its own JSON tree from the string and answers the page itself with `replyOk` / `replyError`, which go through
the mutex-protected shim. What this gives, stated exactly: *in the CeeLing code that was checked, a spawned job can
reach only its own copies and its own allocations, so there are no data races through `spawn`.* What it does not
give: anything about `ext_fn`s, the shim, the browser engine, the file system or deadlocks. Nothing here is
"thread-safe".

Jobs run on a capped worker pool (default max(4, cores); `app.setWorkerLimit(n)` changes it; extra jobs queue, oldest
first, and the command loop never blocks). A job that throws rejects the page's promise with `command_failed` and the
message: the dispatcher installs a default failure handler around every `AsyncCommand.start` (and clears it after), and
a `spawn` can carry its own as a second closure. An in-flight job keeps the process alive: on exit the runtime
finishes every queued job and joins the pool, so a job that never finishes hangs the exit, and enough of them occupy
every slot and starve the queue.
