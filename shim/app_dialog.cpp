#include "app_dialog.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "app_fs.h"
#include "app_shim_internal.h"

namespace {

struct FilterPair {
    std::string label;
    std::vector<std::string> extensions;   // empty = all files
};

bool plainText(const std::string& s, size_t maxBytes) {
    if (s.size() > maxBytes) return false;
    if (s.find('\0') != std::string::npos) return false;
    return app_fs_is_utf8(s) == 1;
}

bool extensionOk(const std::string& e) {
    if (e.empty() || e.size() > 16) return false;
    for (char c : e) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// "Label\0txt,md\0Other\0*\0" -> pairs
[[maybe_unused]] bool parseFilters(const std::string& text, std::vector<FilterPair>& out) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : text) {
        if (c == '\0') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) return false;                    // the last part must be terminated
    if (parts.size() % 2 != 0 || parts.size() / 2 > 20) return false;
    for (size_t i = 0; i < parts.size(); i += 2) {
        FilterPair f;
        f.label = parts[i];
        if (f.label.empty() || !plainText(f.label, 80)) return false;
        if (parts[i + 1] != "*") {
            std::string ext;
            for (size_t k = 0; k <= parts[i + 1].size(); ++k) {
                if (k == parts[i + 1].size() || parts[i + 1][k] == ',') {
                    if (!extensionOk(ext)) return false;
                    f.extensions.push_back(ext);
                    ext.clear();
                } else {
                    ext += parts[i + 1][k];
                }
            }
        }
        out.push_back(f);
    }
    return true;
}

// a file name with no folder in it and none of the characters Windows refuses
[[maybe_unused]] bool plainFileName(const std::string& name) {
    if (name.empty() || !plainText(name, 200)) return false;
    if (name == "." || name == "..") return false;
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') return false;
    }
    return true;
}

}  // namespace

#ifdef APP_HEADLESS
// ===============================================================================================================
// Scripted answers, for tests.
// ===============================================================================================================
namespace {
std::mutex g_scriptMutex;
std::deque<std::string> g_answers;
std::deque<std::string> g_requests;

// pops the next answer; false if nothing was scripted
bool nextAnswer(const std::string& request, std::string& answer) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_requests.push_back(request);
    if (g_answers.empty()) return false;
    answer = g_answers.front();
    g_answers.pop_front();
    return true;
}

int pickerResult(int handle, const std::string& request, std::string& path) {
    (void)handle;
    std::string answer;
    if (!nextAnswer(request, answer)) return -1;
    if (answer == "cancel") return 0;
    if (answer.compare(0, 5, "path:") != 0) return -1;
    path = answer.substr(5);
    return 1;
}
}  // namespace

int app_dialog_test_push(std::string answer) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    g_answers.push_back(std::move(answer));
    return 1;
}

int app_dialog_test_take_request(std::string& request) {
    std::lock_guard<std::mutex> lock(g_scriptMutex);
    if (g_requests.empty()) return 0;
    request = std::move(g_requests.front());
    g_requests.pop_front();
    return 1;
}

int app_dialog_open_file(int handle, std::string title, std::string filters, std::string& path) {
    std::vector<FilterPair> f;
    if (!plainText(title, 200) || !parseFilters(filters, f)) return -2;
    return pickerResult(handle, "open|" + title + "|" + std::to_string(f.size()), path);
}

int app_dialog_save_file(int handle, std::string title, std::string default_name, std::string filters, std::string& path) {
    std::vector<FilterPair> f;
    if (!plainText(title, 200) || !parseFilters(filters, f)) return -2;
    if (!default_name.empty() && !plainFileName(default_name)) return -2;
    return pickerResult(handle, "save|" + title + "|" + default_name + "|" + std::to_string(f.size()), path);
}

int app_dialog_pick_folder(int handle, std::string title, std::string& path) {
    if (!plainText(title, 200)) return -2;
    return pickerResult(handle, "folder|" + title, path);
}

int app_dialog_message(int handle, std::string title, std::string message, std::string kind, std::string buttons) {
    (void)handle;
    if (!plainText(title, 200) || !plainText(message, 4000)) return -2;
    if (!(kind == "info" || kind == "warning" || kind == "error" || kind == "question")) return -2;
    if (!(buttons == "ok" || buttons == "okcancel" || buttons == "yesno")) return -2;
    std::string answer;
    if (!nextAnswer("message|" + title + "|" + kind + "|" + buttons, answer)) return -1;
    if (answer.compare(0, 7, "button:") != 0 || answer.size() != 8) return -1;
    return answer[7] - '0';
}

#elif defined(_WIN32)
// ===============================================================================================================
// Windows: the Vista common item dialogs (IFileDialog) and MessageBoxW, shown on the UI thread.
// ===============================================================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>

