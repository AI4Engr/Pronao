// The window and the queue between the page and CeeLing (see app_native.h): a UI thread that creates the window and pumps its messages (WebView2 requires the creating
// thread to pump), a queue of page calls, and a handle table so CeeLing sees small integers. CeeLing runs the loop: no callback crosses into CeeLing.
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
#else
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

// the icon the build compiled in (scripts/build.sh ICON=<file.png>), absent otherwise
extern const unsigned char app_icon_png[] __attribute__((weak));
extern const unsigned long app_icon_png_size __attribute__((weak));
#endif

namespace {
struct App {
    CallQueue calls;
    std::mutex m;                      // guards `w`, `ready` and `created`
    std::condition_variable cv;
    bool ready = false, created = false;
    webview::webview* w = nullptr;     // valid only while the loop runs, and only touched under `m`
    std::thread ui;
    std::atomic<bool> guard{false};
    std::atomic<bool> track{false};
    bool moving = false;
    std::atomic<void*> hwnd{nullptr};  // the native window (Windows), for modal handling
    std::atomic<bool> fullscreen{false};
#ifdef _WIN32
    LONG_PTR savedStyle = 0;
    RECT savedRect = {};
    bool savedMaximized = false;
#endif
    std::atomic<void*> controller{nullptr};   // the browser engine's controller (Windows)
    std::atomic<int> themeMode{-1};    // app_window_theme mode; -1 = never asked: no change is reported
    std::atomic<int> systemTheme{-1};
    std::atomic<int> modalOwner{0};    // the window this one is modal for
    std::atomic<int> maxRequest{16 << 20}, maxQueued{1000};
    std::shared_ptr<NavRules> nav = std::make_shared<NavRules>();
    void* oldProc = nullptr;
};

std::mutex g_tableMutex;
std::vector<std::shared_ptr<App>> g_table;   // handle - 1 = index; a closed slot is reset, not reused

std::shared_ptr<App> lookup(int handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    return (handle < 1 || static_cast<size_t>(handle) > g_table.size()) ? nullptr : g_table[static_cast<size_t>(handle) - 1];
}

// A modal window is gone (closed by the user, or its loop ended): its owner may be used again. Whichever path comes first does it.
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
// Queues "window" "placement": the RESTORED rectangle in screen coordinates (WINDOWPLACEMENT's is in workspace coordinates, hence the offset when maximized).
void reportPlacement(App* a, HWND h) {
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof wp;
    if (!a->track.load() || a->fullscreen.load() || !GetWindowPlacement(h, &wp) || wp.showCmd == SW_SHOWMINIMIZED) return;
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

// WM_SETTINGCHANGE "ImmersiveColorSet" means the colour scheme may have changed (it arrives more than once). The webview library has just set the title bar to the
// system's scheme in its own handler; a window fixed to light or dark gets its choice back. A real change queues the record "window" "theme" 0|1.
void themeSettingChanged(App* a, HWND h) {
    const int mode = a->themeMode.load();
    if (mode == 1 || mode == 2) app_theme_titlebar(h, mode == 2);
    const int now = app_theme_system();
    if (now < 0 || mode < 0 || now == a->systemTheme.exchange(now)) return;
    std::string record("window\0theme\0", 13);
    record += now == 1 ? "1" : "0";
    record += '\0';
    a->calls.push(std::string(), record, 100);
}

LRESULT CALLBACK guardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = static_cast<App*>(GetPropW(h, L"pronao"));
    if (msg == WM_SETTINGCHANGE && lp != 0 && lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
        const LRESULT result = CallWindowProcW(reinterpret_cast<WNDPROC>(a->oldProc), h, msg, wp, lp);
        themeSettingChanged(a, h);
        return result;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == 0 && lp == 0 && app_menu_command(a->nav->handle, LOWORD(wp))) return 0;      // a menu item (not a control, not an accelerator)
    if (msg == WM_ENTERSIZEMOVE) a->moving = true;
    if (msg == WM_EXITSIZEMOVE) { a->moving = false; reportPlacement(a, h); }
    if (msg == WM_SIZE && !a->moving && (wp == SIZE_MAXIMIZED || wp == SIZE_RESTORED)) reportPlacement(a, h);
    if (msg == WM_CLOSE && a->guard.load()) {
        a->calls.push(std::string(), std::string("window\0close_requested\0", 23), 100);
        return 0;
    }
    // The webview library ends a window's message loop only when the LAST window of the PROCESS is destroyed, so with another window open this window's thread
    // would keep pumping nothing and its queue would never report that it closed.
    if (msg == WM_DESTROY) {
        app_drop_forget(h);
        releaseOwner(a);
        PostQuitMessage(0);                                            // this window's own message loop ends (tests/run_multiwindow.sh removes this line as its control)
    }
    return CallWindowProcW(reinterpret_cast<WNDPROC>(a->oldProc), h, msg, wp, lp);
}
#endif

#ifndef _WIN32
// Linux (GTK 3). The window's place is reported once it has stopped changing (a drag makes many configure events), as the RESTORED rectangle: while the window is
// maximized or full screen the last ordinary rectangle is kept, which is what the application should save. Under Wayland a program cannot know where its window is;
// there x and y are what GTK reports (0, 0), so a program that saves positions should save only the size on that platform.
struct LinuxPlace {
    int x = 0, y = 0, width = 0, height = 0;
    guint timer = 0;
};

gboolean reportLinuxPlacement(gpointer data) {
    GtkWidget* window = static_cast<GtkWidget*>(data);
    App* a = static_cast<App*>(g_object_get_data(G_OBJECT(window), "pronao-app"));
    LinuxPlace* place = static_cast<LinuxPlace*>(g_object_get_data(G_OBJECT(window), "pronao-place"));
    if (!a || !place) return G_SOURCE_REMOVE;
    place->timer = 0;
    GdkWindow* gw = gtk_widget_get_window(window);
    const GdkWindowState state = gw ? gdk_window_get_state(gw) : static_cast<GdkWindowState>(0);
    if (state & (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_WITHDRAWN)) return G_SOURCE_REMOVE;
    const bool maximized = (state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    if (!maximized) {
        gtk_window_get_position(GTK_WINDOW(window), &place->x, &place->y);
        gtk_window_get_size(GTK_WINDOW(window), &place->width, &place->height);
    }
    if (!a->track.load() || place->width <= 0) return G_SOURCE_REMOVE;
    std::string record("window\0placement\0", 17);
    for (long v : {static_cast<long>(place->x), static_cast<long>(place->y), static_cast<long>(place->width), static_cast<long>(place->height), maximized ? 1L : 0L}) {
        record += std::to_string(v);
        record += '\0';
    }
    a->calls.push(std::string(), record, 100);
    return G_SOURCE_REMOVE;
}

gboolean onLinuxConfigure(GtkWidget* window, GdkEvent*, gpointer) {
    LinuxPlace* place = static_cast<LinuxPlace*>(g_object_get_data(G_OBJECT(window), "pronao-place"));
    if (place && !place->timer) place->timer = g_timeout_add(300, reportLinuxPlacement, window);
    return FALSE;
}

gboolean onLinuxWindowState(GtkWidget* window, GdkEvent*, gpointer) {
    return onLinuxConfigure(window, nullptr, nullptr);
}

// the colour scheme of the desktop changed (GTK's theme name or its dark preference): a real change queues the record "window" "theme" 0|1, once the program has asked
void onLinuxThemeNotify(GObject*, GParamSpec*, gpointer window) {
    App* a = static_cast<App*>(g_object_get_data(G_OBJECT(window), "pronao-app"));
    if (!a) return;
    const int mode = a->themeMode.load();
    const int now = app_theme_system();
    if (now < 0 || mode < 0 || now == a->systemTheme.exchange(now)) return;
    std::string record("window\0theme\0", 13);
    record += now == 1 ? "1" : "0";
    record += '\0';
    a->calls.push(std::string(), record, 100);
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
}

int app_open(std::string title, int width, int height, std::string html, std::string url, std::string init_js, int debug) {
#ifdef APP_RELEASE
    debug = 0;      // RELEASE=1 builds never open developer tools or the context menu
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
            w.bind("__pronao_invoke", [raw, &w](const std::string& id, const std::string& req, void*) {
                const int refused = raw->calls.admit(id, req, raw->maxRequest, raw->maxQueued);
                if (refused == 1 || refused == 2) w.resolve(id, 1, refusalJson(refused));
            }, nullptr);
#ifndef _WIN32
            // the page's drop event (src/bridge.js): the paths of the files come from GTK's drag data, not from the page (shim/posix/drop.cpp)
            w.bind("__pronao_drop_native", [raw, &w](const std::string& id, const std::string& req, void*) {
                long x = 0, y = 0;
                if (std::sscanf(req.c_str(), "[%ld,%ld]", &x, &y) == 2) app_drop_deliver(raw->nav->handle, x, y);
                w.resolve(id, 0, "null");
            }, nullptr);
#endif
#ifdef _WIN32
            const std::string site = assetStore()->origin();
            if (!site.empty()) raw->nav->prefixes.push_back(site + "/");
            app_nav_install(w.browser_controller().value(), raw->nav, debug);
            app_assets_install(w.browser_controller().value(), assetStore());
            raw->controller = w.browser_controller().value();
#else
            // Linux (GTK 3, WebKitGTK): the same hooks as above, in shim/posix/nav.cpp and assets.cpp; the program's site is shown under pronao://<host>/
            const std::string site = assetStore()->origin();
            if (!site.empty()) raw->nav->prefixes.push_back(app_web_url(site) + "/");
            app_nav_install(w.browser_controller().value(), raw->nav, debug);
            app_assets_install(w.browser_controller().value(), assetStore());
            raw->controller = w.browser_controller().value();
#endif
            if (!init_js.empty()) w.init(init_js);
#ifdef _WIN32
            if (!url.empty()) w.navigate(url);
#else
            if (!url.empty()) w.navigate(app_web_url(url));
#endif
            else w.set_html(html);
#ifndef _WIN32
            // the close button (and Alt+F4): with app.confirmClose the program is told and decides, as WM_CLOSE is held back on Windows
            GtkWidget* gtkWindow = static_cast<GtkWidget*>(w.window().value());
            raw->hwnd = gtkWindow;
            g_signal_connect(gtkWindow, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer p) -> gboolean {
                App* a = static_cast<App*>(p);
                if (!a->guard.load()) return FALSE;
                a->calls.push(std::string(), std::string("window\0close_requested\0", 23), 100);
                return TRUE;
            }), raw);
            if (app_icon_png && app_icon_png_size > 0) {                 // the program's icon (ICON= in scripts/build.sh), if it was given
                GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
                if (loader && gdk_pixbuf_loader_write(loader, app_icon_png, app_icon_png_size, nullptr) && gdk_pixbuf_loader_close(loader, nullptr)) {
                    if (GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader)) gtk_window_set_icon(GTK_WINDOW(gtkWindow), pixbuf);
                }
                if (loader) g_object_unref(loader);
            }
            g_object_set_data(G_OBJECT(gtkWindow), "pronao-app", raw);
            g_object_set_data_full(G_OBJECT(gtkWindow), "pronao-place", new LinuxPlace(), +[](gpointer p) { delete static_cast<LinuxPlace*>(p); });
            g_signal_connect(gtkWindow, "configure-event", G_CALLBACK(onLinuxConfigure), nullptr);
            g_signal_connect(gtkWindow, "window-state-event", G_CALLBACK(onLinuxWindowState), nullptr);
            if (GtkSettings* settings = gtk_settings_get_default()) {
                g_signal_connect_object(settings, "notify::gtk-theme-name", G_CALLBACK(onLinuxThemeNotify), gtkWindow, static_cast<GConnectFlags>(0));
                g_signal_connect_object(settings, "notify::gtk-application-prefer-dark-theme", G_CALLBACK(onLinuxThemeNotify), gtkWindow, static_cast<GConnectFlags>(0));
            }
