// Windows: native dialogs (see app_native.h): the Vista common item dialogs and MessageBoxW, shown on the window's UI thread.
// What may be shown (lengths, characters, which kinds) is decided in src/dialogs.clg.
#include <shobjidl.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../app_native.h"
#include "../app_shim_internal.h"
#include "util.h"

namespace {
// mode: 0 open a file, 1 save as, 2 pick a folder. 1 chose, 0 cancelled, -1 failed.
int showFileDialog(HWND owner, int mode, const std::string& title, const std::string& defaultName, const std::string& defaultExt,
                   const std::vector<std::string>& filters, std::string& pathOut) {
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // S_FALSE / RPC_E_CHANGED_MODE are fine
    int result = -1;
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(mode == 1 ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg != nullptr) {
        DWORD options = 0;
        dlg->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | (mode == 0 ? FOS_FILEMUSTEXIST : 0) | (mode == 1 ? FOS_OVERWRITEPROMPT : 0) |
                   (mode == 2 ? FOS_PICKFOLDERS : 0);
        dlg->SetOptions(options);
        if (!title.empty()) dlg->SetTitle(widen(title).c_str());
        std::vector<std::wstring> text;                    // must outlive Show()
        std::vector<COMDLG_FILTERSPEC> specs;
        for (const auto& f : filters) text.push_back(widen(f));
        for (size_t i = 0; mode != 2 && i + 1 < text.size(); i += 2) specs.push_back({text[i].c_str(), text[i + 1].c_str()});
        if (!specs.empty()) {
            dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            dlg->SetFileTypeIndex(1);
        }
        if (mode == 1 && !defaultName.empty()) dlg->SetFileName(widen(defaultName).c_str());
        if (mode == 1 && !defaultExt.empty()) dlg->SetDefaultExtension(widen(defaultExt).c_str());
        hr = dlg->Show(owner);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            result = 0;
        } else if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item != nullptr) {
                PWSTR name = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name != nullptr) {
                    pathOut = narrow(name);
                    CoTaskMemFree(name);
                    result = pathOut.empty() ? -1 : 1;
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(initHr)) CoUninitialize();
    return result;
}
}  // namespace

int app_dialog_pick(int handle, int mode, std::string title, std::string default_name, std::string default_ext, std::vector<std::string> filters,
                    std::string& path) {
    auto out = std::make_shared<std::pair<int, std::string>>(-1, std::string());
    if (!app_ui_call(handle, [=](void* native) { out->first = showFileDialog(static_cast<HWND>(native), mode, title, default_name, default_ext, filters, out->second); })) return -1;
    if (out->first == 1) path = out->second;
    return out->first;
}

// kind: 0 info, 1 warning, 2 error, 3 question. buttons: 0 ok, 1 ok+cancel, 2 yes+no. Returns the button: 1 ok, 2 cancel, 3 yes, 4 no
// (closing the box counts as cancel, or no for yes/no); -1 if it could not be shown.
int app_dialog_message(int handle, std::string title, std::string message, int kind, int buttons) {
    const UINT flags = MB_SETFOREGROUND | (kind == 1 ? MB_ICONWARNING : kind == 2 ? MB_ICONERROR : kind == 3 ? MB_ICONQUESTION : MB_ICONINFORMATION) |
                       (buttons == 1 ? MB_OKCANCEL : buttons == 2 ? MB_YESNO : MB_OK);
    auto out = std::make_shared<int>(-1);
    const std::wstring wtitle = widen(title), wmessage = widen(message);
    if (!app_ui_call(handle, [=](void* native) { *out = MessageBoxW(static_cast<HWND>(native), wmessage.c_str(), wtitle.c_str(), flags); })) return -1;
    switch (*out) {
        case IDOK: return 1;
        case IDYES: return 3;
        case IDNO: return 4;
        case IDCANCEL: return 2;
        default: return buttons == 2 ? 4 : 2;
    }
}
