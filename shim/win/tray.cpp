// Windows: the tray (notification area) icon and its menu (see app_tray_open in app_native.h).
// The icon belongs to a hidden window with its own thread and message loop; it has a handle in the same table as the windows, with only a queue, so the one
// main loop of the program waits for it together with the windows. What reaches the queue: "tray" click | double, and "menu" id for a chosen item of the tray
// menu (an id the application gave). What the menu contains, and every limit on it, is decided in src/menu.clg.
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "../app_native.h"
#include "../app_shim_internal.h"
#include "menu_native.h"
#include "util.h"

namespace {
const UINT kCallback = WM_APP + 1;         // the shell's messages about the icon
const UINT kUpdate = WM_APP + 2;           // the application changed the tooltip
const UINT kArm = WM_APP + 3;              // a notification was shown by a notification-only icon: start the timer that takes the icon away (a timer belongs to the thread of its window)
const UINT_PTR kHideTimer = 1;             // a notification-only icon goes away this long after its notification was shown
const UINT kHideAfterMs = 15000;
const wchar_t* kClass = L"app-cling-tray";

struct Tray {
    int handle = 0;
    HWND hwnd = nullptr;
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    bool added = false;
    bool transient = false;                // a notification-only icon: it is in the notification area only while a notification is up
    std::atomic<bool> shown{false};        // the icon is in the notification area now
    std::string notifyId;                  // what the application called the notification that is up
    std::wstring tooltip;
    std::shared_ptr<NativeMenu> menu;
    UINT taskbarCreated = 0;
};

std::mutex g_trayMutex;
std::map<int, std::shared_ptr<Tray>> g_trays;

std::shared_ptr<Tray> trayOf(int handle) {
    std::lock_guard<std::mutex> lock(g_trayMutex);
    auto it = g_trays.find(handle);
    return it == g_trays.end() ? nullptr : it->second;
}

void tell(int handle, const char* kind, const std::string& what) {
    std::string record(kind);
    record += '\0';
    record += what;
    record += '\0';
    app_push(handle, std::string(), record, 100);
}

HICON programIcon() {
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    return icon ? icon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));          // (the program's own icon, ICON= in build.sh, else the system's)
}

NOTIFYICONDATAW dataFor(Tray* t) {
    NOTIFYICONDATAW d = {};
    d.cbSize = sizeof d;
    d.hWnd = t->hwnd;
    d.uID = 1;
    d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    d.uCallbackMessage = kCallback;
    d.hIcon = programIcon();
    std::lock_guard<std::mutex> lock(t->m);
    wcsncpy(d.szTip, t->tooltip.c_str(), 127);
    d.szTip[127] = 0;
    return d;
}

void removeIcon(Tray* t) {
    if (!t->shown) return;
    NOTIFYICONDATAW d = {};
    d.cbSize = sizeof d;
    d.hWnd = t->hwnd;
    d.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &d);
    t->shown = false;
}

LRESULT CALLBACK trayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Tray* t = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (t == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == kCallback) {
        if (lp == NIN_BALLOONUSERCLICK) {
            std::string id;
            { std::lock_guard<std::mutex> lock(t->m); id = t->notifyId; t->notifyId.clear(); }
            if (!id.empty()) tell(t->handle, "notify", id);
            if (t->transient) removeIcon(t);
        } else if (lp == NIN_BALLOONTIMEOUT && t->transient) {
            removeIcon(t);
        }
        if (lp == WM_LBUTTONUP) tell(t->handle, "tray", "click");
        else if (lp == WM_LBUTTONDBLCLK) tell(t->handle, "tray", "double");
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            std::shared_ptr<NativeMenu> menu;
            { std::lock_guard<std::mutex> lock(t->m); menu = t->menu; }
            if (menu) {
                POINT at;
                GetCursorPos(&at);
                SetForegroundWindow(hwnd);                           // (without this the menu does not close when the user clicks elsewhere)
                TrackPopupMenu(menu->handle(), TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
            }
        }
        return 0;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == 0) {
        std::shared_ptr<NativeMenu> menu;
        { std::lock_guard<std::mutex> lock(t->m); menu = t->menu; }
        const std::string* id = menu ? menu->idOf(LOWORD(wp)) : nullptr;
        if (id) tell(t->handle, "menu", *id);
        return 0;
    }
    if (msg == kUpdate) {
        NOTIFYICONDATAW d = dataFor(t);
        Shell_NotifyIconW(NIM_MODIFY, &d);
        return 0;
    }
    if (msg == kArm) {
        SetTimer(hwnd, kHideTimer, kHideAfterMs, nullptr);
        return 0;
    }
    if (msg == WM_TIMER && wp == kHideTimer) {
        KillTimer(hwnd, kHideTimer);
        if (t->transient) removeIcon(t);
        return 0;
    }
    if (t->taskbarCreated != 0 && msg == t->taskbarCreated) {            // the shell was restarted: its notification area is empty again
        if (t->transient && !t->shown) return 0;
        NOTIFYICONDATAW d = dataFor(t);
        Shell_NotifyIconW(NIM_ADD, &d);
        return 0;
    }
    if (msg == WM_DESTROY) {
        NOTIFYICONDATAW d = {};
        d.cbSize = sizeof d;
        d.hWnd = hwnd;
        d.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &d);                              // (before the window goes: otherwise the icon stays until the mouse passes over it)
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void run(Tray* t) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = trayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);                                                // (already registered by an earlier tray: fine)
    t->hwnd = CreateWindowExW(0, kClass, L"app-cling tray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    bool ok = false;
    if (t->hwnd) {
        SetWindowLongPtrW(t->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(t));
        t->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (t->transient) {
            ok = true;                                                  // (the icon is added when there is something to say)
        } else {
            NOTIFYICONDATAW d = dataFor(t);
            ok = Shell_NotifyIconW(NIM_ADD, &d) != FALSE;
            t->shown = ok;
        }
        if (!ok) DestroyWindow(t->hwnd);
    }
    { std::lock_guard<std::mutex> lock(t->m); t->added = ok; t->started = true; }
    t->cv.notify_all();
    if (!ok) return;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
}  // namespace

