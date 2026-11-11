// Windows: one running copy of a program per user session (see app_native.h).
//   claim   a named mutex ("Local\pronao-<name>"): the first process to create it is the instance.
//   handoff a later process writes its working folder and arguments to the instance's named pipe (DACL: this user and SYSTEM only, no remote clients; a message is
//           at most 64 KiB).
//   serve   one thread in the instance queues each message as an "instance" record on the window given to app_instance_listen (up to 16 are kept before that).
// What may be done with the arguments is decided in src/app.clg; this only moves bytes between two processes of the same user.
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../app_native.h"
#include "util.h"

namespace {
const size_t kMaxMessage = 64 * 1024;
const size_t kMaxPending = 16;

struct Instance {
    HANDLE mutex = nullptr;
    std::wstring pipeName;
    std::wstring userSddl;
    std::mutex m;
    int handle = 0;
    std::vector<std::string> pending;
    bool serving = false;
};
Instance* g_instance = nullptr;

std::string recordFor(const std::string& message) {
    std::string r = "instance";
    r += '\0';
    r += message;
    return r;
}

// the SDDL of a DACL for the current user and SYSTEM, or "" if the user cannot be found
std::wstring userOnlySddl(std::wstring& sidText) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size ? size : 1);
    std::wstring result;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
            sidText = sid;
            result = std::wstring(L"D:(A;;GA;;;") + sid + L")(A;;GA;;;SY)";
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return result;
}

void deliver(Instance* in, const std::string& message) {
    std::lock_guard<std::mutex> lock(in->m);
    if (in->handle != 0) {
        app_push(in->handle, std::string(), recordFor(message), 100);
    } else if (in->pending.size() < kMaxPending) {
        in->pending.push_back(message);
    }
}

HANDLE makePipe(Instance* in) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof sa;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(in->userSddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return INVALID_HANDLE_VALUE;
    sa.lpSecurityDescriptor = sd;
    HANDLE pipe = CreateNamedPipeW(in->pipeName.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &sa);
    LocalFree(sd);
    return pipe;
}

void serve(Instance* in) {
    HANDLE pipe = makePipe(in);
    while (pipe != INVALID_HANDLE_VALUE) {
        // (a client that already wrote and closed is reported as ERROR_PIPE_CONNECTED or ERROR_NO_DATA; its data can still be read)
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED || GetLastError() == ERROR_NO_DATA) {
            HANDLE ready = makePipe(in);                 // before serving this client: a launch right behind it must find a pipe
            std::string message;
            bool tooLong = false;
            char chunk[4096];
            DWORD got = 0;
            while (ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) && got > 0) {
                if (message.size() + got > kMaxMessage) { tooLong = true; break; }
                message.append(chunk, got);
            }
            if (!tooLong && !message.empty()) deliver(in, message);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = ready;
        } else {
            DisconnectNamedPipe(pipe);
        }
    }
}

std::string currentFolder() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(sizeof buffer / sizeof buffer[0]), buffer);
    return (n > 0 && n < sizeof buffer / sizeof buffer[0]) ? narrow(std::wstring(buffer, n)) : std::string();
}
}

int app_command_line(std::string& out) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return 4;
    out.clear();
    for (int i = 1; i < argc; i++) {
        out += narrow(argv[i]);
        out += '\0';
    }
    LocalFree(argv);
    return 0;
}

int app_instance_claim(std::string name, int& first) {
    first = 1;
    if (name.empty() || name.size() > 64 || g_instance != nullptr) return 5;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 5;
    }
    const std::wstring w = widen(name);
    std::wstring sid;
    const std::wstring sddl = userOnlySddl(sid);
    if (sddl.empty()) return 4;
    const std::wstring pipeName = L"\\\\.\\pipe\\pronao-" + sid + L"-" + w;      // (the user's SID is in the name: two users on one machine do not meet)
    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\pronao-" + w).c_str());
    if (mutex == nullptr) return 4;
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        auto* in = new Instance();
        in->mutex = mutex;
        in->pipeName = pipeName;
        in->userSddl = sddl;
        g_instance = in;
        std::thread(serve, in).detach();
        return 0;
    }
    CloseHandle(mutex);
    first = 0;
    // tell the one that is running: its working folder and arguments (the instance may still be starting its pipe: try for a couple of seconds)
    std::string message = currentFolder();
    message += '\0';
    std::string args;
    app_command_line(args);
    message += args;
    if (message.size() > kMaxMessage) message.resize(kMaxMessage);
    AllowSetForegroundWindow(ASFW_ANY);                 // (we were just started by the user, so we may give the running one the focus)
    for (int attempt = 0; attempt < 20; attempt++) {
        if (WaitNamedPipeW(pipeName.c_str(), 100) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                const BOOL ok = WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &wrote, nullptr);
                CloseHandle(pipe);
                return ok && wrote == message.size() ? 0 : 4;
            }
        }
        Sleep(100);
    }
    return 4;
}

int app_instance_listen(int handle) {
    if (g_instance == nullptr || handle == 0) return 5;
    std::vector<std::string> now;
    {
        std::lock_guard<std::mutex> lock(g_instance->m);
        g_instance->handle = handle;
        now.swap(g_instance->pending);
    }
    for (const auto& message : now) app_push(handle, std::string(), recordFor(message), 100);
    return 0;
}
