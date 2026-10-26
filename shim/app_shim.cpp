// The window and the queue between the page and CeeLing (see app_native.h): a UI thread that creates the window and pumps its
// messages (WebView2 requires the creating thread to pump), a queue of calls from JavaScript, and a handle table so CeeLing sees
// small integers. CeeLing runs the loop (app_next_call, then app_return), so no callback ever crosses into CeeLing.
#include "app_native.h"
#include "app_shim_internal.h"
#include "queue.h"

#include <atomic>
#include <cstdio>
#include <exception>
#include <typeinfo>
#include <memory>
#include <thread>
#include <vector>

#include "webview/webview.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
struct App {
    CallQueue calls;                   // closed when the message loop has ended
    std::mutex m;                      // guards `w`, `ready` and `created`
    std::condition_variable cv;
    bool ready = false, created = false;
    webview::webview* w = nullptr;     // valid only while the loop runs, and only touched under `m`
    std::thread ui;
    std::atomic<bool> guard{false};    // closing the window asks CeeLing instead of closing
    void* oldProc = nullptr;
};

std::mutex g_tableMutex;
std::vector<std::shared_ptr<App>> g_table;   // handle - 1 = index; a closed slot is reset, not reused

std::shared_ptr<App> lookup(int handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    return (handle < 1 || static_cast<size_t>(handle) > g_table.size()) ? nullptr : g_table[static_cast<size_t>(handle) - 1];
}

void ended(App* a, bool created) {
    { std::lock_guard<std::mutex> lock(a->m); a->w = nullptr; a->ready = true; a->created = created; }
    a->calls.close();
    a->cv.notify_all();
}

#ifdef _WIN32
LRESULT CALLBACK guardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = static_cast<App*>(GetPropW(h, L"app_cling"));
    if (msg == WM_CLOSE && a->guard.load()) {
        a->calls.push(std::string(), std::string("window\0close_requested\0", 23), 100);
        return 0;
    }
    return CallWindowProcW(reinterpret_cast<WNDPROC>(a->oldProc), h, msg, wp, lp);
}
#endif

// runs `op` on the UI thread (without waiting); 0 if the window is gone
int onUi(int handle, std::function<void(webview::webview&)> op) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    webview::webview* w = app->w;
    return w != nullptr && w->dispatch([w, op] { op(*w); }).ok() ? 1 : 0;
}
}  // namespace

int app_open(std::string title, int width, int height, std::string html, std::string init_js, int debug) {
    auto app = std::make_shared<App>();
    int handle = 0;
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_table.push_back(app);
        handle = static_cast<int>(g_table.size());
    }
    App* raw = app.get();
    app->ui = std::thread([raw, title, width, height, html, init_js, debug] {
        try {
            webview::webview w(debug != 0, nullptr);
            w.set_title(title);
            w.set_size(width, height, WEBVIEW_HINT_NONE);
            w.bind("__cling_invoke", [raw](const std::string& id, const std::string& req, void*) {
                if (!id.empty()) raw->calls.push(id, req, 1 << 30);   // an empty id marks the shim's own records: never from a page
            }, nullptr);
            if (!init_js.empty()) w.init(init_js);
            w.set_html(html);
#ifdef _WIN32
            HWND hwnd = static_cast<HWND>(w.window().value());
            SetPropW(hwnd, L"app_cling", raw);
            raw->oldProc = reinterpret_cast<void*>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(guardProc)));
#endif
            { std::lock_guard<std::mutex> lock(raw->m); raw->w = &w; raw->created = raw->ready = true; }
            raw->cv.notify_all();
            w.run();
            ended(raw, true);              // forget the pointer BEFORE `w` is destroyed at the end of this scope
        } catch (const webview::exception& e) {
            std::fprintf(stderr, "app_shim: webview error code %d\n", static_cast<int>(e.error().code()));
            ended(raw, false);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "app_shim: window creation failed: %s\n", e.what());
            ended(raw, false);
        } catch (...) {
            std::fprintf(stderr, "app_shim: window creation failed (unknown error)\n");
            ended(raw, false);
        }
    });
    std::unique_lock<std::mutex> lock(app->m);
    app->cv.wait(lock, [&] { return app->ready; });
    if (!app->created) {
        lock.unlock();
        app->ui.join();
        std::lock_guard<std::mutex> t(g_tableMutex);
        g_table[static_cast<size_t>(handle) - 1].reset();
        return 0;
    }
    return handle;
}

