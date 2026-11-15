// Linux: the window's menu bar, pop-up menus and keyboard shortcuts (GTK 3), built from the flat spec of src/menu.clg (see app_menu_set in app_native.h). What a menu
// may contain, and every limit on it, is decided in src/menu.clg; this builds what it is given and refuses what does not parse. The numbers in the spec are the
// Windows virtual-key codes and modifier bits (Ctrl 1, Alt 2, Shift 4), so the same spec serves both; this file translates to and from GDK's key values.
// What leaves this file is an id the application gave: the page can neither add an item nor click one. A checked item is not flipped by a click (as on Windows): the
// application decides and says so with app_menu_item.
#include <gtk/gtk.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {

struct Item {
    std::string id, shortcut;
    unsigned vk = 0, mods = 0;
    bool enabled = true, checked = false, check = false;
    GtkWidget* widget = nullptr;
};

struct Model {
    GtkWidget* root = nullptr;                       // the GtkMenuBar or GtkMenu
    std::vector<Item> items;
    int handle = 0;
};

struct WindowMenu {
    std::shared_ptr<Model> model;
    GtkWidget* box = nullptr;
    GtkWidget* bar = nullptr;
    bool keysConnected = false;
    std::set<guint> held;                            // keys down, to tell a repeat from a press
};

std::mutex g_mutex;
std::map<int, std::shared_ptr<WindowMenu>> g_windows;      // handle -> menu state (the widgets are touched on the window's UI thread only)

std::shared_ptr<WindowMenu> windowOf(int handle, bool create) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_windows.find(handle);
    if (it != g_windows.end()) return it->second;
    if (!create) return nullptr;
    return g_windows[handle] = std::make_shared<WindowMenu>();
}

std::vector<std::string> fieldsOf(const std::string& spec) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : spec) {
        if (c == '\0') { f.push_back(cur); cur.clear(); } else cur += c;
    }
    return f;
}

bool toNumber(const std::string& s, unsigned& out) {
    if (s.empty() || s.size() > 9) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    out = v;
    return true;
}

// "&File" is a mnemonic on Windows ("&&" a real &); GTK writes it "_File" and a real underscore "__"
std::string mnemonicText(const std::string& label) {
    std::string out;
    for (size_t i = 0; i < label.size(); i++) {
        if (label[i] == '&' && i + 1 < label.size() && label[i + 1] == '&') { out += '&'; i++; }
        else if (label[i] == '&') out += '_';
        else if (label[i] == '_') out += "__";
        else out += label[i];
    }
    return out;
}

unsigned vkFor(guint key) {
    key = gdk_keyval_to_lower(key);
    if (key >= 'a' && key <= 'z') return key - 'a' + 'A';
    if (key >= '0' && key <= '9') return key;
    if (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) return 112 + (key - GDK_KEY_F1);
    switch (key) {
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: return 13;
        case GDK_KEY_Escape: return 27;
        case GDK_KEY_space: return 32;
        case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return 9;
        case GDK_KEY_BackSpace: return 8;
        case GDK_KEY_Delete: return 46;
        case GDK_KEY_Insert: return 45;
        case GDK_KEY_Home: return 36;
        case GDK_KEY_End: return 35;
        case GDK_KEY_Page_Up: return 33;
        case GDK_KEY_Page_Down: return 34;
        case GDK_KEY_Left: return 37;
        case GDK_KEY_Up: return 38;
        case GDK_KEY_Right: return 39;
        case GDK_KEY_Down: return 40;
    }
    return 0;
}

unsigned modsOf(guint state) {
    return ((state & GDK_CONTROL_MASK) ? 1u : 0u) | ((state & GDK_MOD1_MASK) ? 2u : 0u) | ((state & GDK_SHIFT_MASK) ? 4u : 0u);
}

void tell(int handle, const std::string& id) {
    std::string record("menu\0", 5);
    record += id;
    record += '\0';
    app_push(handle, std::string(), record, 100);
}

// A menu item's signal data. The model is held weakly: a replaced menu's widgets die with their root, and cannot reach into a freed model.
struct Hook {
    int handle;
    size_t index;
    std::weak_ptr<Model> model;
};

void freeHook(gpointer p, GClosure*) { delete static_cast<Hook*>(p); }

