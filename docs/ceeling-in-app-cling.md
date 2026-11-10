# What CeeLing does in app-cling

First version, 2026-10-29. It is meant to grow; see "To add" at the end. Everything here is something that can be checked in the repository, and the
numbers come from `scripts/ceeling_stats.sh`, which you can run again.

app-cling is written in CeeLing (the checked language, `src/*.clg`) on top of a small trusted C++ layer (`shim/`). This page is about the first part: what the
language and its compiler give a real program of about 6,000 lines, and where they got in the way.

## In numbers

Measured on 2026-10-31 (the app in 17 modules, since the split of `app.clg`) with `scripts/ceeling_stats.sh` after `tests/run_headless.sh`:

| | app-cling `src/` |
|---|---|
| CeeLing source (comments and blank lines left out) | 6,115 lines |
| Memory-management words in that source (`delete`, `.get()`, `reinterpret`, `std::move`, `unique_ptr`, `shared_ptr`, `weak_ptr`) | **0** |
| Smart pointers the compiler chose and wrote in the generated C++ | 79 `unique_ptr`, 559 `shared_ptr`, 0 `weak_ptr` |
| `std::move` the compiler inserted | 59 |
| Bounds checks the compiler inserted (`cling::at`) | 508 |
| Null checks the compiler inserted (`cling::nn`) | 586 |
| Closures, of which thread jobs (`spawn`) | 47, of which 7 |
| `ext_fn` functions: the native surface the checked code can call (different names; a function several files use is declared in each, 88 lines in all) | 69 |

The count of `shared_ptr` is high relative to the size; I have not looked at why for each one, and the compiler's reasons are not printed.
The numbers come from `scripts/ceeling_stats.sh`; `SRC` and `GEN` count other files.

(An earlier version of this page counted declaration lines, 48, as if they were functions; the count by name was 47.)

The first row is the point. The program creates, shares, moves and drops many kinds of objects (windows, watches, command tables, the file policy, program runs) and its source
never says who owns what.

## What is checked here, and where you can see it

**Ownership is inferred.** `Window w = new Window(h, dispatcher.rules); windows.add(w);` (`App.openAt`) is all the source says. The compiler decides that `w` is shared (the book and the
caller both hold it) and writes `std::shared_ptr`; a value only one place owns becomes `unique_ptr`. A use of a value after it was moved is a compile error
(the tests have to work around it: see `emptyArgs()` in `tests/headless_main.clg`, a fresh vector per call because a vector passed to a method is moved).

**Every subscript and every object dereference is checked at run time.** `v[i]` is `cling::at(v, i, line)` and `n.field` is `cling::nn(n, line)`; a bad index or an
empty handle prints the `.clg` line and aborts instead of reading memory. `scripts/build.sh` builds app-cling with both checks on (the opt-out flags exist and are
not used here). The program has 508 and 586 of them (previous section: what they cost).

**Threads share nothing but plain values.** The 7 `spawn(closure Job [...])` in `src/app.clg` (the `proc.run` command, the four dialogs, the close
confirmation, the notification) can capture strings, numbers and vectors of them, and structs made of those; the compiler refuses an object. That rule shapes the design:
- `ToolRun` and `ToolResult` are `struct`s, because they have to travel into a worker;
- state that belongs to the main thread (the file policy, the window book) stays there, and a worker hands its result back as a record
  (`finishLater`, `Pump` + `wakeMain`) or through a `Channel<T>`, which carries copies of plain values;
- a worker cannot read a file through `FileGate` by accident, because it cannot hold one.

**Closures capture by value, only.** The 32 closures (`app.command("fs.read", closure Command [files, maxReadBytes] (Item args) {...})`) list what they capture;
there is no `[&]`, so a command cannot keep a reference to something that is gone. A command that needs "the window that called me" captures a shared
`CallContext` object instead of a global.

