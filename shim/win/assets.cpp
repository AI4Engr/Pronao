// Windows: answers the window's requests for the application's own files from memory (see shim/assets.h). Every request passes through here; only those under the
// configured origin are answered.
#include <shlwapi.h>
#include <WebView2.h>

#include <atomic>
#include <functional>

#include "../app_native.h"
#include "../app_shim_internal.h"
#include "../assets.h"
#include "util.h"

namespace {
struct Handler : ICoreWebView2WebResourceRequestedEventHandler {
    std::function<void(ICoreWebView2WebResourceRequestedEventArgs*)> fn;
    std::atomic<ULONG> refs{1};
    explicit Handler(std::function<void(ICoreWebView2WebResourceRequestedEventArgs*)> f) : fn(std::move(f)) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        *p = (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_ICoreWebView2WebResourceRequestedEventHandler)) ? this : nullptr;
        if (*p) AddRef();
        return *p ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) override { fn(a); return S_OK; }
};
}

void app_assets_install(void* controller, std::shared_ptr<AssetStore> store) {
    ICoreWebView2* view = nullptr;
    ICoreWebView2_2* view2 = nullptr;
    ICoreWebView2Environment* env = nullptr;
    if (FAILED(static_cast<ICoreWebView2Controller*>(controller)->get_CoreWebView2(&view)) || !view || FAILED(view->QueryInterface(IID_ICoreWebView2_2, reinterpret_cast<void**>(&view2))) ||
        FAILED(view2->get_Environment(&env)) || !env) return;
    view->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken token;
    Handler* h = new Handler([store, env](ICoreWebView2WebResourceRequestedEventArgs* a) {
        ICoreWebView2WebResourceRequest* request = nullptr;
        if (FAILED(a->get_Request(&request)) || !request) return;
        LPWSTR uri = nullptr, method = nullptr;
        request->get_Uri(&uri);
        request->get_Method(&method);
        int status = 0;
        std::string headers;
        std::shared_ptr<const std::string> body;
        const bool mine = uri && method && store->respond(narrow(method), narrow(uri), status, headers, body);
        CoTaskMemFree(uri);
        CoTaskMemFree(method);
        if (!mine) return;
        IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(body->data()), static_cast<UINT>(body->size()));
        ICoreWebView2WebResourceResponse* response = nullptr;
        if (stream && SUCCEEDED(env->CreateWebResourceResponse(stream, status, status == 200 ? L"OK" : status == 404 ? L"Not Found" : L"Method Not Allowed", widen(headers).c_str(), &response)) && response) {
            a->put_Response(response);
            response->Release();
        }
        if (stream) stream->Release();
    });
    view->add_WebResourceRequested(h, &token);
}
