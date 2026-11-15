// Linux: system notifications (see app_tray_notify in app_native.h) through the desktop's own notification service, org.freedesktop.Notifications on the session bus,
// which GNOME, KDE, XFCE and the other desktops all provide. No tray icon is involved (a Linux desktop has no common one). A notification that carries an id has a default
// action; the service tells when the user clicked it ("ActionInvoked"), and that is queued as the record "notify" id.
// Without a session bus or a notification service app_notifier_open answers -1 and the application is told that notifications cannot be shown here.
// Messages go out from the caller's thread (a synchronous call with a time limit); the answers come back on a thread of their own that runs a GLib loop.
#include <gio/gio.h>
#include <glib.h>

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {

struct Pending {
    int handle;
    std::string id;
};

struct Service {
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    GDBusConnection* connection = nullptr;
    std::map<guint32, Pending> pending;           // notification number -> who is told on a click (the oldest are forgotten at 64)
    std::thread loop;
    std::string appName = "pronao";
};
Service* g_service = nullptr;

void onSignal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* signal, GVariant* parameters, gpointer) {
    if (std::string(signal) != "ActionInvoked") return;
    guint32 number = 0;
    const gchar* action = nullptr;
    g_variant_get(parameters, "(u&s)", &number, &action);
    if (std::string(action ? action : "") != "default") return;
    Pending p;
    {
        std::lock_guard<std::mutex> lock(g_service->m);
        auto it = g_service->pending.find(number);
        if (it == g_service->pending.end()) return;
        p = it->second;
        g_service->pending.erase(it);
    }
    std::string record("notify", 6);
    record += '\0';
    record += p.id;
    record += '\0';
    app_push(p.handle, std::string(), record, 100);
}

void runLoop(Service* s) {
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error) g_error_free(error);
    if (connection) {
        g_dbus_connection_signal_subscribe(connection, "org.freedesktop.Notifications", "org.freedesktop.Notifications", "ActionInvoked", "/org/freedesktop/Notifications", nullptr,
                                           G_DBUS_SIGNAL_FLAGS_NONE, onSignal, nullptr, nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->connection = connection;
        s->started = true;
    }
    s->cv.notify_all();
    if (connection) {
        GMainLoop* loop = g_main_loop_new(context, FALSE);
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
    }
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}

Service* service() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_service = new Service();
        g_service->loop = std::thread(runLoop, g_service);
        g_service->loop.detach();
    });
    Service* s = g_service;
    std::unique_lock<std::mutex> lock(s->m);
    s->cv.wait(lock, [s] { return s->started; });
    return s->connection ? s : nullptr;
}

}  // namespace

int app_notifier_open(std::string tooltip, int& handle) {
    handle = 0;
    Service* s = service();
    if (!s) return -1;
    {
        std::lock_guard<std::mutex> lock(s->m);
        if (!tooltip.empty()) s->appName = tooltip.substr(0, 64);
    }
    handle = app_queue_entry_new();
    return handle > 0 ? 0 : 4;
}

int app_tray_notify(int handle, std::string title, std::string text, int kind, std::string id) {
    Service* s = service();
    if (!s) return -1;
    if (title.empty() || title.size() > 63 * 4 || text.size() > 255 * 4) return 5;
    std::string app;
    {
        std::lock_guard<std::mutex> lock(s->m);
        app = s->appName;
    }
    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    if (!id.empty()) {
        g_variant_builder_add(&actions, "s", "default");
        g_variant_builder_add(&actions, "s", "Open");
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(kind == 2 ? 2 : 1));
    const char* icon = kind == 2 ? "dialog-error" : kind == 1 ? "dialog-warning" : "dialog-information";
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(s->connection, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
                                                  g_variant_new("(susssasa{sv}i)", app.c_str(), 0u, icon, title.c_str(), text.c_str(), &actions, &hints, -1), G_VARIANT_TYPE("(u)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    if (!reply) {
        if (error) g_error_free(error);
        return 4;
    }
    guint32 number = 0;
    g_variant_get(reply, "(u)", &number);
    g_variant_unref(reply);
    if (!id.empty()) {
        std::lock_guard<std::mutex> lock(s->m);
        if (s->pending.size() >= 64) s->pending.erase(s->pending.begin());
        s->pending[number] = Pending{handle, id};
    }
    return 0;
}
