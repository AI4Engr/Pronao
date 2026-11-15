// Linux: files dragged from the file manager onto the window (see app_drop_accept in app_native.h). WebKitGTK gives the page the HTML drag events and a File for each
// dropped file, but a File has no path. The paths come from GTK's own drag data: the web view asks for the "text/uri-list" of a drop, this file sees that data first
// and keeps the paths of the LAST drop. When src/bridge.js has seen the page's `drop` event it calls __pronao_drop_native(x, y) (bound in app_shim.cpp), which queues
// them as the record "drop" "files" x y path... that shim/win/drop.cpp queues too. A page cannot make up a path: it can only ask for the ones the desktop handed this
// window, once each, within two seconds of the drop. Whether a path is granted is decided in src/drops.clg.
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {
const size_t kMaxPaths = 512;            // what one drop may carry through the native layer; the application's own limit is lower

struct Dropped {
    std::vector<std::string> paths;
    gint64 when = 0;                     // g_get_monotonic_time() at the drop
};

std::mutex g_mutex;
std::map<int, Dropped> g_last;           // handle -> the paths of its last drop, until asked for
std::map<int, bool> g_on;                // handle -> drops are switched on

void onDataReceived(GtkWidget*, GdkDragContext*, gint, gint, GtkSelectionData* data, guint, guint, gpointer p) {
    const int handle = *static_cast<int*>(p);
    gchar** uris = data ? gtk_selection_data_get_uris(data) : nullptr;
    if (!uris) return;
    Dropped d;
    for (gchar** u = uris; *u && d.paths.size() < kMaxPaths; u++) {
        gchar* name = g_filename_from_uri(*u, nullptr, nullptr);              // (only file: addresses on this machine have a path)
        if (name) { if (name[0] == '/') d.paths.push_back(name); g_free(name); }
    }
    g_strfreev(uris);
    d.when = g_get_monotonic_time();
    if (d.paths.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_last[handle] = d;
}

void freeInt(gpointer p, GClosure*) { delete static_cast<int*>(p); }

// the page learns whether drops are switched on from window.__pronao_drop (src/bridge.js)
void tellPage(WebKitWebView* view, bool on) {
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS                  // (webkit_web_view_evaluate_javascript, its successor, needs WebKitGTK 2.40; Ubuntu 22.04 has older)
    webkit_web_view_run_javascript(view, on ? "window.__pronao_drop=true;" : "window.__pronao_drop=false;", nullptr, nullptr, nullptr);
    G_GNUC_END_IGNORE_DEPRECATIONS
}
}

int app_drop_set(void* nativeWindow, void* controller, int handle, bool on) {
    WebKitWebView* view = controller ? WEBKIT_WEB_VIEW(controller) : nullptr;
    if (!nativeWindow || !view) return 4;
    GtkWidget* widget = GTK_WIDGET(view);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_on[handle] = on;
        if (!on) g_last.erase(handle);
    }
    if (on && !g_object_get_data(G_OBJECT(view), "pronao-drop")) {
        g_object_set_data(G_OBJECT(view), "pronao-drop", GINT_TO_POINTER(1));
        g_signal_connect_data(widget, "drag-data-received", G_CALLBACK(onDataReceived), new int(handle), freeInt, static_cast<GConnectFlags>(0));
        WebKitUserContentManager* content = webkit_web_view_get_user_content_manager(view);
        WebKitUserScript* script = webkit_user_script_new("window.__pronao_drop=true;", WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
        webkit_user_content_manager_add_script(content, script);            // for every document that follows
        webkit_user_script_unref(script);
    }
    tellPage(view, on);
    return 0;
}

void app_drop_forget(void* nativeWindow) { (void)nativeWindow; }

// the page's `drop` event: queue the paths of the drop it is about (once)
void app_drop_deliver(int handle, long x, long y) {
    Dropped d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto on = g_on.find(handle);
        auto it = g_last.find(handle);
        if (on == g_on.end() || !on->second || it == g_last.end()) return;
        d = it->second;
        g_last.erase(it);
    }
    if (g_get_monotonic_time() - d.when > 2 * G_USEC_PER_SEC) return;
    std::string record("drop\0files\0", 11);
    record += std::to_string(x); record += '\0';
    record += std::to_string(y); record += '\0';
    for (const auto& path : d.paths) { record += path; record += '\0'; }
    app_push(handle, std::string(), record, 100);
}

void app_drop_closed(int handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_on.erase(handle);
    g_last.erase(handle);
}
