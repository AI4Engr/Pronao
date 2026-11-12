# C. Writing the program in C++

Pronao is CeeLing compiled to C++, so a program can also be written in plain C++ against the headers the compiler generates. The framework's own parts (the file gate, the program list, the dispatcher, the limits) stay checked CeeLing; the commands you write in C++ are ordinary C++.

The example is [examples/cpp](../../examples/cpp/): `main.cpp`, `commands.h` and a page. Its headless check is `tests/examples/cpp_check.cpp`.

## Build

Give `scripts/build.sh` a `.cpp` file instead of a `.clg` file:

```
PAGE=examples/cpp/index.html bash scripts/build.sh examples/cpp/main.cpp out/cpp.exe
```

The script transpiles the framework modules as always, writes their headers (`app.h`, `core.h`, `cjson_item.h`, ...) to the work folder, compiles your file with that folder on the include path and links it with the native layer. `PAGE=`, `ASSETS=`, `ICON=`, `RELEASE=1`, `--headless` work as they do for a CeeLing program. Without `EXTRA=` nothing of yours is transpiled.

## What you write

```cpp
#include "app.h"
#include "cjson_item.h"
#include "core.h"

int main() {
    auto app = std::make_shared<App>();
    app->command("add", makeCommand([](std::shared_ptr<Item> args) {
        return createNumber(numberAt(args, 0) + numberAt(args, 1));
    }));
    if (!app->openPage("My app", 640, 420, false)) return 1;
    app->run();
    app->close();
    return 0;
}
```

The `makeCommand` helper is in [commands.h](../../examples/cpp/commands.h): a `Command` subclass that calls a lambda. Everything the CeeLing manual chapters describe (`allowAppFolder`, `enableFileCommands`, `onMenu`, windows, programs, events) is a method of `App` and is called the same way, with `->` for a member access.

## Rules a C++ command must follow

- **Errors are `std::string`.** The dispatcher catches only `std::string` thrown by a command, written `"code: message"`; a text without a code becomes `command_failed`. Any other exception (`std::runtime_error`, `std::bad_alloc`) is not caught and ends the program, so `makeCommand` turns `std::exception` into a `command_failed` string.
- **Names are global.** The generated code has no namespaces: `App`, `Command`, `Item`, `createNumber`, `numberAt` and every other name in the CeeLing modules are visible, and a name of yours that equals one of them will not compile.
- **Pointer kinds are inferred.** `App`, `Item` and `Command` are held in `std::shared_ptr` today; the compiler picks that per program, so look at the generated header if a signature does not match.
- **Call the framework from its main thread**, like a CeeLing program: commands run on the main loop, and only the calls documented for workers (chapter 7) may be made from another thread.

## What is checked and what is not

| | C++ program | CeeLing program |
|---|---|---|
| a page reaching a file, program or command it was not given | refused (CeeLing) | refused (CeeLing) |
| size and flood limits, command lists | enforced (CeeLing) | enforced (CeeLing) |
| dangling references, use after move in a command | **not checked** | rejected by the compiler |
| index and null checks in a command | **not checked** | inserted by the compiler |

A C++ command is trusted, in the way an `ext_fn` is: a bug in it is a bug in C++. What the page can reach does not depend on it, because those decisions are made before a command runs. Choose C++ for a command that needs a C++ library; keep the rest in CeeLing if you want the compiler's guarantee.

## What the test sees

The example's check builds against the test doubles (`--headless`), calls the commands through the real dispatcher, and covers the error cases above: a thrown `std::string`, a thrown `std::runtime_error`, and a path outside the allowed folder. It does not open a window.
