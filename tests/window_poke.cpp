// A test tool: finds a top-level window by the start of its title and moves, maximizes, restores, closes or describes it, from outside the program, the way a
// user would. (A mouse drag itself is not simulated: after moving the window this posts WM_EXITSIZEMOVE, which is what the end of a drag sends.)
//   window_poke.exe find TITLE               prints "x y width height maximized" (screen coordinates; the restored rectangle when maximized); exit 1 if no window
//   window_poke.exe move TITLE x y w h       SetWindowPos, then WM_EXITSIZEMOVE
//   window_poke.exe maximize|restore|close TITLE
//   window_poke.exe inside TITLE             exit 0 if the window lies wholly inside the work area of one monitor
//   window_poke.exe icon TITLE big|small    prints the colour in the middle of the window's icon, "RRGGBB alpha"; exit 1 if the window has none
//   window_poke.exe exeicon FILE             the same for the first icon of a program file
//   window_poke.exe menu TITLE              prints the menu bar: "File: Open|Save|-|Quit ; View: Grid*" (- a separator, * a ticked item, ~ a greyed one); exit 1 if it has none
//   window_poke.exe click TITLE LABEL        chooses the menu item with that label (as if clicked: WM_COMMAND), exit 1 if there is none
//   window_poke.exe key TITLE ctrl+s         presses the keys with the window in front and the page holding the keyboard (SendInput); keys: ctrl, alt, shift, a letter, a digit, f1..f12
//   window_poke.exe client TITLE            prints "width height" of the window's client area in pixels (the part below the menu bar)
//   window_poke.exe state TITLE             prints visible, hidden or none (a window of that title, shown or not)
//   window_poke.exe trayexists              exit 0 if the shell holds the tray icon of the program's hidden tray window (it can say where the icon is), else 1
//   window_poke.exe traywait SECONDS        polls (every 15 ms) until the shell holds the tray icon; exit 0 if it did within the time, else 1
//   window_poke.exe trayabsent SECONDS      polls until the shell does not hold the tray icon; exit 0 if it let go within the time, else 1
//   window_poke.exe dismiss                 closes the tray menu if one is open, without the keyboard (WM_CANCELMODE to the tray window: the keyboard goes to whichever window is in front)
//   window_poke.exe popupexists             exit 0 if a popup menu is open on the screen (class #32768); escape: presses Esc
//   window_poke.exe traycall left|double|right   what the shell sends the tray window when the icon is clicked (the real click on the icon is not simulated)
//   window_poke.exe traycall balloon       (also) what the shell sends when the user clicks a notification (balloon) of the icon
//   window_poke.exe traycommand N           chooses item number N (counting from 0, separators excluded) of the tray menu, as the menu would
//   window_poke.exe rightclick TITLE        brings the window to the front and right-clicks in the middle of its page
//   window_poke.exe menukeys                 presses Down and Enter (chooses the first item of the menu that is open)
//   window_poke.exe box TITLE               prints the text of a message box whose title starts with TITLE (waits up to 20 s); exit 1 if there is none
//   window_poke.exe boxanswer TITLE yes|no|ok   answers that message box
//   window_poke.exe layout TITLE            prints what Windows says about the window: "x y width height topmost caption thickframe enabled coversMonitor" (the flags are 0 / 1)
//   window_poke.exe drop TITLE file...      drags the files over the middle of the window's page and lets go there, the way the file manager does (OLE drag and drop with the cursor
//                                            moved by SendInput); prints what the window answered: copy, move or link (it took them), none (it does not want them) or cancel; exit 0 if it took them
//   window_poke.exe droptext TITLE           the same with a piece of text instead of files (a window that wants files only must answer none)
//   window_poke.exe darkbar TITLE            prints 1 if Windows draws the window's title bar dark, 0 if light (DWMWA_USE_IMMERSIVE_DARK_MODE as DWM holds it)
//   window_poke.exe themechange TITLE        sends the window the notice Windows sends when the colour scheme changes (WM_SETTINGCHANGE "ImmersiveColorSet"); changes no setting
//   window_poke.exe appsdark                 prints 1 if the user chose dark applications (the registry value Windows keeps), else 0
//   window_poke.exe wait TITLE               waits up to 20 s for the window to be visible; exit 1 if it never is
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Search {
    std::wstring prefix;
    HWND found = nullptr;
};