namespace {

std::wstring widen(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// mode: 0 open a file, 1 save as, 2 pick a folder. Result: 1 chose, 0 cancelled, -1 failed.
int showFileDialog(HWND owner, int mode, const std::string& title, const std::string& defaultName,
                   const std::vector<FilterPair>& filters, std::string& pathOut) {
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // S_FALSE / RPC_E_CHANGED_MODE are fine
    int result = -1;
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(mode == 1 ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg != nullptr) {
        DWORD options = 0;
        dlg->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
        if (mode == 0) options |= FOS_FILEMUSTEXIST;
        if (mode == 1) options |= FOS_OVERWRITEPROMPT;
        if (mode == 2) options |= FOS_PICKFOLDERS;
        dlg->SetOptions(options);
        if (!title.empty()) dlg->SetTitle(widen(title).c_str());

        std::vector<std::wstring> labels, patterns;   // must outlive Show()
        std::vector<COMDLG_FILTERSPEC> specs;
        if (mode != 2 && !filters.empty()) {
            for (const auto& f : filters) {
                labels.push_back(widen(f.label));
                std::wstring pat;
                if (f.extensions.empty()) pat = L"*.*";
                for (const auto& e : f.extensions) {
                    if (!pat.empty()) pat += L";";
                    pat += L"*." + widen(e);
                }
                patterns.push_back(pat);
            }
            for (size_t i = 0; i < labels.size(); ++i) specs.push_back({labels[i].c_str(), patterns[i].c_str()});
            dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
            dlg->SetFileTypeIndex(1);
            if (mode == 1 && !filters[0].extensions.empty()) dlg->SetDefaultExtension(widen(filters[0].extensions[0]).c_str());
        }
        if (mode == 1 && !defaultName.empty()) dlg->SetFileName(widen(defaultName).c_str());

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

int runPicker(int handle, int mode, const std::string& title, const std::string& defaultName,
              const std::vector<FilterPair>& filters, std::string& path) {
    auto out = std::make_shared<std::pair<int, std::string>>(-1, std::string());
    int ran = app_ui_call(handle, [=](void* native) {
        out->first = showFileDialog(static_cast<HWND>(native), mode, title, defaultName, filters, out->second);
    });
    if (!ran) return -1;
    if (out->first == 1) path = out->second;
    return out->first;
}

}  // namespace

int app_dialog_open_file(int handle, std::string title, std::string filters, std::string& path) {
    std::vector<FilterPair> f;
    if (!plainText(title, 200) || !parseFilters(filters, f)) return -2;
    return runPicker(handle, 0, title, std::string(), f, path);
}

int app_dialog_save_file(int handle, std::string title, std::string default_name, std::string filters, std::string& path) {
    std::vector<FilterPair> f;
    if (!plainText(title, 200) || !parseFilters(filters, f)) return -2;
    if (!default_name.empty() && !plainFileName(default_name)) return -2;
    return runPicker(handle, 1, title, default_name, f, path);
}

int app_dialog_pick_folder(int handle, std::string title, std::string& path) {
    if (!plainText(title, 200)) return -2;
    return runPicker(handle, 2, title, std::string(), std::vector<FilterPair>(), path);
}

int app_dialog_message(int handle, std::string title, std::string message, std::string kind, std::string buttons) {
    if (!plainText(title, 200) || !plainText(message, 4000)) return -2;
    UINT flags = MB_SETFOREGROUND;
    if (kind == "info") flags |= MB_ICONINFORMATION;
    else if (kind == "warning") flags |= MB_ICONWARNING;
    else if (kind == "error") flags |= MB_ICONERROR;
    else if (kind == "question") flags |= MB_ICONQUESTION;
    else return -2;
    if (buttons == "ok") flags |= MB_OK;
    else if (buttons == "okcancel") flags |= MB_OKCANCEL;
    else if (buttons == "yesno") flags |= MB_YESNO;
    else return -2;
    auto out = std::make_shared<int>(-1);
    const std::wstring wtitle = widen(title), wmessage = widen(message);
    int ran = app_ui_call(handle, [=](void* native) {
        *out = MessageBoxW(static_cast<HWND>(native), wmessage.c_str(), wtitle.c_str(), flags);
    });
    if (!ran) return -1;
    switch (*out) {
        case IDOK: return 1;
        case IDYES: return 3;
        case IDNO: return 4;
        case IDCANCEL: return 2;
        default: return buttons == "yesno" ? 4 : 2;   // closed with the X
    }
}

#else
// ===============================================================================================================
// Other platforms: no native dialogs yet (a GTK / Cocoa implementation belongs here).
// ===============================================================================================================
int app_dialog_open_file(int, std::string, std::string, std::string&) { return -1; }
int app_dialog_save_file(int, std::string, std::string, std::string, std::string&) { return -1; }
int app_dialog_pick_folder(int, std::string, std::string&) { return -1; }
int app_dialog_message(int, std::string, std::string, std::string, std::string) { return -1; }
#endif
