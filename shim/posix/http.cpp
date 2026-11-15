// POSIX: one HTTP request over libsoup 3 (see app_native.h), the library WebKitGTK itself uses, so it is on every machine that can show a window. The shim sends the request
// and returns what came back. Whether the address may be reached, which headers go out and what happens on a redirect is decided in src/http.clg; this file follows no
// redirect, keeps no cookie (a SoupSession has no cookie jar unless one is added) and sends no credentials of its own accord (a 401 is just an answer). The system's proxy
// settings and certificate store are GLib's (glib-networking).
//
// The request runs synchronously on the calling worker, with a GCancellable that a small watcher thread cancels when the time limit passes or the call is abandoned or the window
// closes (the watcher polls every 50 ms, as the Windows file does). Without libsoup 3 (an old system with only libsoup 2.4) the primitive answers "not implemented".
#include <string>
#include <vector>

#include "../app_native.h"
#include "../cancel.h"

#if defined(__has_include)
#if __has_include(<libsoup/soup.h>)
#include <libsoup/soup.h>
#endif
#endif

#if defined(SOUP_MAJOR_VERSION) && SOUP_MAJOR_VERSION >= 3

#include <atomic>
#include <chrono>
#include <thread>

namespace {

int codeForError(const GError* e) {
    if (!e) return 6;
    if (e->domain == G_TLS_ERROR) return 3;
    if (e->domain == G_RESOLVER_ERROR) return 2;
    if (e->domain == G_IO_ERROR) {
        switch (e->code) {
            case G_IO_ERROR_CONNECTION_REFUSED:
            case G_IO_ERROR_HOST_NOT_FOUND:
            case G_IO_ERROR_HOST_UNREACHABLE:
            case G_IO_ERROR_NETWORK_UNREACHABLE:
            case G_IO_ERROR_TIMED_OUT:
            case G_IO_ERROR_PROXY_FAILED:
            case G_IO_ERROR_PROXY_NEED_AUTH:
            case G_IO_ERROR_PROXY_NOT_ALLOWED:
            case G_IO_ERROR_NOT_CONNECTED:
            case G_IO_ERROR_BROKEN_PIPE:
                return 2;
            default:
                break;
        }
    }
    return 6;
}

}  // namespace

int app_http_request(int handle, std::string call, std::string method, std::string url, std::vector<std::string> headers, std::string body,
                     int timeout_ms, int max_bytes, int& status_code, std::string& resp_headers, std::string& resp_body, int& flags) {
    status_code = 0;
    resp_headers.clear();
    resp_body.clear();
    flags = 0;
    if (method.empty() || method.find_first_of(std::string(" \r\n\0\t", 5)) != std::string::npos) return 4;
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) return 4;
    for (const std::string& line : headers)
        if (line.find_first_of(std::string("\r\n\0", 3)) != std::string::npos || line.find(':') == std::string::npos || line[0] == ':') return 4;
    GError* uriError = nullptr;
    GUri* uri = g_uri_parse(url.c_str(), static_cast<GUriFlags>(G_URI_FLAGS_PARSE_RELAXED), &uriError);
    if (!uri) { if (uriError) g_error_free(uriError); return 4; }
    SoupMessage* message = soup_message_new_from_uri(method.c_str(), uri);
    g_uri_unref(uri);
    if (!message) return 4;
    SoupMessageHeaders* request = soup_message_get_request_headers(message);
    for (const std::string& line : headers) {
        const size_t colon = line.find(':');
        size_t from = colon + 1;
        while (from < line.size() && (line[from] == ' ' || line[from] == '\t')) ++from;
        soup_message_headers_replace(request, line.substr(0, colon).c_str(), line.substr(from).c_str());
    }
    if (!body.empty()) {
        GBytes* bytes = g_bytes_new(body.data(), body.size());
        soup_message_set_request_body_from_bytes(message, nullptr, bytes);
        g_bytes_unref(bytes);
    }
    soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);                  // redirects are followed in CeeLing, hop by hop

    SoupSession* session = soup_session_new_with_options("user-agent", "Pronao/1", "timeout", 0u, "idle-timeout", 0u, nullptr);
    GCancellable* cancellable = g_cancellable_new();
    std::atomic<bool> done{false};
    std::atomic<int> why{0};                                                    // 1 time limit, 8 cancelled
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 1 ? 1 : timeout_ms);
    std::thread watcher([&] {
        while (!done) {
            if (cancelRegistry().cancelled(handle, call)) { why = 8; g_cancellable_cancel(cancellable); return; }
            if (std::chrono::steady_clock::now() >= deadline) { why = 1; g_cancellable_cancel(cancellable); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    int result = 0;
    GError* error = nullptr;
    GInputStream* stream = soup_session_send(session, message, cancellable, &error);
    if (stream) {
        status_code = static_cast<int>(soup_message_get_status(message));
        SoupMessageHeaders* response = soup_message_get_response_headers(message);
        SoupMessageHeadersIter iter;
        const char *name, *value;
        soup_message_headers_iter_init(&iter, response);
        while (soup_message_headers_iter_next(&iter, &name, &value)) { resp_headers += name; resp_headers += ": "; resp_headers += value; resp_headers += '\n'; }
        const size_t limit = static_cast<size_t>(max_bytes < 0 ? 0 : max_bytes);
        std::vector<char> chunk(64 * 1024);
        for (;;) {
            const gssize got = g_input_stream_read(stream, chunk.data(), chunk.size(), cancellable, &error);
            if (got < 0) break;
            if (got == 0) break;
            resp_body.append(chunk.data(), static_cast<size_t>(got));
            if (resp_body.size() > limit) { resp_body.resize(limit); flags |= 2; break; }
        }
        g_input_stream_close(stream, nullptr, nullptr);
        g_object_unref(stream);
    }
    done = true;
    watcher.join();
    if (why == 1) { flags |= 1; result = 0; }
    else if (why == 8) { flags |= 8; result = 0; }
    else if (!stream || error) result = codeForError(error);
    if (error) g_error_free(error);
    g_object_unref(cancellable);
    g_object_unref(message);
    g_object_unref(session);
    return result;
}

#else

int app_http_request(int, std::string, std::string, std::string, std::vector<std::string>, std::string, int, int, int& status_code, std::string& resp_headers, std::string& resp_body,
                     int& flags) {
    status_code = 0;
    resp_headers.clear();
    resp_body.clear();
    flags = 0;
    return 5;                                                                   // (built without libsoup 3: libsoup3-dev, or the dev package of WebKitGTK 4.1, was not there)
}

#endif
