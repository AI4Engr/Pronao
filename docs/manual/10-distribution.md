# 10. Giving a program to someone else

What this chapter covers is a **single portable exe**: no installer. What was run here is marked as tested; what needs something this
machine does not have (a certificate from a recognized authority, a timestamp server, a computer without WebView2) is marked as not tested.

## What the program is, and what it needs

One `.exe`, linked statically: no DLLs next to it, the web files compiled in. The only thing it needs from the computer is the **Microsoft WebView2 Runtime**, which
Windows 11 has, and which is installed on Windows 10 by Microsoft Edge's updates and by many applications; an older Windows 10 that was never updated may not have it.

The program checks for it before it opens a window:

```
if (!app.checkRuntime("My app")) { return 1; }       // a message box says what is missing and offers Microsoft's download page; the answer is false: end
```

The template (`scripts/new_project.sh`) calls it as the first thing in `main`; a program that was not made from it should too. Tested: with the runtime the window opens;
with a copy of the shim that reports it missing, the box appears, no window opens, and the program ends with exit code 3 (`tests/run_distribution.sh`).
**Not tested:** a computer that really lacks the runtime, and how the box looks there.

## Building the release

```
RELEASE=1 ICON=app.ico APP_VERSION=1.4.0 APP_NAME="My app" APP_COMPANY="Example Co" APP_DESCRIPTION="What it does" \
    bash scripts/build.sh main.clg out/my-app.exe          # a project made by new_project.sh: bash build.sh --release
```

- **`RELEASE=1`** compiles out the developer tools and the development-server switch, strips the symbols, and links the exe as a **Windows program** (no black console
  window when it is double-clicked). `GUI=0` keeps the console; the tests keep it because they read what the program prints. In a windowed program `std::cout` goes nowhere:
  use `app.showMessage(title, text)` for what the user must be told.
- **`ICON=app.ico`** is the icon in Explorer, the title bar and the taskbar (see [windows](05-windows.md#the-programs-icon)). `scripts/make_icon.py` makes a placeholder.
- **`APP_VERSION`** (numbers and dots, like `1.4.0`) and **`APP_NAME`, `APP_COMPANY`, `APP_DESCRIPTION`, `APP_COPYRIGHT`** become the version resource that Explorer shows under
  Properties > Details. An exe with no version information and no signature is what virus scanners and SmartScreen are most suspicious of; fill them in. Letters, digits, spaces and `. , _ - ( )` only.
- Size: 1.2 MB for the smallest program with `RELEASE=1`, 2.0 MB for one that prints with `std::cout` or serves several web files ([getting started](01-getting-started.md) has the numbers).

## Signing the exe

**Nothing here is signed with a real certificate, because this machine has none**: the `signtool` commands below were not run, and the claims about SmartScreen are what is generally said, to be checked against
Microsoft's current documentation. The PowerShell scripts further down were run, with a throwaway certificate.

Why: Windows' SmartScreen shows "Windows protected your PC" for a program it has not seen enough of, and for an unsigned one the user has to choose "More info" and then "Run anyway". A signature
ties the exe to a publisher; a certificate from a recognized authority is needed (there is no way to make one yourself that Windows trusts). What the authority sells and how fast the
program then stops being warned about has changed over the years (an "EV" certificate used to remove the warning at once; I do not know the current rules). Microsoft also offers a signing service
in Azure; I have not looked at its terms.

With a certificate file (`cert.pfx`) and the Windows SDK's `signtool`:

```
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /f cert.pfx /p PASSWORD out\my-app.exe
signtool verify /pa /v out\my-app.exe
```

- The two scripts in `scripts/` do this without the SDK, with PowerShell: `scripts/sign.ps1 -File out\my-app.exe -Pfx cert.pfx -PasswordEnv SIGN_PASSWORD -Timestamp http://timestamp.digicert.com` (the password comes from an
  environment variable, never from the command line) and `scripts/check_signature.ps1 -File out\my-app.exe`, which prints `status|subject|thumbprint`. If Windows PowerShell says that it could not load the
  module `Microsoft.PowerShell.Security`, a PowerShell 7 session has passed its `PSModulePath` on: start it from a plain command prompt, or run `set PSModulePath=` first.
  Tested here with a throwaway self-signed certificate (`tests/run_signing.sh`): signing, who signed, a changed file reported as changed, a wrong password refused. A self-signed certificate is reported as signed
  but not trusted, which is the correct answer; a real timestamp server and a certificate that chains to a trusted root were not used.
- **Sign last.** After the build is finished and stripped; any change to the exe afterwards (a packer, `strip`, resource editing) breaks the signature.
- **Use a timestamp server** (`/tr`), so the signature stays valid after the certificate expires.
- Keep the key out of the repository and out of build logs; hardware tokens and cloud signing keep it off the build machine.

## If a virus scanner objects anyway

A new, unsigned exe from an unknown publisher is sometimes flagged by heuristics. What helps, in order: sign it; keep the version resource filled in; never pack or compress the exe;
send the exe to the scanner's vendor as a false positive (all the large ones have a form for it). The program starts other programs only through `proc.run`, from a list the application
wrote (see [programs and links](04-programs-and-links.md)), and a scanner that is told that can be told what it does.

## A checklist before sending it

1. `RELEASE=1` build with icon and version; the exe starts by double-click with no console window.
2. `checkRuntime` is in `main`.
3. The program ends with `app.close()` (so the tray icon, if any, is removed).
4. Settings and window places are in `%APPDATA%\<name>\` (`rememberWindows`, `SettingsStore`): nothing is written next to the exe, so it can live in a read-only folder.
5. Signed, verified, with a timestamp.
6. Started once on a computer that is not the one it was built on.

## What is not covered

- An installer, Start-menu entries, uninstall, file associations and a `myapp://` handler (E19): a portable exe has none of these. If you later want an installer, the exe from this page is what goes in it.
- Auto-update (E23).
- Other systems: this page is Windows only.
