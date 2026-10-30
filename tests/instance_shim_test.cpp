// The native single-instance support (shim/win/instance.cpp) with real processes: this program starts itself again as "the second launch".
//   instance_shim_test.exe              the test (the first instance)
//   instance_shim_test.exe second NAME ARG...   what the first one starts: claims NAME, which must already be held, and exits 0 when it was handed over
// What the application does with the arguments (limits, the focus, the event for the page) is CeeLing's (headless_main.clg).
#include "../shim/app_native.h"
#include "doubles/test_support.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#endif

static int g_failed = 0;
static int g_passed = 0;
static void check(const char* name, bool ok) {
    if (ok) { ++g_passed; std::printf("PASS %s\n", name); }
    else { ++g_failed; std::printf("FAIL %s\n", name); }
}

static std::vector<std::string> fields(const std::string& rec) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : rec) { if (c == '\0') { f.push_back(cur); cur.clear(); } else cur += c; }
    return f;
}

// the next record on the window, or an empty list if none came within `ms`
static std::vector<std::string> next(int h, int ms) {
    std::string id, req;
    if (app_next_call(h, ms, id, req) != 1) return {};
    return fields(req);
}

static int launchSecond(const std::string& self, const std::string& name, const std::string& argument) {
    const std::string command = "\"\"" + self + "\" second " + name + " " + argument + "\"";
    return std::system(command.c_str());
}

#ifdef _WIN32
// writes `bytes` to the instance's pipe the way a later launch does (the name has the user's SID in it, as in the shim); true if it was accepted
static bool sendRaw(const std::string& name, const std::string& bytes) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size);
    LPWSTR sid = nullptr;
    const bool ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
                    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid);
    CloseHandle(token);
    if (!ok) return false;
    std::wstring pipe = L"\\\\.\\pipe\\app-cling-" + std::wstring(sid) + L"-" + std::wstring(name.begin(), name.end());
    LocalFree(sid);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20 && h == INVALID_HANDLE_VALUE; attempt++) {
        WaitNamedPipeW(pipe.c_str(), 100);
        h = CreateFileW(pipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL w = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    CloseHandle(h);
    return w && wrote == bytes.size();
}
#endif

int main(int argc, char** argv) {
#ifndef _WIN32
    (void)argc; (void)argv;
    std::printf("instance tests: skipped (Windows only)\n");
    return 0;
#else
    if (argc >= 3 && std::string(argv[1]) == "second") {
        int first = -1;
        const int rc = app_instance_claim(argv[2], first);
        return (rc == 0 && first == 0) ? 0 : 10 + rc;
    }
    const std::string self = argv[0];
    const std::string name = "itest-" + std::to_string(GetCurrentProcessId());

    std::string line;
    app_command_line(line);
    check("this process's own arguments", line.empty());
    check("a name that is not plain is refused", [&] { int f = -1; return app_instance_claim("a/b", f) == 5 && f == 1; }());
    int first = -1;
    check("the first claim is the instance", app_instance_claim(name, first) == 0 && first == 1);
    int again = -1;
    check("a second claim in the same process is refused", app_instance_claim(name, again) == 5);
    check("listen needs a window", app_instance_listen(0) == 5);

    // launches before anyone listens are kept, up to 16
    check("a second launch is handed over (exit code 0)", launchSecond(self, name, "alpha") == 0);
    check("another one", launchSecond(self, name, "\"b c\"") == 0);
    bool allSent = true;
    for (int i = 0; i < 18; i++) allSent = sendRaw(name, std::string("folder\0extra", 12) + char('a' + i)) && allSent;
    check("eighteen more are accepted by the pipe", allSent);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));      // (the server handles what was sent; listen must find all of it waiting)
    const int h = app_open("t", 100, 100, "", "", "", 0);
    check("nothing is queued on the window before listen", next(h, 50).empty());
    check("listen accepts a window", app_instance_listen(h) == 0);
    auto r1 = next(h, 2000);
    check("the first message: record, folder, then the arguments", r1.size() == 5 && r1[0] == "instance" && !r1[1].empty() && r1[2] == "second" && r1[3] == name && r1[4] == "alpha");
    auto r2 = next(h, 2000);
    check("the second message keeps an argument with a space whole", r2.size() == 5 && r2[4] == "b c");

    int kept = 2;
    while (!next(h, 200).empty()) kept++;
    if (kept != 16) std::printf("kept %d\n", kept);
    check("only 16 of the 20 sent before listen were kept (2 launches, 14 raw)", kept == 16);

    // launches after listen arrive at once
    check("a launch after listen", launchSecond(self, name, "gamma") == 0);
    auto r3 = next(h, 2000);
    check("is queued", r3.size() == 5 && r3[0] == "instance" && r3[4] == "gamma");

    // a message over 64 KiB is dropped, and the instance keeps serving
    check("an oversized message is dropped", (sendRaw(name, std::string(70 * 1024, 'x')), next(h, 400).empty()));
    check("and the next normal one still arrives", sendRaw(name, std::string("f\0ok\0", 5)) && next(h, 2000).size() == 3);
    check("an empty message is dropped", (sendRaw(name, std::string()), next(h, 300).empty()));
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
#endif
}
