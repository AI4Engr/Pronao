# Appendix B. Compile errors you will meet, and what to do

The messages below are the CeeLing compiler's (the fixtures are under `tests/safety/` in the [CeeLing repository](https://github.com/AI4Engr/CeeLing); the small
programs for the "language limits" are in the text); long help texts are shortened with `...`. Last checked against the compiler on 2026-11-09. A CeeLing error is a refusal, not a warning: the program is
outside the language until it is rewritten. The ones that came from building app-cling say so.

How to read one:

```
error[semantic/use-after-move]: use of moved-from value `m`
  --> tests/safety/map_moved_into_call_rejected.clg:18:5
  help: `m` was moved earlier; assign a new value before using it
```

The part in brackets is the stable name of the rule; the arrow is the `.clg` line (not the generated C++).

## The program would be unsafe

| Message | What it meant | What to do |
|---|---|---|
| `error[semantic/use-after-move]: use of moved-from value 'm'` | A vector, map or object was handed to a function (or assigned to a new owner), which takes it. Using it afterwards would read an emptied value. | Use the value only before the call, pass a fresh local, copy it first, or make the helper a method of the class that owns the data. app-cling's tests use `emptyArgs()` (a fresh vector per call). A function that only reads its vector parameter does not move it: the compiler sees that and passes a `const&`. |
| `error[semantic/use-after-move]: use of dangling observer 'r' (its owner 'x' is gone)` | A `ref` observer still points at something whose owner was moved, reassigned or left its scope. | Do not keep the observer past the owner's death; take it again after the change, or use a plain local (an owner) instead of a `ref`. |
| `error[semantic/use-after-move]: returning observer 'b' (a 'ref') escapes its function` | A function returned a `ref` to something inside its own frame. | Return the owner itself, or let the caller create its own observer. |
| `error[semantic/use-after-move]: 'ref c' is bound to 'parent->child', whose owner cannot be tracked` | A `ref` may observe only a local object, an element `v[i]` of a local container, another `ref`, or `nullptr`. A field or a call result has no local owner whose death the checker could see. | Declare `c` without `ref` (a normal local that shares or owns). |
| `error[semantic/closure-captures-observer]: closure captures 'observed', a 'ref' observer` | A closure copies what it captures into a field that can outlive the observer. | Capture the observer's owner instead, or restructure so the closure does not need it. |
| `error[semantic/spawn-captures-object]: 'spawn' cannot move 'mine' into a new thread: 'mine' has more than one owner` | A thread may be given only copies of plain values, or one object nobody else can reach. This object is stored elsewhere as well. | Make the data a `struct` (it is copied), or turn it into a `string` and rebuild it inside the thread. The `spawn` entry under "Strategy decisions" in CeeLing's `CLAUDE.md` has the exact five conditions. |
| ``error[semantic/forbidden-construct]: `Channel<Thing>` would carry `Thing`, which is not a plain value`` | A `Channel` carries what a `spawn` may capture: numbers, `string`, vectors of those and `struct`s made of those. | Send a `struct`, or serialise the object to a string first. |
| ``error[semantic/forbidden-construct]: `std::vector<Channel<int>>` holds a Channel inside another type`` | A `Channel` is used as itself (a local, parameter, field or result), never as an element of a vector or map. | Keep the channels in separate variables. |

## The language does not have it (by design)

| Message | What it meant | What to do |
|---|---|---|
| ``error[semantic/forbidden-construct]: raw pointer types (`T*`) are forbidden (found `Node*`)`` | A raw pointer is an escape hatch the checker cannot follow. | Declare a plain object variable; the compiler picks `unique_ptr`/`shared_ptr`/`weak_ptr`. |
| ``error[semantic/forbidden-construct]: '.get()' is forbidden on a CeeLing object ('p' is a Node)`` | `.get()` would hand out a raw pointer. | Use the object directly, or add a named accessor to the class. |
| ``Syntax error ... Suggestion: raw `delete` is forbidden`` | There is no manual delete. | Remove it: lifetime is managed by the smart pointer. |
| ``error[semantic/forbidden-construct]: call to unknown function/method `myCube` `` | Anything CeeLing did not compile itself (a library function with no borrow summary) is refused by default. | Define the function in CeeLing, call a known one, or for a number-only native function declare it with `ext_fn` (this is the trusted boundary). |
| `error[semantic/use-after-move]: 'throw' operand is not a string` | Only a `string` can be thrown: there are no exception classes. | `throw "invalid_argument: ...";`. app-cling's dispatcher turns the `code: message` text into the page's `{code, message}`. |
| `error[semantic/forbidden-construct]: statement at file scope` | No global variables: they have no owner and no lifetime the checker can reason about. | Put the value in a class field or pass it as a parameter. |

