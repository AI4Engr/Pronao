# examples/limits

What a page may send and which commands it may call: [manual chapter 8](../../docs/manual/08-security.md). `commands.clg` sets
`setRequestLimits(8192, 16)`, allows one extra navigation address, registers four commands and calls `allowOnlyCommands` with three of them.
The page tries each command, an over-long call and a flood, and shows what was answered, `forbidden`, `too_large` or `busy`.
`reset` is registered but not listed, so it answers `forbidden`, the same as a name nobody registered.

```
PAGE=examples/limits/index.html EXTRA=examples/limits/commands.clg bash scripts/build.sh examples/limits/main.clg out/limits.exe
```

Headless check (no window): `tests/examples/limits_check.clg`.
