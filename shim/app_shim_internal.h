// app-cling -- shim-internal interface between the C++ files of the shim. Not seen by CeeLing.
#pragma once
#include <functional>

// Runs `fn` on the window's UI thread and waits until it has finished (a modal dialog may keep it running for as long
// as the user likes). `fn` receives the native window handle (an HWND on Windows) to use as the dialog's owner.
// Returns 1 if `fn` ran, 0 if there is no such window, or it closed before `fn` could start (then `fn` never runs).
// Headless build: runs `fn(nullptr)` inline.
int app_ui_call(int handle, const std::function<void(void* nativeWindow)>& fn);
