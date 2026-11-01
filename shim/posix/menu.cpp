// POSIX: no native menus yet (a GTK implementation belongs here). The menu primitives answer "not supported" and nothing is ever clicked.
#include "../app_native.h"
#include "../app_shim_internal.h"

int app_menu_set(int, std::string) { return -1; }
int app_menu_item(int, std::string, int, int) { return -1; }
bool app_menu_command(int, unsigned) { return false; }
bool app_menu_shortcut(int, unsigned, unsigned, bool) { return false; }
void app_menu_forget(int) {}

int app_tray_open(std::string, std::string, int& handle) { handle = 0; return -1; }
int app_tray_update(int, std::string, std::string) { return -1; }
int app_tray_item(int, std::string, int, int) { return -1; }
void app_tray_forget(int) {}

int app_menu_popup(int, std::string, std::string& chosen) { chosen.clear(); return -1; }

int app_tray_notify(int, std::string, std::string, int, std::string) { return -1; }
int app_notifier_open(std::string, int& handle) { handle = 0; return -1; }

int app_theme_system() { return -1; }
int app_window_theme(int, int) { return -1; }
int app_drop_accept(int, int) { return -1; }

int app_window_rect(int, std::string& out) { out.clear(); return -1; }
int app_window_style(int, int, int) { return -1; }
int app_window_drag(int, int) { return -1; }
int app_window_modal(int, int, int) { return -1; }
