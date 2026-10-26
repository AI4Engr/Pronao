// POSIX: no native dialogs yet (a GTK implementation belongs here). -1 means "could not be shown".
#include "../app_native.h"

int app_dialog_pick(int, int, std::string, std::string, std::string, std::vector<std::string>, std::string&) { return -1; }
int app_dialog_message(int, std::string, std::string, int, int) { return -1; }
