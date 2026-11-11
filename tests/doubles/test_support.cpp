// What the tests need that the product does not (see test_support.h), and the dialogs: instead of showing one, app_dialog_* return
// the next scripted answer.
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "../../shim/app_native.h"
#include "../../shim/app_shim_internal.h"
#include "../../shim/assets.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

int app_test_make_temp_dir(std::string& out) {
    std::error_code ec;
    static std::atomic<int> counter{0};
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) return 4;
    for (int attempt = 0; attempt < 100; ++attempt) {
#ifdef _WIN32
        const long pid = static_cast<long>(GetCurrentProcessId());
#else
        const long pid = static_cast<long>(getpid());
#endif
        const auto dir = base / ("pronao-" + std::to_string(pid) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
        if (std::filesystem::create_directory(dir, ec)) {
            // the OS's own spelling (symlinks in /tmp, 8.3 names in %TEMP%), which is what the policy compares against
            out = std::filesystem::canonical(dir, ec).u8string();
            return ec ? 4 : 0;
        }
    }
    return 4;
}
int app_test_remove_tree(std::string path) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(path), ec);
    return ec ? 4 : 0;
}
int app_test_write_raw(std::string path, std::string data) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    f << data;
    return f ? 0 : 4;
}
int app_test_mkdir_raw(std::string path) {
    std::error_code ec;
    std::filesystem::create_directory(std::filesystem::u8path(path), ec);
    return ec ? 4 : 0;
}
int app_test_read_raw(std::string path, std::string& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return 0;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return 1;
}
int app_test_exists(std::string path) {
    std::error_code ec;
    return std::filesystem::symlink_status(std::filesystem::u8path(path), ec).type() != std::filesystem::file_type::not_found ? 1 : 0;
}
int app_test_make_dir_link(std::string link, std::string target) {
#ifdef _WIN32
    // a directory junction: needs no privilege, unlike a symbolic link
    return std::system(("cmd /c mklink /J \"" + link + "\" \"" + target + "\" >nul 2>nul").c_str()) == 0 ? 0 : 4;
#else
    return symlink(target.c_str(), link.c_str()) == 0 ? 0 : 4;
#endif
}

namespace {
std::mutex g_scriptMutex;
std::deque<std::string> g_answers, g_requests;

bool nextAnswer(const std::string& request, std::string& answer) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_requests.push_back(request);
    if (g_answers.empty()) return false;
    answer = g_answers.front();
    g_answers.pop_front();
    return true;
}
std::atomic<int> g_gaugeNow{0}, g_gaugePeak{0};
}  // namespace

namespace {
std::string g_monitors;
}  // namespace
int app_test_monitors(std::string fields) {
    g_monitors = std::move(fields);
    return 1;
}
int app_monitors(std::string& out) {
    out = g_monitors;
    return out.empty() ? 4 : 0;
}

void logRequest(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_requests.push_back(line);
}

int app_test_dialog_push(std::string answer) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_answers.push_back(std::move(answer));
    return 1;
}
int app_test_dialog_take_request(std::string& request) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    if (g_requests.empty()) return 0;
    request = std::move(g_requests.front());
    g_requests.pop_front();
    return 1;
}
int app_dialog_pick(int, int mode, std::string title, std::string default_name, std::string default_ext, std::vector<std::string> filters, std::string& path) {
    std::string request = "pick|" + std::to_string(mode) + "|" + title + "|" + default_name + "|" + default_ext;
    for (const auto& f : filters) request += "|" + f;
    std::string answer;
    if (!nextAnswer(request, answer)) return -1;
    if (answer == "cancel") return 0;
    if (answer.compare(0, 5, "path:") != 0) return -1;
    path = answer.substr(5);
    return 1;
}
namespace {
int g_runtimeStatus = 0, g_boxAnswer = 1;
std::string g_runtimeVersion = "120.0.0.0";
}  // namespace
int app_test_runtime(int status, std::string version, int boxAnswer) {
    g_runtimeStatus = status;
    g_runtimeVersion = std::move(version);
    g_boxAnswer = boxAnswer;
    return 1;
}
int app_runtime_version(std::string& version) {
    version = g_runtimeStatus == 0 ? g_runtimeVersion : std::string();
    return g_runtimeStatus;
}
int app_message_box(std::string title, std::string text, int ask) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_requests.push_back("box|" + std::to_string(ask) + "|" + title + "|" + text);
    return ask ? g_boxAnswer : 1;
}
int app_shell_open(std::string target, int mode) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_requests.push_back("shell|" + std::to_string(mode) + "|" + target);     // (read with app_test_dialog_take_request; nothing is started)
    return 0;
}
int app_dialog_message(int, std::string title, std::string message, int kind, int buttons) {
    std::string answer;
    if (!nextAnswer("message|" + title + "|" + message + "|" + std::to_string(kind) + "|" + std::to_string(buttons), answer)) return -1;
    return (answer.compare(0, 7, "button:") == 0 && answer.size() == 8) ? answer[7] - '0' : -1;
}

int app_test_helper(std::string& path) {
    const char* v = std::getenv("APP_PROC_ECHO");
    if (v == nullptr || *v == '\0') return 0;
    path = v;
    return 1;
}
int app_test_sleep_ms(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 1;
}
int app_test_gauge_enter() {
    const int now = g_gaugeNow.fetch_add(1) + 1;
    int peak = g_gaugePeak.load();
    while (now > peak && !g_gaugePeak.compare_exchange_weak(peak, now)) {}
    return now;
}
int app_test_gauge_leave() { return g_gaugeNow.fetch_sub(1) - 1; }
int app_test_gauge_peak() { return g_gaugePeak.load(); }

int app_test_setenv(std::string name, std::string value) {
#ifdef _WIN32
    return SetEnvironmentVariableA(name.c_str(), value.c_str()) ? 0 : 4;
#else
    return setenv(name.c_str(), value.c_str(), 1) == 0 ? 0 : 4;
#endif
}

int app_test_fetch(std::string method, std::string url, int& status, std::string& headers, std::string& body) {
    std::shared_ptr<const std::string> bytes;
    if (!assetStore()->respond(method, url, status, headers, bytes)) return 0;
    body = *bytes;
    return 1;
}
