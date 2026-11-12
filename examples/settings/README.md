# settings

Typed settings the program declares and the page reads and changes.

- `declareString`, `declareInt` and `declareBool` give each key a type, a default and the values accepted.
- `settings.open("pronao-settings")` makes the program's config folder; `settings.load()` reads `settings.json`.
- `app.enableSettingsCommands()` lets the page call `pronao.settings.get`, `getAll` and `set`, for the declared keys only.
- A key that was not declared is `unknown_setting`; a value of the wrong type or outside its range is `invalid_argument`. Every `set` that works sends the event `settings.changed`.

Manual: [chapter 3, The application's own folders and settings](../../docs/manual/03-files-and-data.md). Run it from the repository root:

    PAGE=examples/settings/index.html EXTRA=examples/settings/commands.clg bash scripts/build.sh examples/settings/main.clg out/settings.exe
