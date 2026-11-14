# weather

A live five-day forecast in a frosted-glass page. The data comes from [Open-Meteo](https://open-meteo.com/), a public weather service that needs no key.

- The page calls `pronao.http.request({ url, headers })`. **The request is sent by the program, not by the page.** `commands.clg` lists the one host the program may reach (`api.open-meteo.com`, https only), allows no request body and no method but GET, and limits the answer to 64 KiB and the request to 10 seconds. A page cannot call any other address, cannot send `Authorization` or `Cookie`, and has no CORS to fight with.
- Everything the page needs from CeeLing is two things: the list of cities with their coordinates (`weather.cities`) and the decision of what may be requested. The forecast request, the icons and the layout are in `index.html`.
- **A service that wants a key** works the same way, with one more line in `commands.clg`: `app.addHttpHeader("api.example.com", "Authorization", "Bearer ...")`. The header is added by the program to every request to that host; the page never sees it and cannot send one of its own. (Open-Meteo needs none, so this example has no such line. The key would live in the program, where someone who has the `.exe` can find it: it is hidden from the page, not from the user.)
- The window shows an error with its code when there is no network (`connect_failed`), a slow answer (`timeout`) or a bad answer (`http_500`): it does not hang.
- **Data and terms:** forecasts by Open-Meteo.com (CC BY 4.0, attribution is shown in the window). Open-Meteo's free service is for non-commercial use; read its terms before putting a program like this in front of other people.

The check in `tests/examples/weather_check.clg` runs the program's setup against a scripted network (no internet needed): the page's exact request is answered, another host, plain http, another port and a lookalike name are refused, a POST and an `Authorization` header are refused, and the limits reach the native layer. The page's own code is not run by it. The real thing was also run in a window against the real service (a `PrintWindow` of the program's own window), by hand, not by a script in this repository.

Manual: [HTTP requests from the program](../../docs/manual/04-programs-and-links.md). Run it from the repository root (it needs the internet):

    PAGE=examples/weather/index.html EXTRA=examples/weather/commands.clg bash scripts/build.sh examples/weather/main.clg out/weather.exe
