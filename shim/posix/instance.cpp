// POSIX: no single-instance support yet (a lock file and a socket belong here). The program simply runs; nothing is enforced.
#include <fstream>
#include <iterator>
#include <string>

#include "../app_native.h"

int app_instance_claim(std::string, int& first) { first = 1; return -1; }
int app_instance_listen(int) { return -1; }

int app_command_line(std::string& out) {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    if (!in) return 4;
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t program = all.find('\0');
    out = program == std::string::npos ? std::string() : all.substr(program + 1);
    return 0;
}
