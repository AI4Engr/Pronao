// Linux: answers the window's requests for the application's own files from memory (see shim/assets.h), through a custom URI scheme of WebKitGTK.
// WebKitGTK cannot intercept an http address, so the program's site is shown under "pronao://<host>/" where the table of files (and CeeLing) says
// "http://<host>/": app_web_url turns the one into the other, and a request for pronao:// is answered by the same AssetStore that the Windows build asks.
// What a page sees differs from the Windows build in one way: its origin is pronao://<host>, not http://<host>. Relative addresses, the Content-Security-Policy
// ('self' is the same origin) and fetch of the program's own files work as they do there; an absolute http://<host>/ address written in a page does not.
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <webkit2/webkit2.h>

#include <memory>
#include <set>
#include <string>

#include "../app_native.h"
#include "../app_shim_internal.h"
#include "../assets.h"

std::string app_web_url(const std::string& url) {
    const std::string origin = assetStore()->origin();
    const size_t colon = origin.find("://");
    if (origin.empty() || colon == std::string::npos || url.compare(0, origin.size(), origin) != 0) return url;
    return "pronao://" + url.substr(colon + 3);
}

namespace {

const char* reasonFor(int status) { return status == 200 ? "OK" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 500 ? "Internal Server Error" : "Error"; }

void freeBody(gpointer p) { delete static_cast<std::shared_ptr<const std::string>*>(p); }

void onSchemeRequest(WebKitURISchemeRequest* request, gpointer) {
    const std::shared_ptr<AssetStore> store = assetStore();
    const char* rawUri = webkit_uri_scheme_request_get_uri(request);
    const char* rawMethod = webkit_uri_scheme_request_get_http_method(request);
    const std::string uri = rawUri ? rawUri : "";
    const std::string method = rawMethod ? rawMethod : "GET";
    // pronao://host/path -> the address the table knows it by
    std::string mapped = uri;
    const std::string origin = store->origin();
    const size_t colon = origin.find("://");
    if (colon != std::string::npos && uri.compare(0, 9, "pronao://") == 0) mapped = origin.substr(0, colon) + "://" + uri.substr(9);

    int status = 404;
    std::string headers;
    std::shared_ptr<const std::string> body;
    if (!store->respond(method, mapped, status, headers, body)) {           // not under the program's origin: there is no network behind this scheme
        status = 404;
        headers = "Content-Type: text/plain\r\n";
        body = std::make_shared<const std::string>("not found");
    }
    GBytes* bytes = g_bytes_new_with_free_func(body->data(), body->size(), freeBody, new std::shared_ptr<const std::string>(body));
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    g_bytes_unref(bytes);
    WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(stream, static_cast<gint64>(body->size()));
    g_object_unref(stream);
    webkit_uri_scheme_response_set_status(response, static_cast<guint>(status), reasonFor(status));
    SoupMessageHeaders* soup = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    size_t at = 0;
    while (at < headers.size()) {
        size_t end = headers.find("\r\n", at);
        if (end == std::string::npos) end = headers.size();
        const std::string line = headers.substr(at, end - at);
        at = end + 2;
        const size_t split = line.find(':');
        if (split == std::string::npos) continue;
        const std::string name = line.substr(0, split);
        size_t from = split + 1;
        while (from < line.size() && line[from] == ' ') ++from;
        const std::string value = line.substr(from);
        if (name == "Content-Type") webkit_uri_scheme_response_set_content_type(response, value.c_str());
        if (name != "Content-Length") soup_message_headers_append(soup, name.c_str(), value.c_str());     // (the length is the stream's; the type goes in both places: with nosniff the engine trusts only the header)
    }
    webkit_uri_scheme_response_set_http_headers(response, soup);           // takes the headers
    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
}

}  // namespace

void app_assets_install(void* controller, std::shared_ptr<AssetStore>) {
    static std::set<WebKitWebContext*> registered;                          // the windows share one context: the scheme is registered once
    WebKitWebContext* context = webkit_web_view_get_context(WEBKIT_WEB_VIEW(controller));
    // One reference to the context that is never given back. WebKitGTK 2.50 aborts when the last reference to its context is dropped by the C library's exit handlers
    // (found with AddressSanitizer: exit -> g_object_unref -> libwebkit2gtk -> abort, after the program had finished and printed its result, so every run ended with
    // SIGABRT). The process is ending anyway; with the reference held that finalizer never runs.
    if (registered.empty()) g_object_ref(context);
    if (!registered.insert(context).second) return;
    webkit_web_context_register_uri_scheme(context, "pronao", onSchemeRequest, nullptr, nullptr);
    WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, "pronao");           // so storage, fetch and the like work as on an https page
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "pronao");
}
