// Windows: what the window may navigate to (see NavRules in app_shim_internal.h). Decided here, at the moment the browser engine asks,
// because only the engine knows about frames, redirects and scripts; CeeLing supplies the list of allowed prefixes.
#include <atomic>

#include "../app_native.h"
#include "../app_shim_internal.h"
#include <WebView2.h>
#include "util.h"
#include "menu_native.h"

namespace {
template <class H, class A>
struct Handler : H {
    std::function<void(A*)> fn;
    const IID& iid;
    std::atomic<ULONG> refs{1};
    Handler(const IID& i, std::function<void(A*)> f) : fn(std::move(f)), iid(i) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        *p = (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, iid)) ? this : nullptr;
        if (*p) AddRef();
        return *p ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, A* a) override { fn(a); return S_OK; }
};
}

void app_nav_install(void* controller, std::shared_ptr<NavRules> rules, int debug) {
    ICoreWebView2* view = nullptr;
    if (FAILED(static_cast<ICoreWebView2Controller*>(controller)->get_CoreWebView2(&view)) || !view) return;
    EventRegistrationToken token;
    // the same rule for the window itself and for every frame in it: an iframe pointing at a stranger's site would get the bridge too
    auto guard = [rules](bool frame) {
        return new Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2NavigationStartingEventArgs>(
            IID_ICoreWebView2NavigationStartingEventHandler, [rules, frame](ICoreWebView2NavigationStartingEventArgs* a) {
                LPWSTR raw = nullptr;
                a->get_Uri(&raw);
                const std::string uri = raw ? narrow(raw) : std::string();
                CoTaskMemFree(raw);
                std::lock_guard<std::mutex> lock(rules->m);
                bool ok = frame ? (uri == "about:blank" || uri == "about:srcdoc") : (!rules->loaded || uri == rules->first);   // the first document is the application's
                if (!frame && !rules->loaded) { rules->first = uri; rules->loaded = true; }
                for (const auto& p : rules->prefixes) ok = ok || uri.compare(0, p.size(), p) == 0;
                if (ok) return;
                a->put_Cancel(TRUE);
                BOOL byUser = FALSE;
                a->get_IsUserInitiated(&byUser);
                std::string record = "window"; record += '\0'; record += "navigation_blocked"; record += '\0'; record += uri.substr(0, 2100); record += '\0';
                record += byUser ? "1" : "0"; record += '\0';
                app_push(rules->handle, std::string(), record, 100);   // the application sees it (App.openLinksInBrowser can act on it)
            });
    };
    const int handle = rules->handle;
    app_accelerators_install(controller, [handle](UINT vk, UINT mods, bool repeat) { return app_menu_shortcut(handle, vk, mods, repeat); });
    view->add_NavigationStarting(guard(false), &token);
    view->add_FrameNavigationStarting(guard(true), &token);
    view->add_NewWindowRequested(new Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2NewWindowRequestedEventArgs>(
        IID_ICoreWebView2NewWindowRequestedEventHandler, [rules](ICoreWebView2NewWindowRequestedEventArgs* a) {
            a->put_Handled(TRUE);                                      // (a link with target=_blank, window.open): never a second window of ours
            LPWSTR raw = nullptr;
            a->get_Uri(&raw);
            const std::string uri = raw ? narrow(raw) : std::string();
            CoTaskMemFree(raw);
            BOOL byUser = FALSE;
            a->get_IsUserInitiated(&byUser);
            std::string record = "window"; record += '\0'; record += "navigation_blocked"; record += '\0'; record += uri.substr(0, 2100); record += '\0';
            record += byUser ? "1" : "0"; record += '\0';
            app_push(rules->handle, std::string(), record, 100);
        }), &token);
    ICoreWebView2Settings* settings = nullptr;                    // no right-click menu (save as, print, view source) in a release build
    if (SUCCEEDED(view->get_Settings(&settings)) && settings) settings->put_AreDefaultContextMenusEnabled(debug ? TRUE : FALSE);
}
