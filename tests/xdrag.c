/* A drag source for the Linux drop test: a small window titled "pronao drag source" that offers the files named on the command line as "text/uri-list" when the
 * user drags from it, which is what a file manager offers. The test (tests/run_system_linux.sh) presses the mouse on it with xdotool and moves over the program's window.
 *   xdrag file...      runs until the drag has ended (then prints "dragged" or "cancelled"), or 40 seconds */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

static char** g_files;
static int g_count;

static void on_get(GtkWidget* w, GdkDragContext* c, GtkSelectionData* data, guint info, guint time, gpointer p) {
    gchar** uris = g_new0(gchar*, g_count + 1);
    for (int i = 0; i < g_count; i++) uris[i] = g_filename_to_uri(g_files[i], NULL, NULL);
    gtk_selection_data_set_uris(data, uris);
    g_strfreev(uris);
}
static void on_end(GtkWidget* w, GdkDragContext* c, gpointer p) {
    printf("%s\n", gdk_drag_context_get_selected_action(c) ? "dragged" : "cancelled");
    fflush(stdout);
    gtk_main_quit();
}
static gboolean timeout_quit(gpointer p) { gtk_main_quit(); return FALSE; }

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    g_files = argv + 1;
    g_count = argc - 1;
    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "pronao drag source");
    gtk_window_set_default_size(GTK_WINDOW(win), 200, 120);
    GtkWidget* label = gtk_label_new("drag from here");
    GtkWidget* box = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(box), label);
    gtk_container_add(GTK_CONTAINER(win), box);
    GtkTargetEntry targets[] = {{(gchar*)"text/uri-list", 0, 0}};
    gtk_drag_source_set(box, GDK_BUTTON1_MASK, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(box, "drag-data-get", G_CALLBACK(on_get), NULL);
    g_signal_connect(box, "drag-end", G_CALLBACK(on_end), NULL);
    g_timeout_add_seconds(40, timeout_quit, NULL);
    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
