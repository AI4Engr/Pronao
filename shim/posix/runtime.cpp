// POSIX: nothing separate has to be installed for the window, and there is no message box.
#include "../app_native.h"

int app_runtime_version(std::string& version) { version.clear(); return -1; }
int app_message_box(std::string, std::string, int) { return -1; }
