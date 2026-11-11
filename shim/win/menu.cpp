// Windows: native menus from the flat spec of src/menu.clg, and shortcuts that work while the page has the keyboard (see menu_native.h).
// What a menu may contain, and every limit on it, is decided in src/menu.clg; this builds what it is given and refuses what does not parse.
#include <WebView2.h>

#include <atomic>

#include "menu_native.h"
#include "util.h"

namespace {
const UINT kFirstCommand = 1000;

std::vector<std::string> fieldsOf(const std::string& spec) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : spec) {
        if (c == '\0') { f.push_back(cur); cur.clear(); } else cur += c;
    }
    return f;
}

bool toNumber(const std::string& s, UINT& out) {
    if (s.empty() || s.size() > 9) return false;
    UINT v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<UINT>(c - '0');
    }
    out = v;
    return true;
}
}

NativeMenu::~NativeMenu() {
    if (root_) DestroyMenu(root_);
}

std::unique_ptr<NativeMenu> NativeMenu::build(const std::string& spec, bool bar, std::string& error) {
    const std::vector<std::string> f = fieldsOf(spec);
    if (f.empty() || f.size() % 7 != 0 || f.size() / 7 > 400) { error = "the menu is empty or malformed"; return nullptr; }
    std::unique_ptr<NativeMenu> menu(new NativeMenu());
    menu->root_ = bar ? CreateMenu() : CreatePopupMenu();
    if (!menu->root_) { error = "no menu could be created"; return nullptr; }
    std::vector<HMENU> stack{menu->root_};
    for (size_t i = 0; i < f.size(); i += 7) {
        const std::string &kind = f[i], &id = f[i + 1], &label = f[i + 2], &shortcut = f[i + 3];
        UINT vk = 0, mods = 0, flags = 0;
        if (!toNumber(f[i + 4], vk) || !toNumber(f[i + 5], mods) || !toNumber(f[i + 6], flags)) { error = "a number in the menu is not a number"; return nullptr; }
        if (kind == "e") {
            if (stack.size() < 2) { error = "end without begin"; return nullptr; }
            stack.pop_back();
        } else if (kind == "s") {
            AppendMenuW(stack.back(), MF_SEPARATOR, 0, nullptr);
        } else if (kind == "b" || kind == "i" || kind == "c") {
            std::wstring text = widen(label);
            if (text.empty()) { error = "a label is not valid text"; return nullptr; }
            if (kind == "b") {
                HMENU sub = CreatePopupMenu();
                if (!sub || !AppendMenuW(stack.back(), MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), text.c_str())) { error = "a submenu could not be added"; return nullptr; }
                stack.push_back(sub);
            } else {
                if (!shortcut.empty()) text += L"\t" + widen(shortcut);
                UINT attributes = MF_STRING | ((flags & 1) ? MF_CHECKED : 0) | ((flags & 2) ? MF_GRAYED : 0);
                if (menu->items_.size() >= 400) { error = "too many items"; return nullptr; }
                if (!AppendMenuW(stack.back(), attributes, kFirstCommand + static_cast<UINT>(menu->items_.size()), text.c_str())) { error = "an item could not be added"; return nullptr; }
                Item item;
                item.id = id; item.shortcut = shortcut; item.vk = vk; item.mods = mods; item.enabled = (flags & 2) == 0;
                menu->items_.push_back(item);
            }
        } else {
            error = "unknown kind of entry";
            return nullptr;
        }
    }
    if (stack.size() != 1) { error = "a submenu was not closed"; return nullptr; }
    return menu;
}

const std::string* NativeMenu::idOf(UINT command) const {
    if (command < kFirstCommand || command - kFirstCommand >= items_.size()) return nullptr;
    return &items_[command - kFirstCommand].id;
}

const std::string* NativeMenu::idOfShortcut(UINT vk, UINT modifiers) const {
    for (const Item& item : items_)
        if (item.vk != 0 && item.vk == vk && item.mods == modifiers && item.enabled) return &item.id;
    return nullptr;
}

bool NativeMenu::update(const std::string& id, int enabled, int checked, const std::string& label) {
    for (size_t i = 0; i < items_.size(); i++) {
        Item& item = items_[i];
        if (item.id != id) continue;
        const UINT command = kFirstCommand + static_cast<UINT>(i);
        if (enabled >= 0) { item.enabled = enabled != 0; EnableMenuItem(root_, command, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED)); }
        if (checked >= 0) CheckMenuItem(root_, command, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
        if (!label.empty()) {
            std::wstring text = widen(label);
            if (text.empty()) return false;
            if (!item.shortcut.empty()) text += L"\t" + widen(item.shortcut);
            MENUITEMINFOW info = {};
            info.cbSize = sizeof info;
            info.fMask = MIIM_STRING;
            info.dwTypeData = const_cast<wchar_t*>(text.c_str());
            SetMenuItemInfoW(root_, command, FALSE, &info);
        }
        return true;
    }
    return false;
}