#endif
#ifdef _WIN32
            HWND hwnd = static_cast<HWND>(w.window().value());
            SetPropW(hwnd, L"pronao", raw);
            raw->hwnd = hwnd;
            // the program's icon (resource 1, ICON= in scripts/build.sh) for the title bar, Alt+Tab and the taskbar
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
    if (!app || !app->calls.answer(id)) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    // resolve() posts to the UI thread itself, so calling it from this thread is safe
    return app->w != nullptr && app->w->resolve(id, status, json).ok() ? 1 : 0;
}

int app_eval(int handle, std::string js) {
    return onUi(handle, [js](webview::webview& w) { w.eval(js); });
}

int app_terminate(int handle) {
    // Not terminate() from this thread: on Windows that is PostQuitMessage(0), which posts WM_QUIT to the CALLING thread, so the UI loop would never stop.
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
        if (state == 3) { if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        else if (state == 4) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        else if (state == 5) ShowWindow(hwnd, SW_HIDE);
        else if (state == 6) { ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(hwnd); }
        else ShowWindow(hwnd, state == 1 ? SW_MINIMIZE : state == 2 ? SW_MAXIMIZE : SW_RESTORE);
    });
#else
    return app_linux_state(handle, state);
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
    out = c.primary + c.others;
    return out.empty() ? 4 : 0;
