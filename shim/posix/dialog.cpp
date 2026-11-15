// Linux: native dialogs (see app_native.h): GTK's file chooser and message dialog, shown on the window's UI thread, where they run their own loop until the user answers
// (the calling thread waits, as on Windows). What may be shown is decided in src/dialogs.clg. Closing a message box counts as cancel, or as no for yes/no.
#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {

// "*.txt;*.md" -> one GtkFileFilter with a pattern for each
GtkFileFilter* filterFor(const std::string& label, const std::string& patterns) {
    GtkFileFilter* f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, label.c_str());
    size_t at = 0;
    bool any = false;
    while (at <= patterns.size()) {
        size_t end = patterns.find(';', at);
        if (end == std::string::npos) end = patterns.size();
        const std::string p = patterns.substr(at, end - at);
        if (!p.empty()) { gtk_file_filter_add_pattern(f, p.c_str()); any = true; }
        at = end + 1;
    }
    if (!any) gtk_file_filter_add_pattern(f, "*");
    return f;
}

int showFileDialog(GtkWindow* owner, int mode, const std::string& title, const std::string& defaultName, const std::string& defaultExt,
                   const std::vector<std::string>& filters, std::string& pathOut) {
    const GtkFileChooserAction action = mode == 1 ? GTK_FILE_CHOOSER_ACTION_SAVE : mode == 2 ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN;
    GtkWidget* dlg = gtk_file_chooser_dialog_new(title.empty() ? nullptr : title.c_str(), owner, action, "_Cancel", GTK_RESPONSE_CANCEL,
                                                 mode == 1 ? "_Save" : mode == 2 ? "_Select" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    if (!dlg) return -1;
    GtkFileChooser* chooser = GTK_FILE_CHOOSER(dlg);
    gtk_file_chooser_set_local_only(chooser, TRUE);                          // a path the file commands can reach, not a remote location
    if (mode == 1) gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
    if (mode != 2) {
        bool first = true;
        for (size_t i = 0; i + 1 < filters.size(); i += 2) {
            GtkFileFilter* f = filterFor(filters[i], filters[i + 1]);
            gtk_file_chooser_add_filter(chooser, f);
            if (first) { gtk_file_chooser_set_filter(chooser, f); first = false; }
        }
    }
    std::string name = defaultName;
    if (mode == 1 && !defaultExt.empty() && name.find('.') == std::string::npos) name += "." + defaultExt;
    if (mode == 1 && !name.empty()) gtk_file_chooser_set_current_name(chooser, name.c_str());
    const gint response = gtk_dialog_run(GTK_DIALOG(dlg));
    int result = -1;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar* file = gtk_file_chooser_get_filename(chooser);
        if (file) {
            pathOut = file;
            g_free(file);
            result = pathOut.empty() ? -1 : 1;
        }
    } else {
        result = 0;                                                          // cancelled, or closed
    }
    gtk_widget_destroy(dlg);
    return result;
}

}  // namespace

int app_dialog_pick(int handle, int mode, std::string title, std::string default_name, std::string default_ext, std::vector<std::string> filters, std::string& path) {
    auto out = std::make_shared<std::pair<int, std::string>>(-1, std::string());
    if (!app_ui_call(handle, [=](void* native) { out->first = showFileDialog(GTK_WINDOW(native), mode, title, default_name, default_ext, filters, out->second); })) return -1;
    if (out->first == 1) path = out->second;
    return out->first;
}

// 1 OK, 2 Cancel, 3 Yes, 4 No
int app_dialog_message(int handle, std::string title, std::string message, int kind, int buttons) {
    auto out = std::make_shared<int>(-1);
    if (!app_ui_call(handle, [=](void* native) {
            GtkMessageType type = kind == 1 ? GTK_MESSAGE_WARNING : kind == 2 ? GTK_MESSAGE_ERROR : kind == 3 ? GTK_MESSAGE_QUESTION : GTK_MESSAGE_INFO;
            GtkButtonsType set = buttons == 1 ? GTK_BUTTONS_OK_CANCEL : buttons == 2 ? GTK_BUTTONS_YES_NO : GTK_BUTTONS_OK;
            GtkWidget* dlg = gtk_message_dialog_new(GTK_WINDOW(native), GTK_DIALOG_MODAL, type, set, "%s", message.c_str());
            if (!dlg) return;
            gtk_window_set_title(GTK_WINDOW(dlg), title.c_str());
            const gint r = gtk_dialog_run(GTK_DIALOG(dlg));
            gtk_widget_destroy(dlg);
            switch (r) {
                case GTK_RESPONSE_OK: *out = 1; break;
                case GTK_RESPONSE_YES: *out = 3; break;
                case GTK_RESPONSE_NO: *out = 4; break;
                default: *out = buttons == 2 ? 4 : buttons == 1 ? 2 : 1; break;   // the box was closed: no, cancel, or (a box with only OK) ok, as on Windows
            }
        })) return -1;
    return *out;
}
