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

// The one table of the application's own files (shim/assets.h), and its hook into the browser engine (shim/win/assets.cpp).
class AssetStore;
std::shared_ptr<AssetStore> assetStore();
void app_assets_install(void* controller, std::shared_ptr<AssetStore> store);

// Stops every watch of a window (its window is closing): used by app_close, never by CeeLing.
int app_watch_stop_all(int handle);

// The menu bar of a window (shim/win/menu.cpp; the POSIX file answers "nothing"). From the window procedure: a WM_COMMAND from the menu, true if the number is one of
// this window's items (the record "menu" id is then queued). From the browser engine: a key the page is about to get, true if it is a shortcut of an enabled item
// (then the page does not get it). app_menu_forget: the window is closing.
bool app_menu_command(int handle, unsigned command);
bool app_menu_shortcut(int handle, unsigned vk, unsigned mods, bool repeat);
void app_menu_forget(int handle);
