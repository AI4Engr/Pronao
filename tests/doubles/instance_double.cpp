// Test double for the single-instance primitives (shim/<platform>/instance.cpp is the real one; the native instance test links that, not this).
// Linked into every headless CeeLing program (scripts/build.sh --headless, tests/wsl_run.sh).
#include <mutex>
#include <string>

#include "../../shim/app_native.h"
#include "test_support.h"

void logRequest(const std::string& line);

namespace {
std::string g_commandLine;
int g_instanceFirst = 1, g_instanceStatus = 0;
}  // namespace
int app_test_instance(int first, int status, std::string commandLine) {
    g_instanceFirst = first;
    g_instanceStatus = status;
    g_commandLine = std::move(commandLine);
    return 1;
}
int app_instance_claim(std::string name, int& first) {
    logRequest("instance|claim|" + name);
    first = g_instanceFirst;
    return g_instanceStatus;
}
int app_instance_listen(int handle) {
    logRequest("instance|listen|" + std::to_string(handle));
    return 0;
}
int app_command_line(std::string& out) {
    out = g_commandLine;
    return 0;
}