// gtk_check_menu_item_set_active works by activating the item, which would be told to the application as a click: while the program itself sets a tick, no click
bool g_settingTick = false;

void setTick(GtkWidget* widget, bool on) {
    const bool before = g_settingTick;
    g_settingTick = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(widget), on ? TRUE : FALSE);
    g_settingTick = before;
}

void onActivate(GtkMenuItem*, gpointer data) {
    if (g_settingTick) return;
    Hook* h = static_cast<Hook*>(data);
    std::shared_ptr<Model> m = h->model.lock();
    if (!m || h->index >= m->items.size()) return;
    Item& item = m->items[h->index];
    if (item.check) setTick(item.widget, item.checked);      // a click does not flip it: the application decides
    tell(h->handle, item.id);
}

// what a pop-up does instead: remember the pick and end the nested loop
struct PopupRun {
    GMainLoop* loop;
    std::string picked;
    std::shared_ptr<Model> model;
    guint timer = 0;                                 // the pending "the menu has closed" timeout, 0 if none
};
struct PopupHook {
    PopupRun* run;
    size_t index;
};

void onPopupPick(GtkMenuItem*, gpointer data) {
    PopupHook* h = static_cast<PopupHook*>(data);
    if (h->index < h->run->model->items.size()) h->run->picked = h->run->model->items[h->index].id;
    g_main_loop_quit(h->run->loop);
}

gboolean quitLater(gpointer data) {
    PopupRun* run = static_cast<PopupRun*>(data);
    run->timer = 0;
    g_main_loop_quit(run->loop);
    return G_SOURCE_REMOVE;
}

void onPopupDeactivate(GtkMenuShell*, gpointer data) {
    PopupRun* run = static_cast<PopupRun*>(data);
    if (!run->timer) run->timer = g_timeout_add(200, quitLater, run);       // GTK reports the pick after the menu has gone: give it a moment
}

