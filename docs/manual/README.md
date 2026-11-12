# Pronao user manual

Each chapter says what the application writes in CeeLing, what the page calls in JavaScript, how it is checked, what the tests see and what they cannot see. Chapters stand alone; read 1 and 2 first.

| # | Chapter | What it covers |
|---|---|---|
| 1 | [Getting started](01-getting-started.md) | fetch the dependencies, build the example, make a project, run the tests |
| 2 | [How it works](02-how-it-works.md) | the page, the native layer and the CeeLing loop; events; reading arguments into a struct |
| 3 | [Files and data](03-files-and-data.md) | the file policy, per-user folders and settings, folder watching, dialogs, dropped files |
| 4 | [Programs and links](04-programs-and-links.md) | running a fixed list of programs; opening addresses and files in the system |
| 5 | [Windows](05-windows.md) | the window, layout, several windows, remembered places, one running copy, the icon |
| 6 | [Menus, tray, notifications and theme](06-menus-tray-notifications.md) | the menu bar, the tray icon, pop-up menus, notifications, light and dark |
| 7 | [Background work](07-background-work.md) | async commands, cancelling, deadlines, workers and the main thread |
| 8 | [What a page may do, and what is checked](08-security.md) | the limits on a page; what CeeLing checks and what it trusts |
| 9 | [The web UI and giving the program away](09-web-ui-and-shipping.md) | a front end of many files, the development server, release builds |
| 10 | [Giving a program to someone else](10-distribution.md) | the runtime check, the release build, signing, a checklist |
| A | [One command, from the page to the reply](appendix-command-journey.md) | `fs.read` with the generated C++ beside the CeeLing |
| B | [Compile errors you will meet](appendix-compile-errors.md) | what each CeeLing message meant and the fix |
| C | [Writing the program in C++](appendix-cpp.md) | build a `.cpp` main against the generated headers; the rules a C++ command follows; what is not checked |
| | [Source map](source-map.md) | where things are in the repository |

What the language did to this program, with counts: [chapter 16 of the CeeLing guide](https://github.com/AI4Engr/CeeLing/blob/main/docs/language-guide/16-case-study-pronao.md).
