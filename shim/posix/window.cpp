// Linux: what the program may ask of its window beyond showing a page (GTK 3), each run on the window's UI thread through app_ui_call: minimize, maximize, hide,
// close, full screen, always on top, no frame, fixed size, starting a move or a resize from the page, being modal for another window, the monitors' work areas,
// where the window is, and the light or dark scheme. The same meanings as the Windows code in shim/app_shim.cpp; the numbers are those of app_native.h.
// Differences that come from the platform, not from this file: a window manager may ignore a request (a tiling one ignores moving and resizing), and under Wayland a
// program cannot put a window where it likes or know where it is, so the placement calls are only as good as the compositor lets them be (run with GDK_BACKEND=x11 for X11).
#include <gtk/gtk.h>

#include <map>
#include <mutex>
#include <string>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {
GtkWindow* gtkWindow(void* native) { return native ? GTK_WINDOW(native) : nullptr; }
}

// 0 restore, 1 minimize, 2 maximize, 3 bring to the front, 4 close (through the same "delete-event" as the X, so app.confirmClose is asked), 5 hide, 6 show and bring to the front
int app_linux_state(int handle, int state) {
    return app_ui_call(handle, [state](void* native) {
        GtkWindow* w = gtkWindow(native);
        if (!w) return;
        switch (state) {
            case 1: gtk_window_iconify(w); break;
            case 2: gtk_window_maximize(w); break;
            case 3: gtk_window_present(w); break;
            case 4: gtk_window_close(w); break;
            case 5: gtk_widget_hide(GTK_WIDGET(w)); break;
            case 6: gtk_widget_show(GTK_WIDGET(w)); gtk_window_present(w); break;
            default: gtk_window_unmaximize(w); gtk_window_deiconify(w); gtk_window_present(w); break;
        }
    }) ? 1 : 0;
}

// the work area of every monitor, the primary one first: left, top, right, bottom, each number ended by a NUL
int app_linux_monitors(int handle, std::string& out) {
    std::string primary, others;
    const int ran = app_ui_call(handle, [&](void*) {
        GdkDisplay* display = gdk_display_get_default();
        if (!display) return;
        const int n = gdk_display_get_n_monitors(display);
        for (int i = 0; i < n; i++) {
            GdkMonitor* m = gdk_display_get_monitor(display, i);
            if (!m) continue;
            GdkRectangle r;
            gdk_monitor_get_workarea(m, &r);
            std::string one;
            for (long v : {static_cast<long>(r.x), static_cast<long>(r.y), static_cast<long>(r.x + r.width), static_cast<long>(r.y + r.height)}) { one += std::to_string(v); one += '\0'; }
            (gdk_monitor_is_primary(m) ? primary : others) += one;
        }
    });
    out = primary + others;
    return ran && !out.empty() ? 0 : 4;
}

// "x y width height state", state: 1 maximized, 2 minimized, 4 full screen, 8 on top, 16 no frame, 32 not resizable, 64 hidden
int app_linux_rect(int handle, std::string& out) {
    out.clear();
    std::string text;
    const int ran = app_ui_call(handle, [&](void* native) {
        GtkWindow* w = gtkWindow(native);
        if (!w) return;
        gint x = 0, y = 0, width = 0, height = 0;
        gtk_window_get_position(w, &x, &y);
        gtk_window_get_size(w, &width, &height);
        GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(w));
        const GdkWindowState st = gw ? gdk_window_get_state(gw) : static_cast<GdkWindowState>(0);
        const int state = (gtk_window_is_maximized(w) ? 1 : 0) | ((st & GDK_WINDOW_STATE_ICONIFIED) ? 2 : 0) | ((st & GDK_WINDOW_STATE_FULLSCREEN) ? 4 : 0) |
                          ((st & GDK_WINDOW_STATE_ABOVE) ? 8 : 0) | (gtk_window_get_decorated(w) ? 0 : 16) | (gtk_window_get_resizable(w) ? 0 : 32) |
                          (gtk_widget_get_visible(GTK_WIDGET(w)) ? 0 : 64);
        text = std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(width) + " " + std::to_string(height) + " " + std::to_string(state);
    });
    if (!ran || text.empty()) return 4;
    out = text;
    return 0;
}

// what: 1 full screen, 2 always on top, 3 no frame, 4 resizable
int app_linux_style(int handle, int what, int on) {
    return app_ui_call(handle, [what, on](void* native) {
        GtkWindow* w = gtkWindow(native);
        if (!w) return;
        if (what == 1) { if (on) gtk_window_fullscreen(w); else gtk_window_unfullscreen(w); }
        else if (what == 2) gtk_window_set_keep_above(w, on ? TRUE : FALSE);
        else if (what == 3) gtk_window_set_decorated(w, on ? FALSE : TRUE);
        else if (what == 4) gtk_window_set_resizable(w, on ? TRUE : FALSE);
    }) ? 1 : 0;
}

