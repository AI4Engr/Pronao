# weather

A five-day forecast in a frosted-glass page, with two CeeLing commands behind it.

- `weather.cities` lists the cities; `weather.forecast {city}` returns the current weather and five days. The page reads `{city}` into a struct (`ForecastArgs`, decoded by the compiler), so a missing or mistyped field is refused as `invalid_argument` before the command runs, and a city with no data is refused with its own code, `not_found`.
- **The numbers are sample data kept in the program.** The CeeLing side cannot reach a network yet, so this is not a live forecast; the page says "Source: sample data".
- The page is one HTML file: a gradient with a blurred, semi-transparent card (`backdrop-filter`), the logo as a small inline image, and buttons that call `pronao.invoke`. The blur is inside the page; the window frame is the system's.

Manual: [chapter 2, How it works](../../docs/manual/02-how-it-works.md). Run it from the repository root:

    PAGE=examples/weather/index.html EXTRA=examples/weather/commands.clg bash scripts/build.sh examples/weather/main.clg out/weather.exe
