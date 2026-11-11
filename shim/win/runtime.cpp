// Windows: is the WebView2 runtime installed, and a message box for when it is not (see app_runtime_version and app_message_box in app_native.h).
// The runtime registers its version as the value "pv" under the "Clients" key of the Edge updater ("0.0.0.0" or missing: not installed), per machine (both registry
// views) or per user.
#include <windows.h>

#include <string>

#include "../app_native.h"
#include "util.h"

namespace {
bool readVersion(HKEY root, REGSAM view, const wchar_t* path, std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return false;
    wchar_t value[64] = {};
    DWORD size = sizeof value - sizeof(wchar_t), type = 0;
    const bool ok = RegQueryValueExW(key, L"pv", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    if (!ok || value[0] == 0 || std::wstring(value) == L"0.0.0.0") return false;
    out = value;
    return true;
}
}

int app_runtime_version(std::string& version) {
    static const wchar_t* kClients = L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    static const wchar_t* kClientsWow = L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    std::wstring v;
    if (readVersion(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, kClientsWow, v) || readVersion(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, kClients, v) ||
        readVersion(HKEY_CURRENT_USER, 0, kClients, v)) {
        version = narrow(v);
        return 0;
    }
    version.clear();
    return 1;
}

int app_message_box(std::string title, std::string text, int ask) {
    const std::wstring t = widen(title), m = widen(text);
    if (t.empty() || m.empty()) return -1;
    const int answer = MessageBoxW(nullptr, m.c_str(), t.c_str(), (ask ? MB_YESNO : MB_OK) | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    return answer == IDYES || answer == IDOK ? 1 : 0;
}
