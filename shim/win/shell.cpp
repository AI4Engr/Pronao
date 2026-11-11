// Windows: hand an address or a path to the shell (see app_native.h). What may be opened is decided in src/links.clg; this only asks the shell to do it.
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "../app_native.h"
#include "util.h"

namespace {
// SEE_MASK_FLAG_NO_UI: a failure comes back as an error code instead of a message box nobody asked for
int run(const wchar_t* file, const wchar_t* params) {
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = file;
    info.lpParameters = params;
    info.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&info) ? 0 : 4;
}
}

int app_shell_open(std::string target, int mode) {
    if (target.empty() || target.find('\0') != std::string::npos) return 5;
    if (target.compare(0, 4, "\\?\\") == 0 && target.size() > 6 && target[5] == ':') target = target.substr(4);   // (the shell does not take the long-path form)
    const std::wstring w = widen(target);
    if (w.empty()) return 5;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);     // (the shell wants COM on the calling thread; any answer is fine)
    if (mode == 1) {
        if (target.find('"') != std::string::npos) return 5;
        const std::wstring args = L"/select,\"" + w + L"\"";
        return run(L"explorer.exe", args.c_str());
    }
    return run(w.c_str(), nullptr);
}
