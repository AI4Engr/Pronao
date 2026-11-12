# commands

Named commands, checked arguments, structured errors and an event from CeeLing to the page.

- `add` reads positional arguments with `numberAt`.
- `order` reads `{item, quantity}` into a struct; the compiler writes `decodeOrderArgs`. A missing or wrongly typed field is refused as `invalid_argument`.
- Errors carry a code the page switches on: `invalid_argument`, and `out_of_stock`, a code of the program's own (`throw "out_of_stock: ..."`).
- `countdown` pushes `tick` events with `app.emit`; the page listens with `pronao.on('tick', ...)`.

Manual: [chapter 2, How it works](../../docs/manual/02-how-it-works.md). Run it from the repository root:

    PAGE=examples/commands/index.html EXTRA=examples/commands/commands.clg bash scripts/build.sh examples/commands/main.clg out/commands.exe
