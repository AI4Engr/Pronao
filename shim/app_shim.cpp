// The window and the queue between the page and CeeLing (see app_native.h): a UI thread that creates the window and pumps its
// messages (WebView2 requires the creating thread to pump), a queue of calls from JavaScript, and a handle table so CeeLing sees
// small integers. CeeLing runs the loop (app_next_call, then app_return), so no callback ever crosses into CeeLing.
#include "app_native.h"
#include "app_shim_internal.h"
#include "assets.h"
#include "queue.h"

#include <atomic>
#include <cstdio>
#include <exception>
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
    std::atomic<bool> track{false};    // report where the window is after the user moves or resizes it (app_window_track)
    bool moving = false;               // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE (UI thread only)
    std::atomic<void*> hwnd{nullptr};  // the native window (Windows), for the other windows' modal handling
    // the layout switches (app_window_style); the first four are UI-thread business, the flags are read by reportPlacement
    std::atomic<bool> fullscreen{false};
#ifdef _WIN32
    LONG_PTR savedStyle = 0;           // the style and the rectangle a window had before it went full screen
    RECT savedRect = {};
    bool savedMaximized = false;
#endif
    std::atomic<int> modalOwner{0};    // the window this one is modal for (its owner is disabled until this one closes)
    std::atomic<int> maxRequest{16 << 20}, maxQueued{1000};
    std::shared_ptr<NavRules> nav = std::make_shared<NavRules>();   // (nav->handle is set by app_open)
    void* oldProc = nullptr;
};

std::mutex g_tableMutex;
std::vector<std::shared_ptr<App>> g_table;   // handle - 1 = index; a closed slot is reset, not reused

std::shared_ptr<App> lookup(int handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    return (handle < 1 || static_cast<size_t>(handle) > g_table.size()) ? nullptr : g_table[static_cast<size_t>(handle) - 1];
}

// a modal window is gone, however it went (the user's close button: WM_DESTROY; the application's terminate(): the loop just ends): its owner may be used again.
// Whichever comes first does it, the other finds nothing to do.
void releaseOwner(App* a) {
    const int owner = a->modalOwner.exchange(0);
    if (owner != 0) app_window_modal_ended(owner);
}

void ended(App* a, bool created) {
    releaseOwner(a);
    { std::lock_guard<std::mutex> lock(a->m); a->w = nullptr; a->ready = true; a->created = created; }
    a->calls.close();
    a->cv.notify_all();
}

#ifdef _WIN32
// where the window is, as the record "window" "placement" x y width height maximized, in SCREEN coordinates (the same ones app_monitors uses), and the
// RESTORED rectangle: a maximized window keeps its normal size. (WINDOWPLACEMENT's own rectangle is in workspace coordinates, relative to the work area of
// the primary monitor, which is why that is converted here and not handed on.)
void reportPlacement(App* a, HWND h) {
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof wp;
    if (!a->track.load() || a->fullscreen.load() || !GetWindowPlacement(h, &wp) || wp.showCmd == SW_SHOWMINIMIZED) return;      // (full screen is not a place worth keeping)
    RECT r = wp.rcNormalPosition;
    if (wp.showCmd == SW_SHOWMAXIMIZED) {
        MONITORINFO info = {};
        info.cbSize = sizeof info;
        if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) OffsetRect(&r, info.rcWork.left, info.rcWork.top);
    } else if (!GetWindowRect(h, &r)) {
        return;
    }
    std::string record("window\0placement\0", 17);
    for (long v : {r.left, r.top, r.right - r.left, r.bottom - r.top, wp.showCmd == SW_SHOWMAXIMIZED ? 1L : 0L}) {
        record += std::to_string(v);
        record += '\0';
    }
    a->calls.push(std::string(), record, 100);
}

