// app-cling -- native dialogs (open, save as, pick a folder, message box). Trusted C++, like the rest of the shim.
//
// Return convention for the three pickers: 1 = the user chose (output set), 0 = the user cancelled, negative = nothing
// was shown: -1 no window or not supported on this platform, -2 the arguments were refused (see the limits below).
// Every dialog is shown on the window's UI thread, owned by the window, and the calling thread waits; call it from a
// worker (the dialog commands do) so the command loop keeps running while the dialog is open.
//
// `filters` is a list of pairs, each part followed by a NUL: a label ("Text files") then a comma-separated list of
// extensions without dots ("txt,md"), or "*" for all files. At most 20 pairs, a label at most 80 bytes, an extension
// 1-16 characters from [A-Za-z0-9_-]. Titles are at most 200 bytes, a message 4000, a suggested name 200 and an
// ordinary file name (no folder, no colon, no reserved characters). All text is UTF-8.
#pragma once
#include <string>

int app_dialog_open_file(int handle, std::string title, std::string filters, std::string& path);
int app_dialog_save_file(int handle, std::string title, std::string default_name, std::string filters, std::string& path);
int app_dialog_pick_folder(int handle, std::string title, std::string& path);

// kind: "info" | "warning" | "error" | "question". buttons: "ok" | "okcancel" | "yesno".
// Returns the button pressed: 1 ok, 2 cancel, 3 yes, 4 no (closing the box with the window's X counts as cancel, or
// no for yes/no); negative as above.
int app_dialog_message(int handle, std::string title, std::string message, std::string kind, std::string buttons);

#ifdef APP_HEADLESS
// Test-only. No dialog is shown: each call takes the next scripted answer. An answer is "cancel", "path:<text>" for the
// pickers, or "button:<1-4>" for the message box. With nothing scripted a dialog returns -1.
int app_dialog_test_push(std::string answer);
// The oldest request a dialog received, as "kind|title|extra" (arguments that passed validation only). 0 if none.
int app_dialog_test_take_request(std::string& request);
#endif