namespace {
int openTray(std::string tooltip, std::shared_ptr<NativeMenu> menu, bool transient, int& handle) {
    handle = 0;
    auto t = std::make_shared<Tray>();
    t->tooltip = widen(tooltip);
    t->menu = menu;
    t->transient = transient;
    t->handle = app_queue_entry_new();
    if (t->handle == 0) return 4;
    t->thread = std::thread(run, t.get());
    {
        std::unique_lock<std::mutex> lock(t->m);
        t->cv.wait(lock, [&] { return t->started; });
    }
    if (!t->added) {
        t->thread.join();
        app_queue_entry_end(t->handle);
        return 4;
    }
    { std::lock_guard<std::mutex> lock(g_trayMutex); g_trays[t->handle] = t; }
    handle = t->handle;
    return 0;
}
}  // namespace

int app_tray_open(std::string tooltip, std::string menuSpec, int& handle) {
    handle = 0;
    std::string error;
    std::shared_ptr<NativeMenu> menu(NativeMenu::build(menuSpec, false, error).release());
    if (!menu) return 5;
    return openTray(tooltip, menu, false, handle);
}

int app_notifier_open(std::string tooltip, int& handle) {
    return openTray(tooltip, nullptr, true, handle);
}

int app_tray_notify(int handle, std::string title, std::string text, int kind, std::string id) {
    auto t = trayOf(handle);
    if (!t || !t->hwnd) return 4;
    const std::wstring wtitle = widen(title), wtext = widen(text);
    if (wtitle.empty() || wtitle.size() > 63 || wtext.size() > 255) return 5;
    { std::lock_guard<std::mutex> lock(t->m); t->notifyId = id; }
    NOTIFYICONDATAW d = dataFor(t.get());
    d.uFlags |= NIF_INFO;
    wcsncpy(d.szInfoTitle, wtitle.c_str(), 63);
    wcsncpy(d.szInfo, wtext.c_str(), 255);
    d.dwInfoFlags = (kind == 2 ? NIIF_ERROR : kind == 1 ? NIIF_WARNING : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    BOOL ok;
    if (t->transient && !t->shown) {
        ok = Shell_NotifyIconW(NIM_ADD, &d);
        t->shown = ok != FALSE;
    } else {
        ok = Shell_NotifyIconW(NIM_MODIFY, &d);
    }
    if (ok && t->transient) PostMessageW(t->hwnd, kArm, 0, 0);
    return ok ? 0 : 4;
}

int app_tray_update(int handle, std::string tooltip, std::string menuSpec) {
    auto t = trayOf(handle);
    if (!t) return 4;
    if (!menuSpec.empty()) {
        std::string error;
        std::shared_ptr<NativeMenu> menu(NativeMenu::build(menuSpec, false, error).release());
        if (!menu) return 5;
        std::lock_guard<std::mutex> lock(t->m);
        t->menu = menu;
    }
    if (!tooltip.empty()) {
        { std::lock_guard<std::mutex> lock(t->m); t->tooltip = widen(tooltip); }
        PostMessageW(t->hwnd, kUpdate, 0, 0);
    }
    return 0;
}

int app_tray_item(int handle, std::string id, int enabled, int checked) {
    auto t = trayOf(handle);
    if (!t) return 4;
    std::shared_ptr<NativeMenu> menu;
    { std::lock_guard<std::mutex> lock(t->m); menu = t->menu; }
    return menu && menu->update(id, enabled, checked, std::string()) ? 0 : 5;
}

// the tray is closed with app_close (the icon is removed first, then the thread ends)
void app_tray_forget(int handle) {
    std::shared_ptr<Tray> t;
    {
        std::lock_guard<std::mutex> lock(g_trayMutex);
        auto it = g_trays.find(handle);
        if (it == g_trays.end()) return;
        t = it->second;
        g_trays.erase(it);
    }
    if (t->hwnd) PostMessageW(t->hwnd, WM_CLOSE, 0, 0);                 // WM_CLOSE -> DestroyWindow -> WM_DESTROY (removes the icon) -> the loop ends
    if (t->thread.joinable()) t->thread.join();
}