UINT app_current_modifiers() {
    return ((GetKeyState(VK_CONTROL) & 0x8000) ? 1u : 0u) | ((GetKeyState(VK_MENU) & 0x8000) ? 2u : 0u) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 4u : 0u);
}

namespace {
struct KeyHandler : ICoreWebView2AcceleratorKeyPressedEventHandler {
    std::function<bool(UINT, UINT, bool)> onKey;
    std::atomic<ULONG> refs{1};
    explicit KeyHandler(std::function<bool(UINT, UINT, bool)> f) : onKey(std::move(f)) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        *p = (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, IID_ICoreWebView2AcceleratorKeyPressedEventHandler)) ? this : nullptr;
        if (*p) AddRef();
        return *p ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* a) override {
        COREWEBVIEW2_KEY_EVENT_KIND kind;
        UINT vk = 0;
        if (FAILED(a->get_KeyEventKind(&kind)) || FAILED(a->get_VirtualKey(&vk))) return S_OK;
        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return S_OK;
        COREWEBVIEW2_PHYSICAL_KEY_STATUS status = {};
        a->get_PhysicalKeyStatus(&status);
        const UINT mods = app_current_modifiers();
        if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT) return S_OK;
        if (onKey(vk, mods, status.WasKeyDown != 0)) a->put_Handled(TRUE);
        return S_OK;
    }
};
}

void app_accelerators_install(void* controller, std::function<bool(UINT, UINT, bool)> onKey) {
    EventRegistrationToken token;
    static_cast<ICoreWebView2Controller*>(controller)->add_AcceleratorKeyPressed(new KeyHandler(std::move(onKey)), &token);
}

#include <map>
#include <mutex>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {
std::mutex g_barsMutex;
std::map<int, std::shared_ptr<NativeMenu>> g_bars;

std::shared_ptr<NativeMenu> barOf(int handle) {
    std::lock_guard<std::mutex> lock(g_barsMutex);
    auto it = g_bars.find(handle);
    return it == g_bars.end() ? nullptr : it->second;
}

void tell(int handle, const std::string& id) {
    std::string record("menu\0", 5);
    record += id;
    record += '\0';
    app_push(handle, std::string(), record, 100);
}
}

int app_menu_set(int handle, std::string spec) {
    std::shared_ptr<NativeMenu> fresh;
    if (!spec.empty()) {
        std::string error;
        fresh = NativeMenu::build(spec, true, error);
        if (!fresh) return 5;
    }
    std::shared_ptr<NativeMenu> old;
    const int ran = app_ui_call(handle, [&](void* window) {
        HWND hwnd = static_cast<HWND>(window);
        {
            std::lock_guard<std::mutex> lock(g_barsMutex);
            old = g_bars.count(handle) ? g_bars[handle] : nullptr;
            if (fresh) g_bars[handle] = fresh; else g_bars.erase(handle);
        }
        SetMenu(hwnd, fresh ? fresh->handle() : nullptr);
        DrawMenuBar(hwnd);
    });
    return ran ? 0 : 4;
}

int app_menu_popup(int handle, std::string spec, std::string& chosen) {
    chosen.clear();
    std::string error;
    std::shared_ptr<NativeMenu> menu(NativeMenu::build(spec, false, error).release());
    if (!menu) return 5;
    const int ran = app_ui_call(handle, [&](void* window) {
        HWND hwnd = static_cast<HWND>(window);
        POINT at;
        GetCursorPos(&at);
        SetForegroundWindow(hwnd);                                  // (without this the menu does not close when the user clicks elsewhere)
        const UINT command = static_cast<UINT>(TrackPopupMenu(menu->handle(), TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr));
        PostMessageW(hwnd, WM_NULL, 0, 0);
        if (const std::string* id = command ? menu->idOf(command) : nullptr) chosen = *id;
    });
    if (!ran) return 4;
    if (!chosen.empty()) tell(handle, chosen);
    return 0;
}

int app_menu_item(int handle, std::string id, int enabled, int checked) {
    std::shared_ptr<NativeMenu> bar = barOf(handle);
    if (!bar) return 5;
    bool found = false;
    app_ui_call(handle, [&](void* window) {
        found = bar->update(id, enabled, checked, std::string());
        DrawMenuBar(static_cast<HWND>(window));
    });
    return found ? 0 : 5;
}

bool app_menu_command(int handle, unsigned command) {
    std::shared_ptr<NativeMenu> bar = barOf(handle);
    const std::string* id = bar ? bar->idOf(command) : nullptr;
    if (!id) return false;
    tell(handle, *id);
    return true;
}

bool app_menu_shortcut(int handle, unsigned vk, unsigned mods, bool repeat) {
    std::shared_ptr<NativeMenu> bar = barOf(handle);
    const std::string* id = bar ? bar->idOfShortcut(vk, mods) : nullptr;
    if (!id) return false;
    if (!repeat) tell(handle, *id);
    return true;
}

void app_menu_forget(int handle) {
    std::lock_guard<std::mutex> lock(g_barsMutex);
    g_bars.erase(handle);
}
