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
//   window_poke.exe wait TITLE               waits up to 20 s for the window to be visible; exit 1 if it never is
#include <windows.h>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
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

int main(int argc, char** argv) {
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
