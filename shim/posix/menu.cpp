// POSIX: no native menus yet (a GTK implementation belongs here). The menu primitives answer "not supported" and nothing is ever clicked.
#include "../app_native.h"
#include "../app_shim_internal.h"

int app_menu_set(int, std::string) { return -1; }
int app_menu_item(int, std::string, int, int) { return -1; }
bool app_menu_command(int, unsigned) { return false; }
bool app_menu_shortcut(int, unsigned, unsigned, bool) { return false; }
void app_menu_forget(int) {}
