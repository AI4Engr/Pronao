// shim-internal: what the C++ files of the shim (and the test double in tests/doubles) share. Not seen by CeeLing.
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Runs `fn` on the window's UI thread and waits until it has finished (a modal dialog may keep it running for as long as the user
// likes). `fn` gets the native window handle (an HWND on Windows). 1 if `fn` ran, 0 if there is no such window or it closed first.
int app_ui_call(int handle, const std::function<void(void* nativeWindow)>& fn);

// What the window may navigate to (shim/win/nav.cpp). The bridge is injected into every document the window shows, so a page that
// navigates away (a link, a redirect, a frame, window.open) would hand it to a stranger's page. The first document is the
// application's own; after it only an address starting with one of `prefixes` (set by CeeLing) may load, and no new windows.
struct NavRules {
    std::mutex m;
    std::vector<std::string> prefixes;
    std::string first;
    int handle = 0;
    bool loaded = false;
};
void app_nav_install(void* controller, std::shared_ptr<NavRules> rules, int debug);
