// shim-internal: what the C++ files of the shim (and the test double in tests/doubles) share. Not seen by CeeLing.
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Runs `fn` on the window's UI thread and waits for it (a modal dialog may keep it running as long as the user likes). `fn` gets the native window (an HWND).
// 1 if it ran, 0 if there is no such window or it closed first.
int app_ui_call(int handle, const std::function<void(void* nativeWindow)>& fn);

// What the window may navigate to (shim/win/nav.cpp). The bridge is injected into every document the window shows, so a page that navigates away would hand it
// to a stranger's page. The first document is the application's own; after it only an address starting with one of `prefixes` may load, and no new windows.
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

// Stops every watch of a window; used by app_close, never by CeeLing.
int app_watch_stop_all(int handle);

// The menu bar of a window (shim/win/menu.cpp; the POSIX file answers "nothing"). app_menu_command (window procedure): true if a WM_COMMAND is one of this window's items.
// app_menu_shortcut (browser engine): true if the key is a shortcut of an enabled item, so the page does not get it. app_menu_forget: the window is closing.
bool app_menu_command(int handle, unsigned command);
bool app_menu_shortcut(int handle, unsigned vk, unsigned mods, bool repeat);
void app_menu_forget(int handle);

// A handle in the table with only a queue (no window), used by the tray (shim/win/tray.cpp). app_queue_entry_end frees it. app_tray_forget stops the tray behind
// a handle (the icon goes first) and ignores a handle that is not a tray.
int app_queue_entry_new();
void app_queue_entry_end(int handle);
void app_tray_forget(int handle);

// shim/win/theme.cpp: the title bar of a window dark or light; the page's preferred scheme (mode 0 follow the system, 1 light, 2 dark; 0 ok, 4 this runtime cannot).
void app_theme_titlebar(void* nativeWindow, bool dark);
int app_theme_page(void* controller, int mode);

// shim/win/drop.cpp: switches file-drop handling on or off for a window (UI thread only). app_drop_forget: the window is being destroyed.
int app_drop_set(void* nativeWindow, void* controller, int handle, bool on);
void app_drop_forget(void* nativeWindow);

// A modal window has closed: its owner can be used again. Called from the modal window's window procedure.
void app_window_modal_ended(int owner);