BOOL CALLBACK look(HWND h, LPARAM p) {
    auto* s = reinterpret_cast<Search*>(p);
    wchar_t title[256];
    if (!IsWindowVisible(h) || GetWindowTextW(h, title, 256) <= 0) return TRUE;
    if (std::wstring(title).compare(0, s->prefix.size(), s->prefix) == 0 && GetParent(h) == nullptr) {
        s->found = h;
        return FALSE;
    }
    return TRUE;
}

HWND findWindow(const std::wstring& prefix) {
    Search s;
    s.prefix = prefix;
    EnumWindows(look, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

std::wstring wide(const char* text) {
    const std::string s(text);                       // (plain ASCII titles only)
    return std::wstring(s.begin(), s.end());
}

// "RRGGBB alpha" of the middle of an icon
bool middleOf(HICON icon, std::string& out) {
    ICONINFO info = {};
    if (!icon || !GetIconInfo(icon, &info)) return false;
    BITMAP bm = {};
    bool ok = info.hbmColor && GetObject(info.hbmColor, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0;
    if (ok) {
        std::vector<unsigned char> bits(static_cast<size_t>(bm.bmWidth) * bm.bmHeight * 4);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;                          // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, info.hbmColor, 0, bm.bmHeight, bits.data(), &bi, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, dc);
        if (ok) {
            const unsigned char* p = &bits[(static_cast<size_t>(bm.bmHeight / 2) * bm.bmWidth + bm.bmWidth / 2) * 4];
            char text[40];
            std::snprintf(text, sizeof text, "%02x%02x%02x %d", p[2], p[1], p[0], p[3]);
            out = text;
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return ok;
}

// the menu item text without its mnemonic mark and shortcut ("&Open...\tCtrl+O" -> "Open...")
std::string plainLabel(HMENU m, UINT pos) {
    wchar_t text[256];
    if (GetMenuStringW(m, pos, text, 256, MF_BYPOSITION) <= 0) return "";
    std::string out;
    for (const wchar_t* c = text; *c && *c != L'\t'; ++c) if (*c != L'&' || (c[1] == L'&')) out += static_cast<char>(*c);
    return out;
}

// "Open|Save|-|Quit" for one submenu
std::string describeItems(HMENU m) {
    std::string out;
    for (int i = 0; i < GetMenuItemCount(m); i++) {
        if (i) out += "|";
        const UINT state = GetMenuState(m, i, MF_BYPOSITION);
        if (state & MF_SEPARATOR) { out += "-"; continue; }
        out += plainLabel(m, i);
        if (state & MF_CHECKED) out += "*";
        if (state & MF_GRAYED) out += "~";
    }
    return out;
}

// the command number of the item with this label, searching every submenu; 0 if none
UINT commandOf(HMENU m, const std::string& label) {
    for (int i = 0; i < GetMenuItemCount(m); i++) {
        if (HMENU sub = GetSubMenu(m, i)) {
            if (UINT c = commandOf(sub, label)) return c;
        } else if (plainLabel(m, i) == label) {
            return GetMenuItemID(m, i);
        }
    }
    return 0;
}

// the restored rectangle in screen coordinates, and whether the window is maximized
bool describe(HWND h, RECT& r, bool& maximized) {
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof wp;
    if (!GetWindowPlacement(h, &wp)) return false;
    maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    r = wp.rcNormalPosition;
    if (maximized) {
        MONITORINFO info = {};
        info.cbSize = sizeof info;
        if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) OffsetRect(&r, info.rcWork.left, info.rcWork.top);
        return true;
    }
    return GetWindowRect(h, &r) != 0;
}
}  // namespace

// the program's hidden tray window (class pronao-tray), shown or not
HWND findTray() {
    return FindWindowW(L"pronao-tray", nullptr);
}

struct Any {
    std::wstring prefix;
    HWND found = nullptr;
};

BOOL CALLBACK lookAny(HWND h, LPARAM p) {
    auto* s = reinterpret_cast<Any*>(p);
    wchar_t title[256];
    if (GetParent(h) != nullptr || GetWindowTextW(h, title, 256) <= 0) return TRUE;
    if (std::wstring(title).compare(0, s->prefix.size(), s->prefix) == 0) { s->found = h; return FALSE; }
    return TRUE;
}

// a message box (class #32770) whose title starts with `prefix`
HWND findBox(const std::wstring& prefix) {
    struct S { std::wstring prefix; HWND found = nullptr; } s;
    s.prefix = prefix;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto* s = reinterpret_cast<S*>(p);
        wchar_t cls[32], title[256];
        if (GetClassNameW(h, cls, 32) && std::wstring(cls) == L"#32770" && GetWindowTextW(h, title, 256) > 0 && std::wstring(title).compare(0, s->prefix.size(), s->prefix) == 0) {
            s->found = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.found;
}

// ---- dragging something over a window ----------------------------------------------------------------------------------------------------------------------------
class DragData : public IDataObject {
public:
    explicit DragData(const std::vector<std::wstring>& files, const std::wstring& text) : files_(files), text_(text) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDataObject)) ? static_cast<IDataObject*>(this) : nullptr;
        if (*out) AddRef();
        return *out ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
        if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (f->cfFormat == CF_HDROP && !files_.empty()) {
            size_t chars = 1;
            for (const auto& p : files_) chars += p.size() + 1;
            HGLOBAL g = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
            auto* d = static_cast<DROPFILES*>(GlobalLock(g));
            d->pFiles = sizeof(DROPFILES);
            d->fWide = TRUE;
            wchar_t* at = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(d) + sizeof(DROPFILES));
            for (const auto& p : files_) { memcpy(at, p.c_str(), (p.size() + 1) * sizeof(wchar_t)); at += p.size() + 1; }
            GlobalUnlock(g);
            m->tymed = TYMED_HGLOBAL; m->hGlobal = g; m->pUnkForRelease = nullptr;
            return S_OK;
        }
        if (f->cfFormat == CF_UNICODETEXT && files_.empty()) {
            HGLOBAL g = GlobalAlloc(GHND, (text_.size() + 1) * sizeof(wchar_t));
            memcpy(GlobalLock(g), text_.c_str(), (text_.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            m->tymed = TYMED_HGLOBAL; m->hGlobal = g; m->pUnkForRelease = nullptr;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
        if (!(f->tymed & TYMED_HGLOBAL) || f->dwAspect != DVASPECT_CONTENT) return DV_E_FORMATETC;
        return (f->cfFormat == CF_HDROP && !files_.empty()) || (f->cfFormat == CF_UNICODETEXT && files_.empty()) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override { out->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC f = {static_cast<CLIPFORMAT>(files_.empty() ? CF_UNICODETEXT : CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return SHCreateStdEnumFmtEtc(1, &f, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG refs_ = 1;
    std::vector<std::wstring> files_;
    std::wstring text_;
};

// lets the drag run for a while with the cursor moving a little over the target, then lets go
class DragSource : public IDropSource {
public:
    DragSource(int x, int y) : x_(x), y_(y), began_(GetTickCount()) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        *out = (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_IDropSource)) ? static_cast<IDropSource*>(this) : nullptr;
        if (*out) AddRef();
        return *out ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD) override {
        if (escape) return DRAGDROP_S_CANCEL;
        return GetTickCount() - began_ > 1200 ? DRAGDROP_S_DROP : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG refs_ = 1;
    int x_, y_;
    DWORD began_;
};

// keeps the pointer moving a little over the target while the drag runs: OLE looks at what is under the pointer, and asks the source whether to go on, only when something happens
void wiggle(int x, int y, volatile bool* stop) {
    bool step = false;
    for (int i = 1; i <= 8 && !*stop; i++) {                                   // from where the drag began to the target, in steps, so that every window on the way sees the pointer
        POINT now;
        GetCursorPos(&now);
        SetCursorPos(now.x + (x - now.x) * i / 8, now.y + (y - now.y) * i / 8);
        Sleep(40);
    }
    while (!*stop) {
        step = !step;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN), vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        in.mi.dx = static_cast<LONG>((x + (step ? 6 : 0) - vx) * 65535.0 / (vw - 1));
        in.mi.dy = static_cast<LONG>((y + (step ? 4 : 0) - vy) * 65535.0 / (vh - 1));
        SendInput(1, &in, sizeof in);
        Sleep(60);
    }
}

int dragOver(HWND window, const std::vector<std::wstring>& files, const std::wstring& text) {
    RECT r;
    GetWindowRect(window, &r);
    // the drag starts in a small window of this tool, in front of everything: a drag needs the program that holds it to be the one that has the keyboard and mouse, as Explorer is
    // when the user drags from it
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"poke-drag-source";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    keybd_event(VK_MENU, 0, 0, 0);                       // (pressing Alt lets a program take the foreground)
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);                         // the target first: in front of whatever else is on the screen (a drop must land on it and on nothing else)
    Sleep(500);
    HWND source_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"poke-drag-source", L"poke", WS_POPUP | WS_VISIBLE, 5, 5, 80, 50, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (int i = 0; i < 10; i++) { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(20); }
    keybd_event(VK_MENU, 0, 0, 0);                       // (pressing Alt lets a program take the foreground)
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(source_window);
    Sleep(300);
    SetCursorPos(30, 30);
    Sleep(100);
    POINT middle = {(r.left + r.right) / 2, (r.top + r.bottom) / 2 + 20};
    SetCursorPos(middle.x, middle.y);
    Sleep(100);
    {   // if another window is over the target at that point the drag would end in a program that has nothing to do with the test: do not start it
        HWND under = WindowFromPoint(middle);
        if (!under || GetAncestor(under, GA_ROOT) != window) {
            DestroyWindow(source_window);
            std::printf("covered\n");
            return 3;
        }
    }
    SetCursorPos(30, 30);
    Sleep(100);
    INPUT button = {};
    button.type = INPUT_MOUSE;
    button.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &button, sizeof button);                  // pressed over the tool's own window, where the drag starts
    Sleep(100);
    OleInitialize(nullptr);
    DragData* data = new DragData(files, text);
    DragSource* source = new DragSource(middle.x, middle.y);
    DWORD effect = DROPEFFECT_NONE;
    volatile bool stop = false;
    std::thread mover(wiggle, static_cast<int>(middle.x), static_cast<int>(middle.y), &stop);
    const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    stop = true;
    mover.join();
    button.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &button, sizeof button);
    data->Release();
    source->Release();
    DestroyWindow(source_window);
    const char* said = hr == DRAGDROP_S_CANCEL ? "cancel" : effect == DROPEFFECT_COPY ? "copy" : effect == DROPEFFECT_MOVE ? "move" : effect == DROPEFFECT_LINK ? "link" : "none";
    std::printf("%s\n", said);
    return std::string(said) == "none" || std::string(said) == "cancel" ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "drop") {
        HWND w = findWindow(wide(argv[2]));
        if (!w) return 2;
        std::vector<std::wstring> files;
        for (int i = 3; i < argc; i++) { const std::string s(argv[i]); files.emplace_back(s.begin(), s.end()); }
        return dragOver(w, files, L"");
    }
    if (argc >= 2 && std::string(argv[1]) == "mouseup") {          // lets go of the left button (a drag that was cut short may leave it held)
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof in);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "droptext") {
        HWND w = findWindow(wide(argv[2]));
        return w ? dragOver(w, {}, L"just some text") : 2;
    }
    if (argc >= 3 && std::string(argv[1]) == "darkbar") {
        HWND w = findWindow(wide(argv[2]));
        BOOL dark = FALSE;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        auto get = dwm ? reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD)>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmGetWindowAttribute"))) : nullptr;
        if (!w || !get || FAILED(get(w, 20, &dark, sizeof dark))) return 2;
        std::printf("%d\n", dark ? 1 : 0);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "themechange") {
        HWND w = findWindow(wide(argv[2]));
        if (!w) return 2;
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(w, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 3000, &result) ? 0 : 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "appsdark") {
        DWORD light = 1, size = sizeof light;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        std::printf("%d\n", light == 0 ? 1 : 0);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "box") {
        const std::string t(argv[2]);
        HWND box = nullptr;
        for (int i = 0; i < 200 && !box; i++) { box = findBox(std::wstring(t.begin(), t.end())); if (!box) Sleep(100); }
        if (!box) return 1;
        std::wstring text;
        EnumChildWindows(box, [](HWND c, LPARAM p) -> BOOL {
            wchar_t cls[32], buf[2048];
            if (GetClassNameW(c, cls, 32) && std::wstring(cls) == L"Static" && GetWindowTextW(c, buf, 2048) > 0) *reinterpret_cast<std::wstring*>(p) += std::wstring(buf) + L" ";
            return TRUE;
        }, reinterpret_cast<LPARAM>(&text));
        std::string out;
        for (wchar_t c : text) out += (c == L'\n' || c == L'\r') ? ' ' : static_cast<char>(c < 128 ? c : '?');
        std::printf("%s\n", out.c_str());
        return 0;
    }
    if (argc >= 4 && std::string(argv[1]) == "boxanswer") {
        const std::string t(argv[2]), a(argv[3]);
        HWND box = findBox(std::wstring(t.begin(), t.end()));
        if (!box) return 1;
        return PostMessageW(box, WM_COMMAND, a == "yes" ? IDYES : a == "no" ? IDNO : IDOK, 0) ? 0 : 1;
    }
    if (argc >= 3 && (std::string(argv[1]) == "traywait" || std::string(argv[1]) == "trayabsent")) {
        const bool wantPresent = std::string(argv[1]) == "traywait";
        for (long ms = 0; ms <= std::atol(argv[2]) * 1000; ms += 15) {
            HWND t = findTray();
            bool present = false;
            if (t) {
                NOTIFYICONIDENTIFIER id = {};
                id.cbSize = sizeof id;
                id.hWnd = t;
                id.uID = 1;
                RECT where;
                present = SUCCEEDED(Shell_NotifyIconGetRect(&id, &where));
            }
            if (present == wantPresent) return 0;
            Sleep(15);
        }
        return 1;
    }
    if (argc >= 2 && std::string(argv[1]) == "trayexists") {
        HWND t = findTray();
        if (!t) return 1;
        NOTIFYICONIDENTIFIER id = {};
        id.cbSize = sizeof id;
        id.hWnd = t;
        id.uID = 1;
        RECT where;
        return SUCCEEDED(Shell_NotifyIconGetRect(&id, &where)) ? 0 : 1;           // the shell knows where the icon is only if it holds it
    }
    if (argc >= 2 && std::string(argv[1]) == "menukeys") {
        for (WORD vk : {VK_DOWN, VK_RETURN}) {
            keybd_event(static_cast<BYTE>(vk), 0, 0, 0);
            keybd_event(static_cast<BYTE>(vk), 0, KEYEVENTF_KEYUP, 0);
            Sleep(150);
        }
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "rightclick") {
        Any a;
        const std::string s(argv[2]);
        a.prefix = std::wstring(s.begin(), s.end());
        EnumWindows(lookAny, reinterpret_cast<LPARAM>(&a));
        if (!a.found) return 1;
        keybd_event(VK_SHIFT, 0, 0, 0);
        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(a.found);
        Sleep(300);
        if (GetForegroundWindow() != a.found) { std::fprintf(stderr, "the window could not be brought to the front: no click is sent\n"); return 3; }
        RECT client;
        GetClientRect(a.found, &client);
        POINT middle{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(a.found, &middle);
        SetCursorPos(middle.x, middle.y);
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "dismiss") {
        HWND t = findTray();
        if (!t) return 1;
        SendMessageTimeoutW(t, WM_CANCELMODE, 0, 0, SMTO_ABORTIFHUNG, 2000, nullptr);
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "popupexists") return FindWindowW(L"#32768", nullptr) ? 0 : 1;
    if (argc >= 2 && std::string(argv[1]) == "escape") {
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "traycall") {
        HWND t = findTray();
        if (!t) return 1;
        const std::string kind = argv[2];
        const LPARAM what = kind == "left" ? WM_LBUTTONUP : kind == "double" ? WM_LBUTTONDBLCLK : kind == "balloon" ? WM_USER + 5 : WM_RBUTTONUP;
        return PostMessageW(t, WM_APP + 1, 1, what) ? 0 : 1;
    }
    if (argc >= 3 && std::string(argv[1]) == "traycommand") {
        HWND t = findTray();
        if (!t) return 1;
        return PostMessageW(t, WM_COMMAND, MAKEWPARAM(1000 + std::atoi(argv[2]), 0), 0) ? 0 : 1;
    }
    if (argc >= 3 && std::string(argv[1]) == "state") {
        Any a;
        const std::string s(argv[2]);
        a.prefix = std::wstring(s.begin(), s.end());
        EnumWindows(lookAny, reinterpret_cast<LPARAM>(&a));
        std::printf("%s\n", !a.found ? "none" : IsWindowVisible(a.found) ? "visible" : "hidden");
        return a.found ? 0 : 1;
    }
    if (argc < 3) return 2;
    const std::string action = argv[1];
    const std::wstring title = wide(argv[2]);
    if (action == "wait") {
        for (int i = 0; i < 200; i++) {
            if (findWindow(title)) return 0;
            Sleep(100);
        }
        return 1;
    }
    if (action == "exeicon") {
        HICON large = nullptr;
        std::string text;
        const std::string file(argv[2]);
        if (ExtractIconExW(std::wstring(file.begin(), file.end()).c_str(), 0, &large, nullptr, 1) < 1 || !middleOf(large, text)) return 1;
        std::printf("%s\n", text.c_str());
        return 0;
    }
    HWND h = findWindow(title);
    if (!h) return 1;
    if (action == "client") {
        RECT c;
        if (!GetClientRect(h, &c)) return 1;
        std::printf("%ld %ld\n", c.right - c.left, c.bottom - c.top);
        return 0;
    }
    if (action == "layout") {
        RECT r;
        if (!GetWindowRect(h, &r)) return 1;
        const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        MONITORINFO info = {};
        info.cbSize = sizeof info;
        const bool covers = GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &info) && r.left == info.rcMonitor.left && r.top == info.rcMonitor.top &&
                            r.right == info.rcMonitor.right && r.bottom == info.rcMonitor.bottom;
        std::printf("%ld %ld %ld %ld %d %d %d %d %d\n", r.left, r.top, r.right - r.left, r.bottom - r.top, (ex & WS_EX_TOPMOST) ? 1 : 0, (style & WS_CAPTION) == WS_CAPTION ? 1 : 0,
                    (style & WS_THICKFRAME) ? 1 : 0, IsWindowEnabled(h) ? 1 : 0, covers ? 1 : 0);
        return 0;
    }
    if (action == "menu") {
        HMENU bar = GetMenu(h);
        if (!bar) return 1;
        std::string out;
        for (int i = 0; i < GetMenuItemCount(bar); i++) {
            if (i) out += " ; ";
            out += plainLabel(bar, i) + ": " + (GetSubMenu(bar, i) ? describeItems(GetSubMenu(bar, i)) : std::string());
        }
        std::printf("%s\n", out.c_str());
        return 0;
    }
    if (action == "click" && argc >= 4) {
        HMENU bar = GetMenu(h);
        const UINT command = bar ? commandOf(bar, argv[3]) : 0;
        if (!command) return 1;
        PostMessageW(h, WM_COMMAND, MAKEWPARAM(command, 0), 0);
        return 0;
    }
    if (action == "key" && argc >= 4) {
        std::vector<WORD> keys;
        std::string spec = argv[3];
        for (size_t start = 0; start <= spec.size();) {
            const size_t plus = spec.find('+', start);
            const std::string part = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            WORD vk = 0;
            if (part == "ctrl") vk = VK_CONTROL; else if (part == "alt") vk = VK_MENU; else if (part == "shift") vk = VK_SHIFT;
            else if (part.size() == 1) vk = static_cast<WORD>(std::toupper(part[0]));
            else if (part.size() >= 2 && part[0] == 'f') vk = static_cast<WORD>(VK_F1 + std::atoi(part.c_str() + 1) - 1);
            if (!vk) return 2;
            keys.push_back(vk);
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        // Windows lets a program bring a window to the front only if it was the last to receive input: a tap on Shift, then SetForegroundWindow, gets round that
        keybd_event(VK_SHIFT, 0, 0, 0);                  // (not Alt: a tap on Alt switches a menu bar into its keyboard mode, which would swallow the keys)
        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
        SetForegroundWindow(h);
        Sleep(300);
        if (GetForegroundWindow() != h) { std::fprintf(stderr, "the window could not be brought to the front: no keys are sent\n"); return 3; }
        // the page (the browser engine's child window) must hold the keyboard, not the frame: give it the focus through the window's own thread (no mouse is used)
        HWND page = nullptr;
        EnumChildWindows(h, [](HWND c, LPARAM p) -> BOOL {
            wchar_t name[64];
            if (GetClassNameW(c, name, 64) && std::wstring(name) == L"Chrome_RenderWidgetHostHWND") { *reinterpret_cast<HWND*>(p) = c; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&page));
        if (page && std::getenv("POKE_FOCUS")) {
            const DWORD theirs = GetWindowThreadProcessId(h, nullptr), mine = GetCurrentThreadId();
            AttachThreadInput(mine, theirs, TRUE);
            SetFocus(page);
            AttachThreadInput(mine, theirs, FALSE);
        }
        Sleep(200);
        std::vector<INPUT> in;
        for (WORD vk : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = vk; in.push_back(i); }
        for (size_t k = keys.size(); k-- > 0;) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = keys[k]; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
        return SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) == in.size() ? 0 : 1;
    }
    if (action == "icon" && argc >= 4) {
        std::string text;
        const bool big = std::string(argv[3]) == "big";
        HICON icon = reinterpret_cast<HICON>(SendMessageW(h, WM_GETICON, big ? ICON_BIG : ICON_SMALL, 0));
        if (!middleOf(icon, text)) return 1;
        std::printf("%s\n", text.c_str());
        return 0;
    }
    if (action == "find") {
        RECT r;
        bool maximized = false;
        if (!describe(h, r, maximized)) return 1;
        std::printf("%ld %ld %ld %ld %d\n", r.left, r.top, r.right - r.left, r.bottom - r.top, maximized ? 1 : 0);
        return 0;
    }
    if (action == "move" && argc >= 7) {
        SetWindowPos(h, nullptr, std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]), SWP_NOZORDER | SWP_NOACTIVATE);
        PostMessageW(h, WM_EXITSIZEMOVE, 0, 0);
        return 0;
    }
    if (action == "maximize") { ShowWindow(h, SW_MAXIMIZE); return 0; }
    if (action == "restore") { ShowWindow(h, SW_RESTORE); return 0; }
    if (action == "close") { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
    if (action == "inside") {
        RECT r;
        bool maximized = false;
        if (!describe(h, r, maximized)) return 1;
        bool inside = false;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
            auto* both = reinterpret_cast<std::pair<RECT*, bool*>*>(p);
            MONITORINFO info = {};
            info.cbSize = sizeof info;
            if (GetMonitorInfoW(m, &info) && both->first->left >= info.rcWork.left && both->first->top >= info.rcWork.top &&
                both->first->right <= info.rcWork.right && both->first->bottom <= info.rcWork.bottom) *both->second = true;
            return TRUE;
        }, reinterpret_cast<LPARAM>(new std::pair<RECT*, bool*>(&r, &inside)));
        return inside ? 0 : 1;
    }
    return 2;
}
