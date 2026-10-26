// shim-internal: what the C++ files of the shim (and the test double in tests/doubles) share. Not seen by CeeLing.
#pragma once
#include <functional>
#include <string>

// Runs `fn` on the window's UI thread and waits until it has finished (a modal dialog may keep it running for as long as the user
// likes). `fn` gets the native window handle (an HWND on Windows). 1 if `fn` ran, 0 if there is no such window or it closed first.
int app_ui_call(int handle, const std::function<void(void* nativeWindow)>& fn);
