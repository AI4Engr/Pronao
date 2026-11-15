// Linux: the tray icon and its menu (see app_tray_open in app_native.h), as a StatusNotifierItem with a com.canonical.dbusmenu menu on the session bus. That is the protocol
// KDE, XFCE, Cinnamon, MATE, LXQt, Budgie and (with the AppIndicator extension, which many distributions ship) GNOME use; the desktop draws the icon and the menu, so what
// the menu looks like is the desktop's. A desktop that shows no tray has no org.kde.StatusNotifierWatcher: then app_tray_open answers 4 and the application is told
// it has no tray icon (the program should still be usable without one). Records: "tray" click, "menu" id. A middle click (SecondaryActivate) is not reported:
// there is no double click on this protocol.
// Each icon has a bus connection and a thread of its own, running a GLib loop; what the menu holds is decided in src/menu.clg and arrives as the flat spec of app_menu_set.
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../app_native.h"
#include "../app_shim_internal.h"

extern const unsigned char app_icon_png[] __attribute__((weak));
extern const unsigned long app_icon_png_size __attribute__((weak));

namespace {

const char* kItemXml =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "<signal name='NewTitle'/><signal name='NewIcon'/><signal name='NewToolTip'/><signal name='NewStatus'><arg type='s'/></signal>"
    "<property name='Category' type='s' access='read'/><property name='Id' type='s' access='read'/><property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/><property name='WindowId' type='i' access='read'/><property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/><property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/><property name='Menu' type='o' access='read'/>"
    "</interface></node>";

const char* kMenuXml =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/><arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/></method>"
    "<method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/><arg type='a(ia{sv})' direction='out'/></method>"
    "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
    "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
    "<method name='EventGroup'><arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/></method>"
    "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "<method name='AboutToShowGroup'><arg type='ai' direction='in'/><arg type='ai' direction='out'/><arg type='ai' direction='out'/></method>"
    "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
    "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "<signal name='ItemActivationRequested'><arg type='i'/><arg type='u'/></signal>"
    "<property name='Version' type='u' access='read'/><property name='TextDirection' type='s' access='read'/><property name='Status' type='s' access='read'/>"
    "<property name='IconThemePath' type='as' access='read'/>"
    "</interface></node>";

struct Node {
    int id = 0;
    std::string cid;                                 // the application's id ("" for a separator, a submenu and the root)
    std::string label;                               // GTK-style: "_" is the mnemonic
    bool enabled = true, checked = false, check = false, separator = false, submenu = false;
    std::vector<int> children;
};

struct Tray {
    int handle = 0;
    std::mutex m;                                    // guards the fields below
    std::vector<Node> nodes;                         // nodes[0] is the root; a node's id is its index
    unsigned revision = 1;
    std::string title;
    GDBusConnection* connection = nullptr;
    std::thread loop;
    GMainLoop* mainLoop = nullptr;
    std::condition_variable cv;
    bool started = false, ok = false;
    std::string service;
    guint ownerId = 0, watchId = 0;
    bool watcherSeen = false;
};

std::mutex g_mutex;
std::map<int, std::shared_ptr<Tray>> g_trays;
std::atomic<int> g_serial{0};

std::string mnemonic(const std::string& label) {
    std::string out;
    for (size_t i = 0; i < label.size(); i++) {
        if (label[i] == '&' && i + 1 < label.size() && label[i + 1] == '&') { out += '&'; i++; }
        else if (label[i] == '&') out += '_';
        else if (label[i] == '_') out += "__";
        else out += label[i];
    }
    return out;
}

std::vector<std::string> fieldsOf(const std::string& spec) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : spec) {
        if (c == '\0') { f.push_back(cur); cur.clear(); } else cur += c;
    }
    return f;
}

