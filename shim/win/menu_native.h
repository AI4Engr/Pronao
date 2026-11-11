// Windows: a native menu built from the flat spec of src/menu.clg (see app_menu_set in app_native.h), and the keyboard shortcuts of its items. Used from the UI thread of the
// window (or of the tray) only. The command numbers are made here; what leaves this file is an id the application gave.
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class NativeMenu {
public:
    ~NativeMenu();
    // `bar`: a menu bar (its top level is only submenus), else a popup menu. nullptr if the spec is malformed (`error` says where).
    static std::unique_ptr<NativeMenu> build(const std::string& spec, bool bar, std::string& error);

    HMENU handle() const { return root_; }
    // the id behind a command number from WM_COMMAND, or nullptr if this menu did not make it
    const std::string* idOf(UINT command) const;
    // the id of the enabled item whose shortcut is vk + modifiers (bits as app_current_modifiers), or nullptr
    const std::string* idOfShortcut(UINT vk, UINT modifiers) const;
    // changes an item that exists: enabled / checked 0 or 1 (-1 leaves it), label "" leaves it. false if there is no such id.
    bool update(const std::string& id, int enabled, int checked, const std::string& label);

private:
    struct Item { std::string id, shortcut; UINT vk = 0, mods = 0; bool enabled = true; };
    HMENU root_ = nullptr;
    std::vector<Item> items_;                // command number = kFirstCommand + index
};

// Calls `onKey(virtual key, modifiers, repeat)` for every key press the page is about to get (repeat: the key is being held) and swallows the key when it
// returns true. UI thread.
void app_accelerators_install(void* controller, std::function<bool(UINT, UINT, bool)> onKey);
UINT app_current_modifiers();