LRESULT CALLBACK guardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = static_cast<App*>(GetPropW(h, L"app_cling"));
    if (msg == WM_COMMAND && HIWORD(wp) == 0 && lp == 0 && app_menu_command(a->nav->handle, LOWORD(wp))) return 0;      // a menu item (not a control, not an accelerator)
    if (msg == WM_ENTERSIZEMOVE) a->moving = true;
    if (msg == WM_EXITSIZEMOVE) { a->moving = false; reportPlacement(a, h); }
    if (msg == WM_SIZE && !a->moving && (wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED)) reportPlacement(a, h);     // (the maximize button, a double click on the title)
    if (msg == WM_CLOSE && a->guard.load()) {
        a->calls.push(std::string(), std::string("window\0close_requested\0", 23), 100);
        return 0;
    }
    // The webview library ends a window's message loop only when the LAST window of the PROCESS is destroyed (a process-wide count), so with
    // another window still open this window's thread would keep pumping nothing and its queue would never report that it closed.
    if (msg == WM_DESTROY) {
        releaseOwner(a);                                               // a modal window is gone: its owner may be used again
        PostQuitMessage(0);                                            // this window's own message loop ends (tests/run_multiwindow.sh removes this line as its control)
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

int app_open(std::string title, int width, int height, std::string html, std::string url, std::string init_js, int debug) {
#ifdef APP_RELEASE
    debug = 0;      // a release build (scripts/build.sh with RELEASE=1) ignores the argument: no developer tools, no context menu, whatever the program asks
#endif
    auto app = std::make_shared<App>();
    app->calls.setNotify(&waitSet());
    int handle = 0;
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_table.push_back(app);
        handle = static_cast<int>(g_table.size());
    }
    App* raw = app.get();
    app->nav->handle = handle;
    app->ui = std::thread([raw, title, width, height, html, url, init_js, debug] {
        try {
            webview::webview w(debug != 0, nullptr);
            w.set_title(title);
            w.set_size(width, height, WEBVIEW_HINT_NONE);
            w.bind("__cling_invoke", [raw, &w](const std::string& id, const std::string& req, void*) {
                const int refused = raw->calls.admit(id, req, raw->maxRequest, raw->maxQueued);
                if (refused == 1 || refused == 2) w.resolve(id, 1, refusalJson(refused));
            }, nullptr);
#ifdef _WIN32
            const std::string site = assetStore()->origin();
            if (!site.empty()) raw->nav->prefixes.push_back(site + "/");        // (no lock needed yet: nothing else has the window)
            app_nav_install(w.browser_controller().value(), raw->nav, debug);
            app_assets_install(w.browser_controller().value(), assetStore());
#endif
            if (!init_js.empty()) w.init(init_js);
            if (!url.empty()) w.navigate(url);
            else w.set_html(html);
#ifdef _WIN32
            HWND hwnd = static_cast<HWND>(w.window().value());
            SetPropW(hwnd, L"app_cling", raw);
            raw->hwnd = hwnd;
            // the program's icon (resource 1, from scripts/build.sh ICON=...) for the title bar, Alt+Tab and the taskbar; none linked in: the default stays
            for (int big = 0; big < 2; big++) {
                const int cx = GetSystemMetrics(big ? SM_CXICON : SM_CXSMICON), cy = GetSystemMetrics(big ? SM_CYICON : SM_CYSMICON);
                if (HANDLE icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR))
                    SendMessageW(hwnd, WM_SETICON, big ? ICON_BIG : ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            }
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

int app_wait_any(std::vector<int> handles, int timeout_ms, int& handle) {
    static std::atomic<int> last{0};
    return waitAny<App>(handles, [](int h) { return lookup(h); }, last, timeout_ms, handle);
}

int app_push(int handle, std::string id, std::string request, int max_queued) {
    auto app = lookup(handle);
    return app ? app->calls.push(std::move(id), std::move(request), max_queued) : -1;
}

int app_return(int handle, std::string id, int status, std::string json) {
    auto app = lookup(handle);
    if (!app || !app->calls.answer(id)) return 0;     // (a page call is answered once: a late second answer is dropped)
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
        if (state == 3) { if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }     // (a minimized window comes back first)
        else if (state == 4) PostMessageW(hwnd, WM_CLOSE, 0, 0);       // as if the user pressed the X: the close guard, if any, is asked
        else if (state == 5) ShowWindow(hwnd, SW_HIDE);                // hidden: not on screen and not on the taskbar; the window and its page keep running
        else if (state == 6) { ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(hwnd); }
        else ShowWindow(hwnd, state == 1 ? SW_MINIMIZE : state == 2 ? SW_MAXIMIZE : SW_RESTORE);
    });
#else
    return (void)handle, (void)state, -1;
#endif
}

int app_monitors(std::string& out) {
#ifdef _WIN32
    struct Collected { std::string primary, others; } c;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO info = {};
        info.cbSize = sizeof info;
        if (!GetMonitorInfoW(m, &info)) return TRUE;
        auto* c = reinterpret_cast<Collected*>(p);
        std::string one;
        for (long v : {info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom}) { one += std::to_string(v); one += '\0'; }
        ((info.dwFlags & MONITORINFOF_PRIMARY) ? c->primary : c->others) += one;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    out = c.primary + c.others;                                      // (the primary monitor first)
    return out.empty() ? 4 : 0;
#else
    return (void)out, -1;
#endif
}

int app_window_placement(int handle, int x, int y, int width, int height, int maximized) {
#ifdef _WIN32
    return onUi(handle, [=](webview::webview& w) {
        HWND hwnd = static_cast<HWND>(w.window().value());
        SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);      // the restored rectangle, in screen coordinates ...
        if (maximized) ShowWindow(hwnd, SW_MAXIMIZE);                                         // ... which a maximized window goes back to when restored
    });
#else
    return (void)handle, (void)x, (void)y, (void)width, (void)height, (void)maximized, -1;
#endif
}

int app_window_track(int handle, int on) {
    auto app = lookup(handle);
    if (!app) return 0;
    app->track = on != 0;
    return 1;
}

// ---- layout: where the window is and how it is dressed (Windows) ----

int app_window_rect(int handle, std::string& out) {
#ifdef _WIN32
    out.clear();
    auto app = lookup(handle);
    if (!app) return 4;
    std::string text;
    const int ran = app_ui_call(handle, [&](void* window) {                       // (waits until it has run, so `text` is still there to write to)
        HWND hwnd = static_cast<HWND>(window);
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof wp;
        RECT r = {};
        if (!hwnd || !GetWindowRect(hwnd, &r) || !GetWindowPlacement(hwnd, &wp)) return;
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        const int state = (wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0) | (wp.showCmd == SW_SHOWMINIMIZED ? 2 : 0) | (app->fullscreen.load() ? 4 : 0) |
                          ((ex & WS_EX_TOPMOST) ? 8 : 0) | ((style & WS_CAPTION) != WS_CAPTION ? 16 : 0) | ((style & WS_THICKFRAME) ? 0 : 32) | (IsWindowVisible(hwnd) ? 0 : 64);
        text = std::to_string(r.left) + " " + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + " " + std::to_string(r.bottom - r.top) + " " + std::to_string(state);
    });
    if (!ran || text.empty()) return 4;
    out = text;
    return 0;
#else
    return (void)handle, out.clear(), -1;
#endif
}

int app_window_style(int handle, int what, int on) {
#ifdef _WIN32
    auto app = lookup(handle);
    if (!app) return 4;
    return onUi(handle, [=](webview::webview& w) {
        HWND hwnd = static_cast<HWND>(w.window().value());
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (what == 1) {                                                          // full screen: no frame, over the whole monitor; back to what it was
            if (on && !app->fullscreen.load()) {
                WINDOWPLACEMENT wp = {};
                wp.length = sizeof wp;
                GetWindowPlacement(hwnd, &wp);
                app->savedMaximized = wp.showCmd == SW_SHOWMAXIMIZED;
                if (app->savedMaximized) ShowWindow(hwnd, SW_RESTORE);
                app->savedStyle = style;
                GetWindowRect(hwnd, &app->savedRect);
                MONITORINFO info = {};
                info.cbSize = sizeof info;
                if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return;
                app->fullscreen = true;
                SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
                SetWindowPos(hwnd, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
            } else if (!on && app->fullscreen.load()) {
                app->fullscreen = false;
                SetWindowLongPtrW(hwnd, GWL_STYLE, app->savedStyle);
                SetWindowPos(hwnd, nullptr, app->savedRect.left, app->savedRect.top, app->savedRect.right - app->savedRect.left, app->savedRect.bottom - app->savedRect.top, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
                if (app->savedMaximized) ShowWindow(hwnd, SW_MAXIMIZE);
            }
        } else if (what == 2) {                                                   // always on top
            SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (what == 3) {                                                   // frameless: no title bar and no border line; the resize border stays if the window may be resized
            if (app->fullscreen.load()) return;
            style = on ? (style & ~WS_CAPTION) : (style | WS_CAPTION);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (what == 4) {                                                   // may the user resize it (and maximize it)
            if (app->fullscreen.load()) return;
            style = on ? (style | WS_THICKFRAME | WS_MAXIMIZEBOX) : (style & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
#else
    return (void)handle, (void)what, (void)on, -1;
#endif
}

int app_window_drag(int handle, int edge) {
#ifdef _WIN32
    return onUi(handle, [edge](webview::webview& w) {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000)) return;                           // only while the left button is held: a page cannot start a move out of nothing
        HWND hwnd = static_cast<HWND>(w.window().value());
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, edge == 0 ? HTCAPTION : static_cast<WPARAM>(edge), 0);          // returns when the button is released
    });
#else
    return (void)handle, (void)edge, -1;
#endif
}

int app_window_modal(int child, int owner, int on) {
#ifdef _WIN32
    auto c = lookup(child);
    auto o = lookup(owner);
    if (!c || !o || child == owner || !o->hwnd.load()) return 4;
    HWND ownerWindow = static_cast<HWND>(o->hwnd.load());
    if (on) {
        c->modalOwner = owner;
        EnableWindow(ownerWindow, FALSE);
        onUi(child, [](webview::webview& w) { SetForegroundWindow(static_cast<HWND>(w.window().value())); });
    } else if (c->modalOwner.exchange(0) == owner) {
        EnableWindow(ownerWindow, TRUE);
    }
    return 0;
#else
    return (void)child, (void)owner, (void)on, -1;
#endif
}

void app_window_modal_ended(int owner) {
#ifdef _WIN32
    auto o = lookup(owner);
    if (!o || !o->hwnd.load()) return;
    HWND ownerWindow = static_cast<HWND>(o->hwnd.load());
    EnableWindow(ownerWindow, TRUE);
    SetForegroundWindow(ownerWindow);
#else
    (void)owner;
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

int app_pending(int handle, std::string call) {
    auto app = lookup(handle);
    return app && app->calls.waiting(call) ? 1 : 0;
}

int app_limits(int handle, int max_request, int max_queued) {
    auto app = lookup(handle);
    if (app) { app->maxRequest = max_request; app->maxQueued = max_queued; }
    return app ? 1 : 0;
}

int app_navigation_allow(int handle, std::vector<std::string> prefixes) {
    auto app = lookup(handle);
    if (app) { std::lock_guard<std::mutex> lock(app->nav->m); app->nav->prefixes = std::move(prefixes); }
    return app ? 1 : 0;
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

int app_queue_entry_new() {
    auto app = std::make_shared<App>();
    app->calls.setNotify(&waitSet());
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table.push_back(app);
    return static_cast<int>(g_table.size());
}

void app_queue_entry_end(int handle) {
    auto app = lookup(handle);
    if (!app) return;
    app->calls.close();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table[static_cast<size_t>(handle) - 1].reset();
}

int app_close(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    app_watch_stop_all(handle);
    app_menu_forget(handle);
    app_tray_forget(handle);                       // (a tray: the icon is removed and its thread ends; anything else: nothing)
    app_terminate(handle);
    if (app->ui.joinable()) app->ui.join();
    app->calls.close();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table[static_cast<size_t>(handle) - 1].reset();
    return 1;
}