// the flat spec of src/menu.clg -> nodes; false if it does not parse
bool parseSpec(const std::string& spec, std::vector<Node>& nodes) {
    const std::vector<std::string> f = fieldsOf(spec);
    if (f.empty() || f.size() % 7 != 0 || f.size() / 7 > 400) return false;
    nodes.clear();
    nodes.push_back(Node());
    nodes[0].submenu = true;
    std::vector<int> stack{0};
    for (size_t i = 0; i < f.size(); i += 7) {
        const std::string &kind = f[i], &id = f[i + 1], &label = f[i + 2];
        unsigned long flags = 0;
        try { flags = std::stoul(f[i + 6]); } catch (...) { return false; }
        if (kind == "e") {
            if (stack.size() < 2) return false;
            stack.pop_back();
            continue;
        }
        Node n;
        n.id = static_cast<int>(nodes.size());
        if (kind == "s") {
            n.separator = true;
        } else if (kind == "b" || kind == "i" || kind == "c") {
            if (label.empty()) return false;
            n.label = mnemonic(label);
            n.submenu = kind == "b";
            n.check = kind == "c";
            if (!n.submenu) {
                n.cid = id;
                n.enabled = (flags & 2) == 0;
                n.checked = (flags & 1) != 0;
            }
        } else {
            return false;
        }
        nodes[static_cast<size_t>(stack.back())].children.push_back(n.id);
        nodes.push_back(n);
        if (kind == "b") stack.push_back(n.id);
    }
    return stack.size() == 1;
}

GVariant* propertiesOf(const Node& n) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (n.id == 0) {
        g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
    } else if (n.separator) {
        g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
    } else {
        g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(n.label.c_str()));
        g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(n.enabled ? TRUE : FALSE));
        if (n.check) {
            g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
            g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(n.checked ? 1 : 0));
        }
        if (n.submenu) g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
    }
    return g_variant_builder_end(&b);
}

// (ia{sv}av) for a node and, down to `depth` levels (-1 = all), its children
GVariant* layoutOf(const std::vector<Node>& nodes, int id, int depth) {
    const Node& n = nodes[static_cast<size_t>(id)];
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    if (depth != 0)
        for (int c : n.children) g_variant_builder_add(&kids, "v", layoutOf(nodes, c, depth < 0 ? -1 : depth - 1));
    return g_variant_new("(i@a{sv}av)", id, propertiesOf(n), &kids);
}

GVariant* pixmaps() {
    GVariantBuilder list;
    g_variant_builder_init(&list, G_VARIANT_TYPE("a(iiay)"));
    if (app_icon_png && app_icon_png_size > 0) {
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        if (loader && gdk_pixbuf_loader_write(loader, app_icon_png, app_icon_png_size, nullptr) && gdk_pixbuf_loader_close(loader, nullptr)) {
            GdkPixbuf* src = gdk_pixbuf_loader_get_pixbuf(loader);
            GdkPixbuf* scaled = src ? gdk_pixbuf_scale_simple(src, 32, 32, GDK_INTERP_BILINEAR) : nullptr;
            if (scaled) {
                const int w = gdk_pixbuf_get_width(scaled), h = gdk_pixbuf_get_height(scaled), stride = gdk_pixbuf_get_rowstride(scaled), channels = gdk_pixbuf_get_n_channels(scaled);
                const guchar* px = gdk_pixbuf_get_pixels(scaled);
                std::vector<guchar> argb(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        const guchar* p = px + y * stride + x * channels;
                        guchar* q = &argb[(static_cast<size_t>(y) * w + x) * 4];
                        q[0] = channels == 4 ? p[3] : 255;               // network byte order: alpha, red, green, blue
                        q[1] = p[0]; q[2] = p[1]; q[3] = p[2];
                    }
                g_variant_builder_add(&list, "(ii@ay)", w, h, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb.data(), argb.size(), 1));
                g_object_unref(scaled);
            }
        }
        if (loader) g_object_unref(loader);
    }
    return g_variant_builder_end(&list);
}

void tell(int handle, const char* kind, const std::string& what) {
    std::string record(kind);
    record += '\0';
    record += what;
    record += '\0';
    app_push(handle, std::string(), record, 100);
}