int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request) {
    auto app = lookup(handle);
    return app ? app->calls.pop(timeout_ms, id, request) : 0;
}

int app_push(int handle, std::string id, std::string request, int max_queued) {
    auto app = lookup(handle);
    return app ? app->calls.push(std::move(id), std::move(request), max_queued) : -1;
}

int app_return(int handle, std::string id, int status, std::string json) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    // resolve() posts to the UI thread itself, so calling it from this thread is safe
    return app->w != nullptr && app->w->resolve(id, status, json).ok() ? 1 : 0;
}

int app_eval(int handle, std::string js) {
    return onUi(handle, [js](webview::webview& w) { w.eval(js); });
}

int app_terminate(int handle) {
    // NOT terminate() from this thread: on Windows that is PostQuitMessage(0), which posts WM_QUIT to the CALLING thread's queue
    // (the wrong one from here), so the UI loop would never stop. onUi runs it on the UI thread.
    return onUi(handle, [](webview::webview& w) { w.terminate(); });
}

int app_window_title(int handle, std::string title) {
    return onUi(handle, [title](webview::webview& w) { w.set_title(title); });
}

int app_window_size(int handle, int width, int height) {
    return onUi(handle, [width, height](webview::webview& w) { w.set_size(width, height, WEBVIEW_HINT_NONE); });
}

int app_window_state(int handle, int state) {
#ifdef _WIN32
    return onUi(handle, [state](webview::webview& w) {
        HWND hwnd = static_cast<HWND>(w.window().value());
        if (state == 3) SetForegroundWindow(hwnd);
        else ShowWindow(hwnd, state == 1 ? SW_MINIMIZE : state == 2 ? SW_MAXIMIZE : SW_RESTORE);
    });
#else
    return (void)handle, (void)state, -1;
#endif
}

int app_window_guard(int handle, int on) {
    auto app = lookup(handle);
    if (app) app->guard = on != 0;
#ifdef _WIN32
    return app ? 1 : 0;
#else
    return -1;
#endif
}

int app_ui_call(int handle, const std::function<void(void*)>& fn) {
    auto app = lookup(handle);
    if (!app) return 0;
    struct State { std::mutex m; std::condition_variable cv; bool started = false, done = false, cancelled = false; };
    auto st = std::make_shared<State>();
    {
        std::lock_guard<std::mutex> lock(app->m);
        webview::webview* w = app->w;
        if (w == nullptr) return 0;
        const bool queued = w->dispatch([st, fn, w] {
            { std::lock_guard<std::mutex> l(st->m); if (st->cancelled) { st->done = true; st->cv.notify_all(); return; } st->started = true; }
            auto win = w->window();
            fn(win.ok() ? win.value() : nullptr);
            { std::lock_guard<std::mutex> l(st->m); st->done = true; }
            st->cv.notify_all();
        }).ok();
        if (!queued) return 0;
    }
    std::unique_lock<std::mutex> lock(st->m);
    while (!st->done) {
        st->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return st->done; });
        if (st->done || st->started) continue;   // running (a dialog may stay open as long as the user likes): wait for it
        lock.unlock();                           // not started: if the window has closed meanwhile it never will
        const bool closed = app->calls.closed();
        lock.lock();
        if (closed && !st->started) { st->cancelled = true; return 0; }
    }
    return 1;
}

int app_close(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    app_watch_stop_all(handle);
    app_terminate(handle);
    if (app->ui.joinable()) app->ui.join();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table[static_cast<size_t>(handle) - 1].reset();
    return 1;
}