// the item's content: the label on the left, the shortcut on the right
GtkWidget* contentFor(const std::string& label, const std::string& shortcut, GtkWidget* menuItem) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    GtkWidget* text = gtk_label_new_with_mnemonic(mnemonicText(label).c_str());
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_label_set_mnemonic_widget(GTK_LABEL(text), menuItem);
    gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
    if (!shortcut.empty()) {
        GtkWidget* key = gtk_label_new(shortcut.c_str());
        gtk_style_context_add_class(gtk_widget_get_style_context(key), "dim-label");
        gtk_box_pack_end(GTK_BOX(box), key, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(box);
    return box;
}

// forPopup: the items are wired by the caller (a pop-up returns the pick instead of queueing it)
std::shared_ptr<Model> build(int handle, const std::string& spec, bool bar, bool forPopup, std::string& error) {
    const std::vector<std::string> f = fieldsOf(spec);
    if (f.empty() || f.size() % 7 != 0 || f.size() / 7 > 400) { error = "the menu is empty or malformed"; return nullptr; }
    std::shared_ptr<Model> model = std::make_shared<Model>();
    model->handle = handle;
    model->root = bar ? gtk_menu_bar_new() : gtk_menu_new();
    g_object_ref_sink(model->root);
    std::vector<GtkWidget*> stack{model->root};
    auto fail = [&](const char* why) {
        error = why;
        gtk_widget_destroy(model->root);
        g_object_unref(model->root);
        return std::shared_ptr<Model>();
    };
    for (size_t i = 0; i < f.size(); i += 7) {
        const std::string &kind = f[i], &id = f[i + 1], &label = f[i + 2], &shortcut = f[i + 3];
        unsigned vk = 0, mods = 0, flags = 0;
        if (!toNumber(f[i + 4], vk) || !toNumber(f[i + 5], mods) || !toNumber(f[i + 6], flags)) return fail("a number in the menu is not a number");
        if (kind == "e") {
            if (stack.size() < 2) return fail("end without begin");
            stack.pop_back();
        } else if (kind == "s") {
            GtkWidget* sep = gtk_separator_menu_item_new();
            gtk_menu_shell_append(GTK_MENU_SHELL(stack.back()), sep);
            gtk_widget_show(sep);
        } else if (kind == "b") {
            if (label.empty()) return fail("a label is not valid text");
            GtkWidget* item = gtk_menu_item_new_with_mnemonic(mnemonicText(label).c_str());
            GtkWidget* sub = gtk_menu_new();
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
            gtk_menu_shell_append(GTK_MENU_SHELL(stack.back()), item);
            gtk_widget_show(item);
            stack.push_back(sub);
        } else if (kind == "i" || kind == "c") {
            if (label.empty()) return fail("a label is not valid text");
            if (model->items.size() >= 400) return fail("too many items");
            Item it;
            it.id = id;
            it.shortcut = shortcut;
            it.vk = vk;
            it.mods = mods;
            it.enabled = (flags & 2) == 0;
            it.checked = (flags & 1) != 0;
            it.check = kind == "c";
            GtkWidget* w = it.check ? gtk_check_menu_item_new() : gtk_menu_item_new();
            gtk_container_add(GTK_CONTAINER(w), contentFor(label, shortcut, w));
            if (it.check) setTick(w, it.checked);
            gtk_widget_set_sensitive(w, it.enabled ? TRUE : FALSE);
            it.widget = w;
            if (!forPopup) g_signal_connect_data(w, "activate", G_CALLBACK(onActivate), new Hook{handle, model->items.size(), model}, freeHook, G_CONNECT_AFTER);
            model->items.push_back(it);
            gtk_menu_shell_append(GTK_MENU_SHELL(stack.back()), w);
            gtk_widget_show(w);
        } else {
            return fail("unknown kind of entry");
        }
    }
    if (stack.size() != 1) return fail("a submenu was not closed");
    gtk_widget_show(model->root);
    return model;
}

gboolean onKeyPress(GtkWidget*, GdkEventKey* e, gpointer data) {
    const int handle = *static_cast<int*>(data);
    std::shared_ptr<WindowMenu> w = windowOf(handle, false);
    if (!w) return FALSE;
    GdkKeymap* keymap = gdk_keymap_get_for_display(gdk_display_get_default());
    guint base = e->keyval;
    gdk_keymap_translate_keyboard_state(keymap, e->hardware_keycode, static_cast<GdkModifierType>(0), e->group, &base, nullptr, nullptr, nullptr);     // the key without Shift
    const unsigned vk = vkFor(base ? base : e->keyval);
    if (vk == 0) return FALSE;
    const bool repeat = !w->held.insert(e->keyval).second;
    return app_menu_shortcut(handle, vk, modsOf(e->state), repeat) ? TRUE : FALSE;
}

gboolean onKeyRelease(GtkWidget*, GdkEventKey* e, gpointer data) {
    std::shared_ptr<WindowMenu> w = windowOf(*static_cast<int*>(data), false);
    if (w) w->held.erase(e->keyval);
    return FALSE;
}

void freeInt(gpointer p, GClosure*) { delete static_cast<int*>(p); }

}  // namespace

int app_menu_set(int handle, std::string spec) {
    int rc = 0;
    const int ran = app_ui_call(handle, [&](void* native) {
        GtkWindow* window = GTK_WINDOW(native);
        std::shared_ptr<Model> fresh;
        if (!spec.empty()) {
            std::string error;
            fresh = build(handle, spec, true, false, error);
            if (!fresh) { rc = 5; return; }
        }
        std::shared_ptr<WindowMenu> w = windowOf(handle, true);
        if (!w->box && fresh) {                                    // the first menu: a box above the page, which keeps the page the whole of the rest of the window
            GtkWidget* page = gtk_bin_get_child(GTK_BIN(window));
            GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            if (page) {
                g_object_ref(page);
                gtk_container_remove(GTK_CONTAINER(window), page);
                gtk_box_pack_end(GTK_BOX(box), page, TRUE, TRUE, 0);
                g_object_unref(page);
            }
            gtk_container_add(GTK_CONTAINER(window), box);
            gtk_widget_show(box);
            w->box = box;
        }
        if (w->bar) {
            gtk_widget_destroy(w->bar);
            w->bar = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            w->model = fresh;
        }
        if (fresh) {
            w->bar = fresh->root;
            gtk_box_pack_start(GTK_BOX(w->box), w->bar, FALSE, FALSE, 0);
            g_object_unref(fresh->root);                           // (the box owns it now)
            if (!w->keysConnected) {
                w->keysConnected = true;
                g_signal_connect_data(window, "key-press-event", G_CALLBACK(onKeyPress), new int(handle), freeInt, static_cast<GConnectFlags>(0));
                g_signal_connect_data(window, "key-release-event", G_CALLBACK(onKeyRelease), new int(handle), freeInt, static_cast<GConnectFlags>(0));
            }
        }
    });
    return ran ? rc : 4;
}

