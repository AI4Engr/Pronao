// Linux: what the window may navigate to (see NavRules in app_shim_internal.h), in WebKitGTK's terms. Decided here, at the moment the browser engine asks,
// because only the engine knows about frames, redirects and scripts; CeeLing supplies the list of allowed prefixes. The same rule as shim/win/nav.cpp, with one
// difference WebKitGTK forces: a navigation action does not say whether it is for the window or for a frame in it, so the rule for the window's first document
// ("the first document is the application's own") and the rule for frames ("only about:blank, about:srcdoc, or an allowed prefix") are one rule here, and
// a frame is held to the stricter of the two.
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <memory>
#include <string>

#include "../app_native.h"
#include "../app_shim_internal.h"

namespace {

struct Guard {
    std::shared_ptr<NavRules> rules;
};

// The application sees what was refused (App.openLinksInBrowser can act on it): "window" "navigation_blocked" <address> <1 if the user asked for it>.
void blocked(const std::shared_ptr<NavRules>& rules, const std::string& uri, bool byUser) {
    std::string record = "window";
    record += '\0';
    record += "navigation_blocked";
    record += '\0';
    record += uri.substr(0, 2100);
    record += '\0';
    record += byUser ? "1" : "0";
    record += '\0';
    app_push(rules->handle, std::string(), record, 100);
}

gboolean onDecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;      // (a response: not ours to decide)
    Guard* g = static_cast<Guard*>(data);
    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
    const char* raw = request ? webkit_uri_request_get_uri(request) : nullptr;
    const std::string uri = raw ? raw : "";
    const bool byUser = action && webkit_navigation_action_is_user_gesture(action);
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {            // a link with target=_blank, window.open: never a second window of ours
        webkit_policy_decision_ignore(decision);
        blocked(g->rules, uri, byUser);
        return TRUE;
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g->rules->m);
        ok = !g->rules->loaded || uri == g->rules->first || uri == "about:blank" || uri == "about:srcdoc";      // the first document is the application's
        if (!g->rules->loaded) { g->rules->first = uri; g->rules->loaded = true; }
        for (const auto& p : g->rules->prefixes) ok = ok || uri.compare(0, p.size(), p) == 0;
    }
    if (ok) {
        webkit_policy_decision_use(decision);
    } else {
        webkit_policy_decision_ignore(decision);
        blocked(g->rules, uri, byUser);
    }
    return TRUE;
}

// window.open and target=_blank reach "create" as well: refuse, whatever the policy decision said
GtkWidget* onCreate(WebKitWebView*, WebKitNavigationAction* action, gpointer data) {
    Guard* g = static_cast<Guard*>(data);
    WebKitURIRequest* request = action ? webkit_navigation_action_get_request(action) : nullptr;
    const char* raw = request ? webkit_uri_request_get_uri(request) : nullptr;
    blocked(g->rules, raw ? raw : "", action && webkit_navigation_action_is_user_gesture(action));
    return nullptr;
}

gboolean onContextMenu(WebKitWebView*, WebKitContextMenu*, GdkEvent*, WebKitHitTestResult*, gpointer) { return TRUE; }          // no right-click menu (save as, print, view source)

void freeGuard(gpointer p, GClosure*) { delete static_cast<Guard*>(p); }

}  // namespace

void app_nav_install(void* controller, std::shared_ptr<NavRules> rules, int debug) {
    WebKitWebView* view = WEBKIT_WEB_VIEW(controller);
    g_signal_connect_data(view, "decide-policy", G_CALLBACK(onDecidePolicy), new Guard{rules}, freeGuard, static_cast<GConnectFlags>(0));
    g_signal_connect_data(view, "create", G_CALLBACK(onCreate), new Guard{rules}, freeGuard, static_cast<GConnectFlags>(0));
    if (!debug) g_signal_connect(view, "context-menu", G_CALLBACK(onContextMenu), nullptr);
}