// The page asks for a move (edge 0) or a resize from one of the edges (the Windows hit-test codes 10 to 17), only while the left button is down: a page cannot start
// a move out of nothing.
int app_linux_drag(int handle, int edge) {
    return app_ui_call(handle, [edge](void* native) {
        GtkWindow* w = gtkWindow(native);
        if (!w) return;
        GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(w));
        GdkSeat* seat = gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(w)));
        GdkDevice* pointer = seat ? gdk_seat_get_pointer(seat) : nullptr;
        if (!gw || !pointer) return;
        gint wx = 0, wy = 0, rx = 0, ry = 0;
        GdkModifierType mask = static_cast<GdkModifierType>(0);
        gdk_window_get_device_position(gw, pointer, &wx, &wy, &mask);
        if (!(mask & GDK_BUTTON1_MASK)) return;
        gdk_device_get_position(pointer, nullptr, &rx, &ry);
        if (edge == 0) { gtk_window_begin_move_drag(w, 1, rx, ry, GDK_CURRENT_TIME); return; }
        static const std::map<int, GdkWindowEdge> edges = {{10, GDK_WINDOW_EDGE_WEST}, {11, GDK_WINDOW_EDGE_EAST}, {12, GDK_WINDOW_EDGE_NORTH}, {13, GDK_WINDOW_EDGE_NORTH_WEST},
                                                           {14, GDK_WINDOW_EDGE_NORTH_EAST}, {15, GDK_WINDOW_EDGE_SOUTH}, {16, GDK_WINDOW_EDGE_SOUTH_WEST}, {17, GDK_WINDOW_EDGE_SOUTH_EAST}};
        auto it = edges.find(edge);
        if (it != edges.end()) gtk_window_begin_resize_drag(w, it->second, 1, rx, ry, GDK_CURRENT_TIME);
    }) ? 1 : 0;
}

// A window that is modal for another: the window manager keeps it in front of its owner and the owner takes no input until it closes.
int app_linux_modal(int child, int owner, int on) {
    void* ownerWindow = nullptr;
    if (!app_ui_call(owner, [&](void* native) { ownerWindow = native; }) || !ownerWindow) return 4;
    return app_ui_call(child, [=](void* native) {
        GtkWindow* c = gtkWindow(native);
        if (!c) return;
        gtk_window_set_transient_for(c, on ? gtkWindow(ownerWindow) : nullptr);
        gtk_window_set_modal(c, on ? TRUE : FALSE);
        if (on) gtk_window_present(c);
    }) ? 0 : 4;
}

namespace {
struct Wanted {
    GtkWindow* window;
    int x, y;
};

// Under some window managers a window that was moved to (x, y) is then reported at another place (with client-side decorations the invisible border of the
// window counts in one of the two calls and not in the other; seen under Weston's window manager in WSLg: 32 pixels). A place that is saved and applied would then creep
// by that much at every start, so once the window has settled, what it reports is brought to what was asked for.
gboolean settle(gpointer data) {
    Wanted* w = static_cast<Wanted*>(data);
    GdkWindow* gw = gtk_widget_get_window(GTK_WIDGET(w->window));
    if (gw && !(gdk_window_get_state(gw) & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_ICONIFIED))) {
        gint x = 0, y = 0;
        gtk_window_get_position(w->window, &x, &y);
        const int dx = w->x - x, dy = w->y - y;
        if ((dx != 0 || dy != 0) && dx > -200 && dx < 200 && dy > -200 && dy < 200) gtk_window_move(w->window, w->x + dx, w->y + dy);
    }
    g_object_unref(w->window);
    delete w;
    return G_SOURCE_REMOVE;
}
}

static gboolean maximizeLater(gpointer window) {
    gtk_window_maximize(GTK_WINDOW(window));
    g_object_unref(window);
    return G_SOURCE_REMOVE;
}

int app_linux_placement(int handle, int x, int y, int width, int height, int maximized) {
    return app_ui_call(handle, [=](void* native) {
        GtkWindow* w = gtkWindow(native);
        if (!w) return;
        gtk_window_resize(w, width, height);
        gtk_window_move(w, x, y);
        // A window that is resized and maximized before it has been shown at that size is restored by the window manager to the size it first had (200 x 200 here), not
        // to the one asked for: so it is first given time to take its size and place, and maximized after that.
        if (maximized) g_timeout_add(300, maximizeLater, g_object_ref(w));
        else g_timeout_add(250, settle, new Wanted{GTK_WINDOW(g_object_ref(w)), x, y});
    }) ? 1 : 0;
}

// ---- the colour scheme --------------------------------------------------------------------------------------------------------------------------------------------------
// GTK's own preference for a dark theme is what the page's prefers-color-scheme follows in WebKitGTK, so setting it sets both the window and the page.

namespace {
std::mutex g_themeMutex;
bool g_capturedTheme = false;
gboolean g_originalDark = FALSE;
}

// 1 dark, 0 light, -1 not known: the preference the user's session sets (what GTK said before this program changed it)
int app_theme_system() {
    GtkSettings* s = gtk_settings_get_default();
    if (!s) return -1;
    std::lock_guard<std::mutex> lock(g_themeMutex);
    gboolean dark = FALSE;
    gchar* theme = nullptr;
    g_object_get(s, "gtk-application-prefer-dark-theme", &dark, "gtk-theme-name", &theme, nullptr);
    const bool named = theme && std::string(theme).find("dark") != std::string::npos;
    g_free(theme);
    return (g_capturedTheme ? g_originalDark : dark) || named ? 1 : 0;
}

// mode 0 follow the system, 1 light, 2 dark (run on the UI thread)
int app_linux_theme(int handle, int mode) {
    int rc = 4;
    const int ran = app_ui_call(handle, [&](void*) {
        GtkSettings* s = gtk_settings_get_default();
        if (!s) return;
        gboolean original;
        {
            std::lock_guard<std::mutex> lock(g_themeMutex);
            if (!g_capturedTheme) { g_object_get(s, "gtk-application-prefer-dark-theme", &g_originalDark, nullptr); g_capturedTheme = true; }
            original = g_originalDark;
        }
        // (not under the lock: setting the preference makes GTK emit "notify", whose handler asks app_theme_system, which takes the lock)
        g_object_set(s, "gtk-application-prefer-dark-theme", mode == 0 ? original : (mode == 2 ? TRUE : FALSE), nullptr);
        rc = 0;
    });
    return ran ? rc : 4;
}