// ---- org.kde.StatusNotifierItem -----------------------------------------------------------------------------------------------------------------------------------------

void itemCall(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant*, GDBusMethodInvocation* inv, gpointer data) {
    Tray* t = static_cast<Tray*>(data);
    if (std::string(method) == "Activate") tell(t->handle, "tray", "click");
    g_dbus_method_invocation_return_value(inv, nullptr);
}

GVariant* itemProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**, gpointer data) {
    Tray* t = static_cast<Tray*>(data);
    const std::string n(name);
    if (n == "Category") return g_variant_new_string("ApplicationStatus");
    if (n == "Id") return g_variant_new_string(t->service.c_str());
    if (n == "Title") { std::lock_guard<std::mutex> lock(t->m); return g_variant_new_string(t->title.c_str()); }
    if (n == "Status") return g_variant_new_string("Active");
    if (n == "WindowId") return g_variant_new_int32(0);
    if (n == "IconName") return g_variant_new_string(app_icon_png && app_icon_png_size > 0 ? "" : "application-x-executable");
    if (n == "IconPixmap") return pixmaps();
    if (n == "ToolTip") {
        std::lock_guard<std::mutex> lock(t->m);
        GVariantBuilder none;
        g_variant_builder_init(&none, G_VARIANT_TYPE("a(iiay)"));
        return g_variant_new("(s@a(iiay)ss)", "", g_variant_builder_end(&none), t->title.c_str(), "");
    }
    if (n == "ItemIsMenu") return g_variant_new_boolean(FALSE);
    if (n == "Menu") return g_variant_new_object_path("/MenuBar");
    return nullptr;
}

// ---- com.canonical.dbusmenu -----------------------------------------------------------------------------------------------------------------------------------------------

void menuCall(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* params, GDBusMethodInvocation* inv, gpointer data) {
    Tray* t = static_cast<Tray*>(data);
    const std::string m(method);
    std::lock_guard<std::mutex> lock(t->m);
    if (m == "GetLayout") {
        gint parent = 0, depth = -1;
        g_variant_get(params, "(ii^as)", &parent, &depth, nullptr);
        if (parent < 0 || static_cast<size_t>(parent) >= t->nodes.size()) parent = 0;
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@(ia{sv}av))", t->revision, layoutOf(t->nodes, parent, depth)));
    } else if (m == "GetGroupProperties") {
        GVariantIter* ids = nullptr;
        g_variant_get(params, "(aias)", &ids, nullptr);
        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        gint id;
        while (g_variant_iter_next(ids, "i", &id))
            if (id >= 0 && static_cast<size_t>(id) < t->nodes.size()) g_variant_builder_add(&out, "(i@a{sv})", id, propertiesOf(t->nodes[static_cast<size_t>(id)]));
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &out));
    } else if (m == "GetProperty") {
        gint id = 0;
        const gchar* name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant* found = nullptr;
        if (id >= 0 && static_cast<size_t>(id) < t->nodes.size()) {
            GVariant* all = propertiesOf(t->nodes[static_cast<size_t>(id)]);
            found = g_variant_lookup_value(all, name, nullptr);
            g_variant_unref(all);
        }
        if (found) g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", found));
        else g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "no such property");
    } else if (m == "Event" || m == "EventGroup") {
        std::vector<std::pair<gint, std::string>> events;
        if (m == "Event") {
            gint id = 0;
            const gchar* type = nullptr;
            g_variant_get(params, "(i&svu)", &id, &type, nullptr, nullptr);
            events.push_back({id, type});
        } else {
            GVariantIter* it = nullptr;
            g_variant_get(params, "(a(isvu))", &it);
            gint id;
            const gchar* type;
            while (g_variant_iter_next(it, "(i&svu)", &id, &type, nullptr, nullptr)) events.push_back({id, type});
            g_variant_iter_free(it);
        }
        for (const auto& e : events) {
            if (e.second != "clicked" || e.first <= 0 || static_cast<size_t>(e.first) >= t->nodes.size()) continue;
            const Node& n = t->nodes[static_cast<size_t>(e.first)];
            if (!n.submenu && !n.separator && n.enabled && !n.cid.empty()) tell(t->handle, "menu", n.cid);       // only ids of the application's own menu
        }
        if (m == "Event") {
            g_dbus_method_invocation_return_value(inv, nullptr);
        } else {
            GVariantBuilder none;
            g_variant_builder_init(&none, G_VARIANT_TYPE("ai"));
            g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", &none));
        }
    } else if (m == "AboutToShow") {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (m == "AboutToShowGroup") {
        GVariantBuilder a, b;
        g_variant_builder_init(&a, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&b, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(aiai)", &a, &b));
    } else {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "unknown method");
    }
}

