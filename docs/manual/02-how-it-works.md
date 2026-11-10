# 2. How it works

The page, the native layer and the CeeLing loop; how events travel back to the page; how a command reads its arguments.

## How it works

```
page JS --cling.invoke--> webview bind callback [UI thread] --> mutex queue (shim)
CeeLing main thread:  app_next_call()  <-- blocks on the queue
                      Dispatcher.handle(request)   parse JSON, find the command, call it
                      app_return(id, status, json) --> resolves / rejects the page's promise
```

The native layer (`shim/`, trusted C++; 1059 lines of code for a Windows build, 1258 counting comments and blank lines, `scripts/shim_lines.sh`) holds no application logic: it opens the window,
queues calls, opens files, runs programs, shows dialogs and waits for file changes, as small primitives that take and return
ints and strings. **Every decision is in `src/`**, which CeeLing checks: which files a page may reach, which programs it may
start, what a dialog may say, what a folder watch reports. CeeLing runs the loop, so no callback ever has to cross into
CeeLing. JSON is handled by [cJSON-cling](https://github.com/AI4Engr/cJSON-cling), reply texts are always built with it, never by string
concatenation.

Errors reach the page as a rejected promise carrying `{code, message}`: `bad_request` (not JSON, not an array, command
name not a string), `unknown_command`, `command_failed` (the handler threw, e.g. through `numberAt`/`stringAt`/`fieldOf`
when the page sent the wrong shape). A malformed request never stops the application.

## Events: CeeLing to the page

```
app.emit("progress", "{\"done\":3}");            // main thread
emitEvent(handle, "progress", text);              // any thread, e.g. a worker job (capture the int handle)
```
```js
const off = cling.on('progress', p => bar.value = p.done);   // off() removes it
const next = await cling.once('finished');                   // Promise for the next one
```
The payload must be valid JSON text. The script sent to the page is built only with the JSON printer, so a name or a
string containing quotes, backslashes or script text stays a string literal; an invalid payload, or one with trailing
text, makes `emit` return false and sends nothing. An event sent before the page has loaded and registered its listener
is dropped; events from one thread arrive in order.

## Reading a command's arguments into a struct

- **Arguments into a struct.** A command with several fields can read them with one call: declare a `struct` with a constructor that
  takes its fields in declaration order and write `Args a = decodeArgs(args);`. There is no `decodeArgs` function in your program; the
  compiler writes it (see [the CeeLing guide, chapter 12](https://github.com/AI4Engr/CeeLing/blob/main/docs/language-guide/12-many-files.md)). Fields may be `int`, `int64`, `double`, `float`, `bool`, `string`, `byte`, vectors of
  those, structs and vectors of structs. A missing field or one of the wrong type is refused as `invalid_argument` naming the field
  (`spot.y`, `tags[1]`); an `int` must be a whole number in range; extra keys are ignored. It checks shape, not meaning: lengths, paths and
  names still go through your own checks.
