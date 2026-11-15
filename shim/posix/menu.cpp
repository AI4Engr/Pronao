// Linux: the tray is not implemented (a Linux desktop has no common tray API: the old one is dead on GNOME and Wayland, the new one is a D-Bus protocol that not every
// desktop shows) and answers -1, so nothing is ever clicked there. The menu bar, pop-up menus and shortcuts are in shim/posix/gtkmenu.cpp, notifications in notify.cpp.
#include "../app_native.h"
#include "../app_shim_internal.h"

int app_tray_open(std::string, std::string, int& handle) { handle = 0; return -1; }
int app_tray_update(int, std::string, std::string) { return -1; }
int app_tray_item(int, std::string, int, int) { return -1; }
void app_tray_forget(int) {}

// (app_theme_system is in window.cpp; app_window_theme, app_drop_accept, app_window_rect, app_window_style, app_window_drag and app_window_modal are in app_shim.cpp, with their
// Windows code)