**The trusted surface is a list.** The checked code reaches the operating system only through the 48 `ext_fn` functions (the head of each `.clg` file, and
`shim/app_native.h` on the C++ side), all of them taking and returning numbers, strings and vectors of those. `scripts/shim_lines.sh` measures what is on the
other side. Policy lives on this side: `FileGate` decides what a path may reach by asking the operating system where an already open handle really points;
`runFitsOs` decides whether a program may be started; the C++ only opens and runs.

**Errors are values with a code.** `throw "not_found: no such watch";` is turned by the dispatcher into the page's `{code, message}`; a thrown string is the only
kind of exception, so an error cannot carry a hidden object.

## Generated decoders for command arguments

`window.open` and the four dialogs do not pick fields out of the JSON by hand. The shapes are structs (`OpenWindowArgs` in `src/windows.clg`; `DialogArgs`, `FilterArgs`,
`MessageArgs` in `src/dialogs.clg`; `NotifyArgs` in `src/notify.clg`) and writes `OpenWindowArgs given = decodeOpenWindowArgs(args);`; the compiler writes `decodeOpenWindowArgs` (see CeeLing's `struct_json.cpp`). Each struct has a
constructor without parameters, which makes every field optional with the constructor's value as the default (`width = 800`). What the decoder checks is the *type*: a field
of the wrong type is refused with its path, ``invalid_argument: `filters[0].name` must be a string``, and an `int` must be a whole number inside the range of an `int`. What a
value *means* is still checked by hand next to it (a title is plain text of at most 200 bytes, a width is 200 to 10000, a message is 1 to 4000 bytes).
`proc.run` stays hand-written: its arguments are slots that depend on the program registered at run time, which no struct can describe.

Doing this found two gaps in CeeLing, both fixed there: structs had no optional fields, and a decoder was not visible to a closure of an included file.

## What the checks cost

`scripts/bench_checks.sh` builds `tests/bench_main.clg` twice, with the two checks on and with `--no-bounds-checks --no-null-checks`, and runs them alternately.
Measured 2026-10-29 on one Windows machine (MinGW g++, `-O1`, best of 5 rounds), through the real dispatcher and file policy with the window replaced by the test doubles:

| operation | checks on | checks off | difference |
|---|---|---|---|
| `add`: a command that does almost nothing (parse, dispatch, reply) | 1.16 us | 1.12 us | +3.8% |
| `fs.read` of a 4 KiB file | 68.7 us | 68.3 us | +0.6% |
| `fs.list` of a folder with 200 files | 237.3 us | 236.3 us | +0.4% |

How far to trust that: repeated runs differ by about 2 to 3 percent (another series gave +2.7% for the read), so read these as "a few percent at most", not as three
precise figures. The build with the checks has 988 of them in the generated code, the other has none (the script prints both counts). What the numbers leave out: a real
call also crosses WebView2 and the browser's message loop, which costs milliseconds, so in use the share of the checks is smaller still; a different compiler or machine,
and code that loops over large vectors with a subscript (zlib-cling, where CeeLing measured +5.5% and +11.3%) would show other figures. app-cling is built with the checks on.

## What the tools found in this code

These are real, from building app-cling; they are the honest other half of the table above.
- **A reference cycle the compiler did not see.** The dispatcher owned commands, and the window commands captured the dispatcher: `shared_ptr` cycle, leaked at exit.
  LeakSanitizer reported it; the compiler did not (cycle detection, threat #12, is not built yet). The fix was to move the list of allowed commands and the
  "current caller" into small objects of their own (`CommandRules`, `CallContext`).
- **A compiler bug.** A parameter with the same name as a field was emitted as the field (`choose(vector names)` in a class with a field `names` assigned the
  field). It compiled and ran with the wrong data. Fixed in CeeLing (`param_shadows_field_accepted`).
- **Races the language does not model.** `fs.watch` answered before the operating system was watching; a file changed right away could be lost. Not a memory-safety
  bug, so nothing in CeeLing could have caught it: the test suite did, under load. Checking says "no dangling references, no data races through `spawn` / `Channel`",
  not "correct".

## One program, several files

`src/app.clg` was 3,915 lines in one file; it is now 1,852 lines plus eight modules (`dispatch`, `fscommands`, `dialogs`, `notify`, `programs`, `later`, `watches`,
`windows`; the list is `scripts/modules.sh`). CeeLing chooses `unique_ptr`, `shared_ptr` or `weak_ptr` **per file**, and a file's header fixes the kind of its
fields, container elements and method results. The first attempt at the split did not compile: a module that copied an element out of another module's vector needed
`shared_ptr` where the declaring module, which cannot see that use, had written `unique_ptr`. Three changes fixed it, in the compiler and not by annotating the source:
- two analyzer rules that were missing (an element copied out of a vector and then swapped or removed; a local initialised from a call that returns a shared object,
  then returned);
- **pointer demands**: each module is compiled once to write what it needs from the classes it includes (`--ptr-demands-out`, only members it reaches), the demands of
  all modules are merged, and every module is compiled again reading them (`--ptr-demands`). A demand can only make a member shared. `scripts/pointer_demands.sh`
  repeats this until nothing changes (a few rounds); `scripts/build.sh` runs it before it transpiles anything;
- a module that needs the arguments of a command declared in another module calls a small reader there (`readDialogArgs`, `readMessageArgs`, `readNotifyArgs`,
  `readOpenWindowArgs`), because a generated decoder `decodeX` may only be called in the file that declares the struct.

**Was the inference kept?** Checked with `scripts/ptr_table.py` against the last commit before the split, compiled with the unmodified compiler: the table of every
pointer kind in the generated C++ is identical except for four new wrappers (the readers above). The text of every function (line numbers and closure numbers
normalised) is identical except `Watches::flush`, which no longer copies `s.root` into a temporary `std::string` before passing it to `FileGate.resolve`. The cause: the
analysis decides whether a callee may modify a string field by matching calls **by name**, so in the 3,915-line program a name shared with some other function made it
cautious, and in the smaller file it is not. `FileGate.resolve` does not write that field, so the omitted copy is not a behaviour change; it is a difference, and this
page does not claim there are none beyond the ones the comparison found. The limit of the method: it compares what the compiler wrote, and says nothing about meaning.

## Where the language got in the way

Each of these cost real code or time; they are listed in CeeLing's `TODO.md`.
- No method call on a call's result (`f(x).length()`), so a local variable first.
- An object returned from a vector element needs `Window w = items[i]; return w;`; an object copied out of a field gets a different pointer kind than the field.
- An object passed to a free function is moved, so a helper that keeps using it has to be a method.
- A closure inside a closure cannot capture the outer closure's captures (copy them to locals first).
- `std::map` holds only primitives, so tables of objects are parallel vectors (`WindowBook`, `LaterBook`, `PumpBook`).
- Generics are fixed: `std::vector`, `std::map`, `Channel<T>`; no user templates.

One command from the page to the reply, with the generated C++ beside the CeeLing: [command-journey.md](command-journey.md).
See [compile-errors.md](compile-errors.md) for the messages themselves and the fix for each.

## What this does not claim

- It is not "memory safe" or "thread safe" as a whole. The claim is about the checked code: no dangling references, no use after move, no out-of-bounds
  subscript or empty-handle dereference that goes unnoticed (it aborts), no data race through `spawn` or `Channel`.
- Everything outside it is trusted: the C++ in `shim/`, WebView2 and the browser engine, the operating system, every `ext_fn` (a wrong declaration is silently trusted),
  and the programs `proc.run` starts.
- Deadlocks, waiting forever, and the size of the worker pool are not checked.
- The two run-time checks are opt-out per build (`--no-bounds-checks`, `--no-null-checks`). A statement about a program is a statement about how it was built.

## To add

- A second journey for an asynchronous command (`proc.run`: the `spawn`, the worker, `Pump`, the reply from another thread).
