// Windows: files dragged from the shell (Explorer, the desktop) onto the window. See app_drop_accept in app_native.h.
//
// How the paths get here. The browser engine owns the window's drop target and a window of the program cannot take it over (tried: a target registered on the program's own windows is
// never asked, because the engine's windows, which are another program's, sit between the pointer and ours). So the engine's own handling is used, and the page is the one that is
// told: it gets the HTML drag events and a File object for each file dropped. The engine can turn such a File into the path it came from, but only for a host that asks
// (WebMessageReceived with AdditionalObjects), which is what this file does. src/bridge.js sends the message from its drop handler when the application has switched drops on.
//
// What that makes of the trust: the paths are those of File objects that a page was handed by the user (dropped, or chosen in its own file input) -- a page cannot make a File that has a
// path. A page that holds such a file can ask for the same grant a drop would give, and nothing else; whether it gets one is the application's call (src/drops.clg).
#include "../app_native.h"
#include "../app_shim_internal.h"
#include <WebView2.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include "util.h"

namespace {
const size_t kMaxPaths = 512;            // what one drop may carry through the native layer; the application's own limit is lower

class DropMessages : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    explicit DropMessages(int handle) : handle_(handle) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_ICoreWebView2WebMessageReceivedEventHandler)) ? static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this) : nullptr;
        if (*out) AddRef();
        return *out ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        if (!on.load()) return S_OK;
        LPWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;          // (a message that is not a string is somebody else's)
        const std::string text = narrow(raw);
        CoTaskMemFree(raw);
        long x = 0, y = 0;
        if (text.compare(0, 11, "cling.drop ") != 0 || std::sscanf(text.c_str() + 11, "%ld %ld", &x, &y) != 2) return S_OK;
        ICoreWebView2WebMessageReceivedEventArgs2* args2 = nullptr;
        if (FAILED(args->QueryInterface(IID_ICoreWebView2WebMessageReceivedEventArgs2, reinterpret_cast<void**>(&args2))) || !args2) return S_OK;
        std::vector<std::string> paths;
        ICoreWebView2ObjectCollectionView* objects = nullptr;
        if (SUCCEEDED(args2->get_AdditionalObjects(&objects)) && objects) {
            UINT count = 0;
            objects->get_Count(&count);
            for (UINT i = 0; i < count && paths.size() < kMaxPaths; i++) {
                IUnknown* object = nullptr;
                if (FAILED(objects->GetValueAtIndex(i, &object)) || !object) continue;
                ICoreWebView2File* file = nullptr;
                if (SUCCEEDED(object->QueryInterface(IID_ICoreWebView2File, reinterpret_cast<void**>(&file))) && file) {
                    LPWSTR path = nullptr;
                    if (SUCCEEDED(file->get_Path(&path)) && path) {
                        const std::string p = narrow(path);
                        if (!p.empty()) paths.push_back(p);                 // (a File that is not a file on disk has no path)
                    }
                    CoTaskMemFree(path);
                    file->Release();
                }
                object->Release();
            }
            objects->Release();
        }
        args2->Release();
        std::string record("drop", 4);
        record += '\0'; record += "files"; record += '\0';
        record += std::to_string(x); record += '\0';
        record += std::to_string(y); record += '\0';
        for (const auto& path : paths) { record += path; record += '\0'; }
        app_push(handle_, std::string(), record, 100);
        return S_OK;
    }
    std::atomic<bool> on{true};

private:
    std::atomic<ULONG> refs_{1};
    int handle_;
};

struct Registration {
    ICoreWebView2* view = nullptr;
    DropMessages* handler = nullptr;
    EventRegistrationToken token = {};
};
std::mutex g_mutex;                       // every window has its own UI thread
std::map<HWND, Registration>& registered() { static std::map<HWND, Registration> m; return m; }

// the page learns whether drops are switched on from window.__cling_drop (src/bridge.js): set for the document that is open and for every one that follows
void tellPage(ICoreWebView2* view, bool on) {
    const wchar_t* script = on ? L"window.__cling_drop=true;" : L"window.__cling_drop=false;";
    view->AddScriptToExecuteOnDocumentCreated(script, nullptr);
    view->ExecuteScript(script, nullptr);
}

void release(Registration& reg) {
    if (reg.view && reg.handler) reg.view->remove_WebMessageReceived(reg.token);
    if (reg.handler) { reg.handler->on = false; reg.handler->Release(); }
    if (reg.view) reg.view->Release();
}
}  // namespace

int app_drop_set(void* nativeWindow, void* controller, int handle, bool on) {
    HWND window = static_cast<HWND>(nativeWindow);
    auto* ctl = static_cast<ICoreWebView2Controller*>(controller);
    if (!window || !ctl) return 4;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = registered().find(window);
    if (!on) {
        if (it == registered().end()) return 0;
        tellPage(it->second.view, false);
        release(it->second);
        registered().erase(it);
        return 0;
    }
    if (it != registered().end()) return 0;
    Registration reg;
    if (FAILED(ctl->get_CoreWebView2(&reg.view)) || !reg.view) return 4;
    reg.handler = new DropMessages(handle);
    if (FAILED(reg.view->add_WebMessageReceived(reg.handler, &reg.token))) { reg.handler->Release(); reg.view->Release(); return 4; }
    tellPage(reg.view, true);
    registered()[window] = reg;
    return 0;
}

void app_drop_forget(void* nativeWindow) {
    HWND window = static_cast<HWND>(nativeWindow);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = registered().find(window);
    if (it == registered().end()) return;
    release(it->second);
    registered().erase(it);
}