#else
    for (int h = 1;; h++) {                                  // the monitors are asked through any window that is open (GTK is used on its own thread)
        if (!lookup(h)) { if (h > static_cast<int>(g_table.size())) return 4; continue; }
        if (app_linux_monitors(h, out) == 0) return 0;
    }
#endif
}

int app_window_theme(int handle, int mode) {
#ifdef _WIN32
    if (mode < 0 || mode > 2) return 5;
    auto app = lookup(handle);
    if (!app) return 4;
    int rc = 4;
    const int ran = app_ui_call(handle, [&](void* window) {
        const int system = app_theme_system();
        app->systemTheme = system;
        app->themeMode = mode;
        app_theme_titlebar(window, mode == 2 || (mode == 0 && system == 1));
        rc = app_theme_page(app->controller.load(), mode);
    });
    return ran ? rc : 4;
#else
    if (mode < 0 || mode > 2) return 5;
    auto app = lookup(handle);
    if (!app) return 4;
    app->systemTheme = app_theme_system();                 // (before the first change this program makes: app_theme_system keeps answering with the desktop's own setting)
    app->themeMode = mode;
    return app_linux_theme(handle, mode);
#endif
}

int app_drop_accept(int handle, int on) {
#ifdef _WIN32
    if (on != 0 && on != 1) return 5;
    auto app = lookup(handle);
    if (!app) return 4;
    int rc = 4;
    const int ran = app_ui_call(handle, [&](void* window) { rc = app_drop_set(window, app->controller.load(), handle, on == 1); });
    return ran ? rc : 4;
#else
    if (on != 0 && on != 1) return 5;
    auto app = lookup(handle);
    if (!app) return 4;
    int rc = 4;
    const int ran = app_ui_call(handle, [&](void* window) { rc = app_drop_set(window, app->controller.load(), handle, on == 1); });
    return ran ? rc : 4;
#endif
}

