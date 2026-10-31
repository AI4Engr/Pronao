// POSIX: nothing separate has to be installed for the window (a GTK implementation belongs here when there is one), and no message box yet.
#include "../app_native.h"

int app_runtime_version(std::string& version) { version.clear(); return -1; }
int app_message_box(std::string, std::string, int) { return -1; }