## Generated decoders (`decodeX`)

| Message | What it meant | What to do |
|---|---|---|
| ``error[semantic/struct-json-constructor]: struct `Thing` has no constructor taking its fields in declaration order`` | The decoder builds the struct with `new Thing(...)`. | Add the constructor the message shows, or add a constructor **without parameters** to make every field optional. |
| ``error[semantic/struct-json-field]: field `c` of struct `Thing` has type `char`, which a generated decoder cannot read`` | Supported: `int`, `int64`, `double`, `float`, `bool`, `string`, `byte`, a struct, and vectors of those. | Read that field by hand from the `Item`, or change its type. |
| ``error[semantic/struct-json-needs-cjson]: generated struct decoders need cjson-cling`` | The decoder calls `getObjectItem`, `isString`, ... which are not visible. | `#include "cjson_api.clh"` (and `cjson_item.clh`). |
| ``call to unknown function/method `decodeDialogArgs` `` | Seen while building app-cling: a decoder was called from a file that included the struct's file, and the compiler had not declared it there. **Fixed in CeeLing** (the pass now also runs on included files). | If it shows up on a new compiler, update `cling.exe`: `dist/cling.exe` may be older than `build/cling.exe`. |

### Files compiled one by one

| Message | What it meant | What to do |
|---|---|---|
| ``conversion from 'std::unique_ptr<Item>' to non-scalar type 'std::shared_ptr<Item>' requested`` (a g++ error in a generated file, not a CeeLing message) | Pointer kinds are chosen per file. The file that declares a class made a field or a vector's elements `unique_ptr`; another file uses them in a way that needs `shared_ptr` (an element copied out, a field handed to a closure). | Build with `scripts/build.sh`: it runs `scripts/pointer_demands.sh` first, which makes the declaring file read what the other files need. When you call the compiler by hand, use `--ptr-demands-out` / `--ptr-demands` the same way. |
| ``call to unknown function/method `decodeDialogArgs` `` in another module | `decodeX` may only be called in the file that declares the struct. | Add a small function next to the struct (`readDialogArgs(item)`) that calls it, and call that. |

## Where the language got in the way (seen while writing app-cling)

These are limits of today's language. The first two are plain messages; the others come out as a parser error or are quietly different.

| What you wrote | What you get | Instead |
|---|---|---|
| `name().length()` (a method on a call's result) | ``call to unknown function/method `length` `` | `string n = name(); int k = n.length();` |
| `use(std::vector<string>())` (an empty vector as an expression) | `Syntax error ... node type 'this_expression'` | `std::vector<string> none; use(none);` or a small helper that returns one. |
| `x = a > b ? 1 : 0;` | `Syntax error ... near: ? 1 : 0` | An `if`/`else`. |
| `return x` (missing `;`) | `Syntax error ... node type ';'` | Add the `;`. Syntax errors give the line and column of the parser, not a fix. |
| A closure inside a closure that captures the outer closure's captures | ``error[semantic/closure-unknown-capture]: closure captures `total`, which is not in scope here`` | Copy the value to a local of the outer closure's body first. |

## What an error does not tell you

- A rejection names the rule and the line, not always the shortest fix; `help:` gives the usual one.
- Some mistakes are not refused but are only caught at run time: an out-of-range subscript or an empty handle prints `cling: ... at line N` and aborts.
- Nothing here finds a wrong answer: a program can be accepted and still be incorrect (app-cling's `fs.watch` race was found by tests, not by the compiler).
