// POSIX: no single-instance support: the program simply runs and nothing is enforced. app_command_line reads /proc/self/cmdline.
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