int app_window_placement(int handle, int x, int y, int width, int height, int maximized) {
#ifdef _WIN32
    return onUi(handle, [=](webview::webview& w) {
        HWND hwnd = static_cast<HWND>(w.window().value());
        SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);      // the RESTORED rectangle, in screen coordinates
        if (maximized) ShowWindow(hwnd, SW_MAXIMIZE);
    });
#else
    return app_linux_placement(handle, x, y, width, height, maximized);
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
    return app_linux_rect(handle, out);
#endif
}

int app_window_style(int handle, int what, int on) {
#ifdef _WIN32
    auto app = lookup(handle);
    if (!app) return 4;
    return onUi(handle, [=](webview::webview& w) {
        HWND hwnd = static_cast<HWND>(w.window().value());
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (what == 1) {
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
        } else if (what == 2) {
            SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (what == 3) {
            if (app->fullscreen.load()) return;
            style = on ? (style & ~WS_CAPTION) : (style | WS_CAPTION);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        } else if (what == 4) {
            if (app->fullscreen.load()) return;
            style = on ? (style | WS_THICKFRAME | WS_MAXIMIZEBOX) : (style & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    });
#else
    return app_linux_style(handle, what, on);
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
    return app_linux_drag(handle, edge);
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
    auto c = lookup(child);
    auto o = lookup(owner);
    if (!c || !o || child == owner) return 4;
    if (on) c->modalOwner = owner; else c->modalOwner.exchange(0);
    return app_linux_modal(child, owner, on);
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
#ifndef _WIN32
    for (auto& p : prefixes) p = app_web_url(p);        // the program's own site is shown under pronao://<host>/ here
#endif
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
    app_tray_forget(handle);
#ifndef _WIN32
    app_drop_closed(handle);
#endif
    app_terminate(handle);
    if (app->ui.joinable()) app->ui.join();
    app->calls.close();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table[static_cast<size_t>(handle) - 1].reset();
    return 1;
}
