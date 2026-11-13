// The real HTTP primitive (shim/win/http.cpp, over WinHTTP) against a small server on the loopback interface that this test starts. Windows only: elsewhere the
// primitive says "not implemented" and the test checks just that. It covers what the native layer is trusted to do: send the request as given, return the
// status, headers and body as they came, follow no redirect, keep no cookie, cut at the byte limit, stop at the time limit and on a cancel, and say what
// kind of failure it was. TLS is not tested here (it needs a certificate); see the manual for how that was checked by hand.
#include <cstdio>
#include <string>
#include <vector>

#include "../shim/app_native.h"
#include "../shim/cancel.h"

static int g_failed = 0, g_passed = 0;
static void check(const char* name, bool ok) { if (ok) { ++g_passed; std::printf("PASS %s\n", name); } else { ++g_failed; std::printf("FAIL %s\n", name); } }

#ifndef _WIN32
int main() {
    int status = 0, flags = 0;
    std::string headers, body;
    check("another platform answers 'not implemented' and sends nothing",
          app_http_request(1, "c", "GET", "http://127.0.0.1:1/", {}, "", 1000, 100, status, headers, body, flags) == 5);
    std::printf("http shim test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#include <zlib.h>

#include <atomic>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_requests{0};
std::string g_lastRequest;                      // the head and body of the latest request, for the test to look at
std::atomic<bool> g_sawCookie{false};

std::string gzipOf(const std::string& in) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(deflateBound(&z, static_cast<uLong>(in.size())) + 32, '\0');
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data())); z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(&out[0]); z.avail_out = static_cast<uInt>(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

void sendAll(SOCKET c, const std::string& s) {
    size_t at = 0;
    while (at < s.size()) {
        const int n = send(c, s.data() + at, static_cast<int>(s.size() - at), 0);
        if (n <= 0) return;
        at += static_cast<size_t>(n);
    }
}

std::string response(int status, const std::string& extraHeaders, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " X\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
}

void serveOne(SOCKET c) {
    std::string in;
    char buf[4096];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        const int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) { closesocket(c); return; }
        in.append(buf, static_cast<size_t>(n));
        headEnd = in.find("\r\n\r\n");
    }
    size_t length = 0;
    const size_t cl = in.find("Content-Length: ");
    if (cl != std::string::npos && cl < headEnd) length = static_cast<size_t>(std::atoi(in.c_str() + cl + 16));
    while (in.size() < headEnd + 4 + length) {
        const int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        in.append(buf, static_cast<size_t>(n));
    }
    ++g_requests;
    g_lastRequest = in;
    if (in.find("Cookie:") != std::string::npos) g_sawCookie = true;
    const std::string line = in.substr(0, in.find("\r\n"));
    const std::string path = line.substr(line.find(' ') + 1, line.rfind(' ') - line.find(' ') - 1);
    if (path == "/ok") sendAll(c, response(200, "X-Test: one\r\nSet-Cookie: s=1\r\nContent-Type: text/plain\r\n", "Hello"));
    else if (path == "/echo") sendAll(c, response(200, "", in));
    else if (path == "/redir") sendAll(c, response(302, "Location: /ok\r\n", "moved"));
    else if (path == "/big") sendAll(c, response(200, "", std::string(100000, 'x')));
    else if (path == "/gz") { const std::string z = gzipOf("gzipped text, long enough to be worth it: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); sendAll(c, response(200, "Content-Encoding: gzip\r\n", z)); }
    else if (path == "/204") sendAll(c, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    else if (path == "/slow") { for (int i = 0; i < 80 && !g_stop; ++i) Sleep(50); sendAll(c, response(200, "", "late")); }
    else if (path == "/auth") sendAll(c, response(401, "WWW-Authenticate: NTLM\r\nWWW-Authenticate: Negotiate\r\n", "no"));
    else sendAll(c, response(404, "", "not here"));
    shutdown(c, SD_SEND);
    closesocket(c);
}

}  // namespace

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    listen(listener, 16);
    int len = sizeof addr;
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    std::thread server([&] {
        std::vector<std::thread> handlers;
        while (!g_stop) {
            fd_set fds; FD_ZERO(&fds); FD_SET(listener, &fds);
            timeval tv{0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
                SOCKET c = accept(listener, nullptr, nullptr);
                if (c != INVALID_SOCKET) handlers.emplace_back([c] { serveOne(c); });
            }
        }
        for (auto& h : handlers) h.join();
    });

    int status = 0, flags = 0;
    std::string headers, body;
    const auto get = [&](const std::string& path, int timeout = 5000, int max = 1 << 20) {
        return app_http_request(1, "call", "GET", base + path, {}, "", timeout, max, status, headers, body, flags);
    };

    check("a request is answered", get("/ok") == 0 && status == 200 && body == "Hello" && flags == 0);
    check("the headers come back as lines", headers.find("X-Test: one\n") != std::string::npos && headers.find("Content-Type: text/plain\n") != std::string::npos);
    check("the status line is not among them", headers.find("HTTP/") == std::string::npos);
    check("a 404 is an answer", get("/missing") == 0 && status == 404 && body == "not here");
    check("a 204 has no body", get("/204") == 0 && status == 204 && body.empty());

    check("a redirect is not followed", get("/redir") == 0 && status == 302 && body == "moved" && headers.find("Location: /ok\n") != std::string::npos);
    const int beforeRedirect = g_requests;
    get("/redir");
    check("and no second request was made", g_requests == beforeRedirect + 1);

    g_sawCookie = false;
    get("/ok");
    get("/ok");
    check("a cookie that was set is not sent back", !g_sawCookie);

    check("a gzip response is delivered decompressed", get("/gz") == 0 && body.rfind("gzipped text", 0) == 0);

    check("the method, the headers and the body go out as given",
          app_http_request(1, "call", "POST", base + "/echo", {"X-One: 1", "Content-Type: text/plain"}, "payload", 5000, 1 << 20, status, headers, body, flags) == 0 &&
          body.rfind("POST /echo HTTP/1.1", 0) == 0 && body.find("X-One: 1\r\n") != std::string::npos && body.find("Content-Type: text/plain\r\n") != std::string::npos &&
          body.size() >= 7 && body.substr(body.size() - 7) == "payload" && body.find("Content-Length: 7") != std::string::npos);
    check("a PUT and a DELETE", app_http_request(1, "call", "PUT", base + "/echo", {}, "x", 5000, 1 << 20, status, headers, body, flags) == 0 && body.rfind("PUT /echo", 0) == 0 &&
                                app_http_request(1, "call", "DELETE", base + "/echo", {}, "", 5000, 1 << 20, status, headers, body, flags) == 0 && body.rfind("DELETE /echo", 0) == 0);
    check("no Authorization is added on its own", app_http_request(1, "call", "GET", base + "/echo", {}, "", 5000, 1 << 20, status, headers, body, flags) == 0 && body.find("Authorization") == std::string::npos);
    const int beforeBad = g_requests;
    check("a header with a line break is refused and nothing is sent", app_http_request(1, "call", "GET", base + "/echo", {"X-A: 1\r\nX-B: 2"}, "", 5000, 1 << 20, status, headers, body, flags) == 4 && g_requests == beforeBad);
    check("so is a header with no colon", app_http_request(1, "call", "GET", base + "/echo", {"NoColonHere"}, "", 5000, 1 << 20, status, headers, body, flags) == 4 && g_requests == beforeBad);
    check("a 401 that offers Windows logon is just an answer, with no credentials sent", get("/auth") == 0 && status == 401);
    check("the user agent is Pronao's", app_http_request(1, "call", "GET", base + "/echo", {}, "", 5000, 1 << 20, status, headers, body, flags) == 0 && body.find("User-Agent: Pronao/1") != std::string::npos);

    check("the body is cut at the limit and says so", get("/big", 5000, 1000) == 0 && status == 200 && body.size() == 1000 && (flags & 2) != 0);
    check("a body exactly at the limit is not cut", get("/ok", 5000, 5) == 0 && body == "Hello" && (flags & 2) == 0);
    check("a larger body arrives whole under a larger limit", get("/big", 5000, 200000) == 0 && body.size() == 100000 && (flags & 2) == 0);

    const ULONGLONG t0 = GetTickCount64();
    const int slow = get("/slow", 600);
    const ULONGLONG took = GetTickCount64() - t0;
    check("the time limit stops a slow server and says so", slow == 0 && (flags & 1) != 0 && took < 2500);

    std::thread canceller([] { Sleep(300); cancelRegistry().cancel(1, "abandoned"); });
    const ULONGLONG t1 = GetTickCount64();
    const int c = app_http_request(1, "abandoned", "GET", base + "/slow", {}, "", 20000, 1 << 20, status, headers, body, flags);
    const ULONGLONG cancelledAfter = GetTickCount64() - t1;
    canceller.join();
    check("a cancel stops the request and says so", c == 0 && (flags & 8) != 0 && cancelledAfter < 2500);
    cancelRegistry().forget(1, "abandoned");
    cancelRegistry().cancelAll(7);
    check("a window that is closing cancels at once", app_http_request(7, "any", "GET", base + "/slow", {}, "", 20000, 1 << 20, status, headers, body, flags) == 0 && (flags & 8) != 0);

    check("nothing listening is 'could not connect'", app_http_request(1, "call", "GET", "http://127.0.0.1:1/", {}, "", 3000, 100, status, headers, body, flags) == 2);
    check("a name that does not resolve is 'could not connect'", app_http_request(1, "call", "GET", "http://does-not-exist.invalid/", {}, "", 8000, 100, status, headers, body, flags) == 2);
    check("an address that is not http or https is refused", app_http_request(1, "call", "GET", "ftp://127.0.0.1/", {}, "", 3000, 100, status, headers, body, flags) == 4 &&
                                                          app_http_request(1, "call", "GET", "not a url", {}, "", 3000, 100, status, headers, body, flags) == 4);
    get("/ok");
    check("the outputs are reset on every call", app_http_request(1, "call", "GET", "http://127.0.0.1:1/", {}, "", 3000, 100, status, headers, body, flags) == 2 && status == 0 && body.empty() && headers.empty() && flags == 0);

    g_stop = true;
    server.join();
    closesocket(listener);
    WSACleanup();
    std::printf("http shim test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
#endif