int app_menu_item(int handle, std::string id, int enabled, int checked) {
    std::shared_ptr<WindowMenu> w = windowOf(handle, false);
    if (!w) return 5;
    bool found = false;
    app_ui_call(handle, [&](void*) {
        std::shared_ptr<Model> m = w->model;
        if (!m) return;
        for (Item& item : m->items) {
            if (item.id != id) continue;
            if (enabled >= 0) {
                item.enabled = enabled != 0;
                gtk_widget_set_sensitive(item.widget, enabled ? TRUE : FALSE);
            }
            if (checked >= 0) {
                item.checked = checked != 0;
                if (item.check) setTick(item.widget, item.checked);
            }
            found = true;
            return;
        }
    });
    return found ? 0 : 5;
}

// A pop-up at the pointer. It blocks until closed (the nested loop runs on the UI thread, the caller waits).
int app_menu_popup(int handle, std::string spec, std::string& chosen) {
    chosen.clear();
    std::string picked;
    int rc = 0;
    const int ran = app_ui_call(handle, [&](void* native) {
        std::string error;
        std::shared_ptr<Model> m = build(handle, spec, false, true, error);
        if (!m) { rc = 5; return; }
        PopupRun run{g_main_loop_new(nullptr, FALSE), std::string(), m, 0};
        for (size_t i = 0; i < m->items.size(); i++)
            g_signal_connect_data(m->items[i].widget, "activate", G_CALLBACK(onPopupPick), new PopupHook{&run, i}, [](gpointer p, GClosure*) { delete static_cast<PopupHook*>(p); },
                                  static_cast<GConnectFlags>(0));
        g_signal_connect(m->root, "deactivate", G_CALLBACK(onPopupDeactivate), &run);
        // at the pointer, written as a rectangle of the window: with no event to attach to (this is called from a command, not from a click) popup_at_pointer has nothing to go on
        GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(native));
        GdkSeat* seat = gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(native)));
        GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
        gint px = 0, py = 0;
        if (gw && pointer) gdk_window_get_device_position(gw, pointer, &px, &py, nullptr);
        GdkRectangle at = {px, py, 1, 1};
        GdkEvent* trigger = gdk_event_new(GDK_BUTTON_PRESS);                  // (a stand-in for the click: GTK wants an event to attach the menu to)
        if (gw) trigger->button.window = static_cast<GdkWindow*>(g_object_ref(gw));
        trigger->button.time = GDK_CURRENT_TIME;
        trigger->button.button = 3;
        if (pointer) gdk_event_set_device(trigger, pointer);
        gtk_menu_popup_at_rect(GTK_MENU(m->root), gw, &at, GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, trigger);
        gdk_event_free(trigger);
        g_main_loop_run(run.loop);
        if (run.timer) g_source_remove(run.timer);
        g_main_loop_unref(run.loop);
        picked = run.picked;
        gtk_widget_destroy(m->root);
        g_object_unref(m->root);
    });
    if (!ran) return 4;
    if (rc != 0) return rc;
    chosen = picked;
    if (!chosen.empty()) tell(handle, chosen);
    return 0;
}

bool app_menu_command(int, unsigned) { return false; }          // (Windows only: a WM_COMMAND from the window procedure)

// a key the window got: true if it is the shortcut of an enabled item (then the page does not get it); a press of it is told to the application
bool app_menu_shortcut(int handle, unsigned vk, unsigned mods, bool repeat) {
    std::shared_ptr<Model> m;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_windows.find(handle);
        if (it != g_windows.end()) m = it->second->model;
    }
    if (!m) return false;
    for (const Item& item : m->items) {
        if (item.vk == 0 || item.vk != vk || item.mods != mods || !item.enabled) continue;
        if (!repeat) tell(handle, item.id);
        return true;
    }
    return false;
}

void app_menu_forget(int handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_windows.erase(handle);
}
