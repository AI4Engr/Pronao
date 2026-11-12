# cpp

A program written in C++ instead of CeeLing. The framework is CeeLing compiled to C++; `main.cpp` includes the headers the compiler generated (`app.h`, `core.h`, `cjson_item.h`) and calls it.

- `commands.h` has four commands (`add`, `mean`, `greet`, `fail`) as lambdas, wrapped by `makeCommand`, which turns any `std::exception` into a `command_failed` error (the dispatcher catches only `std::string`).
- The file commands are still the framework's checked CeeLing: the page reaches one folder and a path outside it is refused.
- The C++ commands themselves are not checked by the CeeLing compiler: no dangling or use-after-move analysis, no inserted bounds or null checks.

Manual: [appendix C, Writing the program in C++](../../docs/manual/appendix-cpp.md). Run it from the repository root:

    PAGE=examples/cpp/index.html bash scripts/build.sh examples/cpp/main.cpp out/cpp.exe