GVariant* menuProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**, gpointer) {
    const std::string n(name);
    if (n == "Version") return g_variant_new_uint32(3);
    if (n == "TextDirection") return g_variant_new_string("ltr");
    if (n == "Status") return g_variant_new_string("normal");
    if (n == "IconThemePath") { GVariantBuilder none; g_variant_builder_init(&none, G_VARIANT_TYPE("as")); return g_variant_builder_end(&none); }
    return nullptr;
}

// ---- registering with the desktop's watcher -----------------------------------------------------------------------------------------------------------------------

void registerItem(Tray* t) {
    g_dbus_connection_call(t->connection, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", t->service.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr, nullptr);
}

// the watcher came back (a panel restarted): register again. The first call is for the watcher that was already there, which registerItem has just told.
void watcherAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer data) {
    Tray* t = static_cast<Tray*>(data);
    if (!t->watcherSeen) { t->watcherSeen = true; return; }
    registerItem(t);
}

void emitLayoutChanged(Tray* t) {
    unsigned revision;
    { std::lock_guard<std::mutex> lock(t->m); revision = ++t->revision; }
    g_dbus_connection_emit_signal(t->connection, nullptr, "/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated", g_variant_new("(ui)", revision, 0), nullptr);
}

void runTray(Tray* t) {
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    bool ok = false;
    GError* error = nullptr;
    gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error) { g_error_free(error); error = nullptr; }
    if (address) {
        t->connection = g_dbus_connection_new_for_address_sync(address, static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                                                               nullptr, nullptr, &error);
        if (error) { g_error_free(error); error = nullptr; }
        g_free(address);
    }
    if (t->connection) {
        // is there a tray on this desktop at all?
        GVariant* owner = g_dbus_connection_call_sync(t->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                                                      g_variant_new("(s)", "org.kde.StatusNotifierWatcher"), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
        gboolean has = FALSE;
        if (owner) { g_variant_get(owner, "(b)", &has); g_variant_unref(owner); }
        if (has) {
            GDBusNodeInfo* item = g_dbus_node_info_new_for_xml(kItemXml, nullptr);
            GDBusNodeInfo* menu = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
            static const GDBusInterfaceVTable itemTable = {itemCall, itemProperty, nullptr, {nullptr}};
            static const GDBusInterfaceVTable menuTable = {menuCall, menuProperty, nullptr, {nullptr}};
            t->service = "org.kde.StatusNotifierItem-" + std::to_string(static_cast<long>(getpid())) + "-" + std::to_string(++g_serial);
            const guint a = g_dbus_connection_register_object(t->connection, "/StatusNotifierItem", item->interfaces[0], &itemTable, t, nullptr, nullptr);
            const guint b = g_dbus_connection_register_object(t->connection, "/MenuBar", menu->interfaces[0], &menuTable, t, nullptr, nullptr);
            // the name is requested on this connection, so it goes away with the icon (the desktop notices that and removes the icon)
            GVariant* named = g_dbus_connection_call_sync(t->connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                                                          g_variant_new("(su)", t->service.c_str(), 0u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
            if (named) g_variant_unref(named);
            if (a && b && named) {
                registerItem(t);
                t->watchId = g_bus_watch_name_on_connection(t->connection, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE, watcherAppeared, nullptr, t, nullptr);
                ok = true;
            }
            g_dbus_node_info_unref(item);
            g_dbus_node_info_unref(menu);
        }
    }
    {
        std::lock_guard<std::mutex> lock(t->m);
        t->ok = ok;
        t->started = true;
        if (ok) t->mainLoop = g_main_loop_new(context, FALSE);
    }
    t->cv.notify_all();
    if (ok) g_main_loop_run(t->mainLoop);
    if (t->watchId) g_bus_unwatch_name(t->watchId);
    if (t->connection) {
        g_dbus_connection_close_sync(t->connection, nullptr, nullptr);          // the bus name goes: the desktop removes the icon
        g_object_unref(t->connection);
        t->connection = nullptr;
    }
    if (t->mainLoop) g_main_loop_unref(t->mainLoop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}

std::shared_ptr<Tray> trayOf(int handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_trays.find(handle);
    return it == g_trays.end() ? nullptr : it->second;
}

}  // namespace

int app_tray_open(std::string tooltip, std::string menuSpec, int& handle) {
    handle = 0;
    auto t = std::make_shared<Tray>();
    if (!parseSpec(menuSpec, t->nodes)) return 5;
    t->title = tooltip;
    t->handle = app_queue_entry_new();
    if (t->handle == 0) return 4;
    t->loop = std::thread(runTray, t.get());
    {
        std::unique_lock<std::mutex> lock(t->m);
        t->cv.wait(lock, [&] { return t->started; });
    }
    if (!t->ok) {
        t->loop.join();
        app_queue_entry_end(t->handle);
        return 4;
    }
    { std::lock_guard<std::mutex> lock(g_mutex); g_trays[t->handle] = t; }
    handle = t->handle;
    return 0;
}

int app_tray_update(int handle, std::string tooltip, std::string menuSpec) {
    auto t = trayOf(handle);
    if (!t) return 4;
    if (!menuSpec.empty()) {
        std::vector<Node> nodes;
        if (!parseSpec(menuSpec, nodes)) return 5;
        { std::lock_guard<std::mutex> lock(t->m); t->nodes = nodes; }
        emitLayoutChanged(t.get());
    }
    if (!tooltip.empty()) {
        { std::lock_guard<std::mutex> lock(t->m); t->title = tooltip; }
        g_dbus_connection_emit_signal(t->connection, nullptr, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewToolTip", nullptr, nullptr);
        g_dbus_connection_emit_signal(t->connection, nullptr, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewTitle", nullptr, nullptr);
    }
    return 0;
}

int app_tray_item(int handle, std::string id, int enabled, int checked) {
    auto t = trayOf(handle);
    if (!t) return 4;
    int found = -1;
    GVariant* update = nullptr;
    {
        std::lock_guard<std::mutex> lock(t->m);
        for (Node& n : t->nodes) {
            if (n.cid.empty() || n.cid != id) continue;
            if (enabled >= 0) n.enabled = enabled != 0;
            if (checked >= 0) n.checked = checked != 0;
            found = n.id;
            GVariantBuilder items, removed;
            g_variant_builder_init(&items, G_VARIANT_TYPE("a(ia{sv})"));
            g_variant_builder_init(&removed, G_VARIANT_TYPE("a(ias)"));
            g_variant_builder_add(&items, "(i@a{sv})", n.id, propertiesOf(n));
            update = g_variant_new("(a(ia{sv})a(ias))", &items, &removed);
            break;
        }
    }
    if (found < 0) return 5;
    g_dbus_connection_emit_signal(t->connection, nullptr, "/MenuBar", "com.canonical.dbusmenu", "ItemsPropertiesUpdated", update, nullptr);
    return 0;
}

void app_tray_forget(int handle) {
    std::shared_ptr<Tray> t;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_trays.find(handle);
        if (it == g_trays.end()) return;
        t = it->second;
        g_trays.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(t->m);
        if (t->mainLoop) g_main_loop_quit(t->mainLoop);
    }
    if (t->loop.joinable()) t->loop.join();
}
