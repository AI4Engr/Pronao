// The HTTP primitive for the headless tests: no network. A test scripts the answers by address (app_test_http_reply) and reads back what was asked
// (app_test_http_request). An address that was not scripted answers 2 (could not connect), as an unreachable host would.
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../shim/app_native.h"
#include "../../shim/cancel.h"
#include "test_support.h"

namespace {
struct Reply { int code; int status; std::string headers, body; int flags; int delayMs; };
struct Asked { std::string method, url, headers, body; int timeoutMs; int maxBytes; };
std::mutex g_mutex;
std::map<std::string, std::deque<Reply>> g_replies;
std::vector<Asked> g_asked;
}  // namespace

int app_http_request(int handle, std::string call, std::string method, std::string url, std::vector<std::string> headers, std::string body, int timeout_ms, int max_bytes,
                     int& status_code, std::string& resp_headers, std::string& resp_body, int& flags) {
    Reply r{2, 0, "", "", 0, 0};
    std::string joined;
    for (const std::string& h : headers) { joined += h; joined += '\n'; }
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_asked.push_back(Asked{method, url, joined, body, timeout_ms, max_bytes});
        auto it = g_replies.find(url);
        if (it != g_replies.end() && !it->second.empty()) {
            r = it->second.front();
            if (it->second.size() > 1) it->second.pop_front();       // the last scripted answer stays for further calls
        }
    }
    // a scripted delay waits in slices, as the real layer does, and notices a cancel
    int waited = 0;
    while (waited < r.delayMs) {
        if (cancelRegistry().cancelled(handle, call)) { status_code = 0; resp_headers.clear(); resp_body.clear(); flags = 8; return 0; }
        const int step = r.delayMs - waited < 20 ? r.delayMs - waited : 20;
        app_sleep_ms(step);
        waited += step;
    }
    status_code = r.status;
    resp_headers = r.headers;
    resp_body = r.body;
    flags = r.flags;
    if (max_bytes >= 0 && resp_body.size() > static_cast<size_t>(max_bytes)) { resp_body.resize(static_cast<size_t>(max_bytes)); flags |= 2; }
    return r.code;
}

int app_test_http_reset() {
    std::lock_guard<std::mutex> l(g_mutex);
    g_replies.clear();
    g_asked.clear();
    return 1;
}

// queues an answer for `url`: the code app_http_request returns, then (for code 0) the status, the header lines, the body, the flags and a delay in milliseconds
int app_test_http_reply(std::string url, int code, int status, std::string headers, std::string body, int flags, int delayMs) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto& q = g_replies[url];
    q.push_back(Reply{code, status, headers, body, flags, delayMs});
    return 1;
}

int app_test_http_count() {
    std::lock_guard<std::mutex> l(g_mutex);
    return static_cast<int>(g_asked.size());
}

// the index-th request that was made; 0 if there is none. `headers` are the lines that would be sent, each ended by \n
int app_test_http_request(int index, std::string& method, std::string& url, std::string& headers, std::string& body, int& timeoutMs, int& maxBytes) {
    std::lock_guard<std::mutex> l(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_asked.size())) return 0;
    const Asked& a = g_asked[static_cast<size_t>(index)];
    method = a.method; url = a.url; headers = a.headers; body = a.body; timeoutMs = a.timeoutMs; maxBytes = a.maxBytes;
    return 1;
}
