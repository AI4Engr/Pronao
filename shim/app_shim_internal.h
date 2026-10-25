// app-cling -- shim-internal interface between the C++ files of the shim. Not seen by CeeLing.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

// Runs `fn` on the window's UI thread and waits until it has finished (a modal dialog may keep it running for as long
// as the user likes). `fn` receives the native window handle (an HWND on Windows) to use as the dialog's owner.
// Returns 1 if `fn` ran, 0 if there is no such window, or it closed before `fn` could start (then `fn` never runs).
// Headless build: runs `fn(nullptr)` inline.
int app_ui_call(int handle, const std::function<void(void* nativeWindow)>& fn);

// Queues a record for the CeeLing main loop, as app_next_call will return it. Used by the file watchers: `id` is empty,
// which no call from the page ever has. `max_queued` bounds the queue: if that many are already waiting nothing is
// added. Returns 1 queued, 0 not queued (full), -1 no such window.
int app_internal_push_call(int handle, const std::string& id, const std::string& request, size_t max_queued);

// True while the window handle is valid.
bool app_internal_exists(int handle);
