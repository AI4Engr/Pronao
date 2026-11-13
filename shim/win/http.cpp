// Windows: one HTTP request over WinHTTP (see app_native.h). The shim sends the request and returns what came back. Whether the address may be
// reached, which headers go out and what happens on a redirect is decided in src/http.clg; this file follows no redirect, keeps no cookie and sends no
// credentials of its own accord.
//
// WinHTTP runs here in its asynchronous mode only so that the request can be aborted while it waits in the network: the caller waits in 50 ms slices,
// and a cancel (the call was abandoned, the window is closing) or the time limit closes the request handle.
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../app_native.h"
#include "../cancel.h"
#include "util.h"

#include <winhttp.h>

namespace {

struct State {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);       // signalled when a step has finished
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);       // signalled when the request handle is gone
    std::atomic<bool> failed{false};
    std::atomic<DWORD> error{0};
    std::atomic<DWORD> amount{0};                                        // bytes available, or bytes read
    ~State() { CloseHandle(event); CloseHandle(closed); }
};

void CALLBACK onStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    State* s = reinterpret_cast<State*>(context);
    if (s == nullptr) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const WINHTTP_ASYNC_RESULT* r = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        s->error = r != nullptr ? r->dwError : ERROR_WINHTTP_INTERNAL_ERROR;
        s->failed = true;
        SetEvent(s->event);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        SetEvent(s->event);
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        s->amount = info != nullptr ? *static_cast<const DWORD*>(info) : 0;
        SetEvent(s->event);
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        s->amount = length;
        SetEvent(s->event);
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(s->closed);
        break;
    default:
        break;
    }
}

enum class Wait { Done, Failed, TimedOut, Cancelled };

Wait waitStep(State& s, int handle, const std::string& call, ULONGLONG deadline) {
    for (;;) {
        const DWORD r = WaitForSingleObject(s.event, 50);
        if (r == WAIT_OBJECT_0) return s.failed ? Wait::Failed : Wait::Done;
        if (cancelRegistry().cancelled(handle, call)) return Wait::Cancelled;
        if (GetTickCount64() >= deadline) return Wait::TimedOut;
    }
}

int codeForError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        return 3;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_TIMEOUT:
        return 2;
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
        return 4;
    default:
        return 6;
    }
}

struct Url {
    bool secure = true;
    std::wstring host, path;
    INTERNET_PORT port = 443;
};

bool splitUrl(const std::string& url, Url& out) {
    const std::wstring w = widen(url);
    if (w.empty()) return false;
    URL_COMPONENTSW c = {};
    c.dwStructSize = sizeof c;
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &c)) return false;
    if (c.nScheme != INTERNET_SCHEME_HTTPS && c.nScheme != INTERNET_SCHEME_HTTP) return false;
    out.secure = c.nScheme == INTERNET_SCHEME_HTTPS;
    out.host.assign(c.lpszHostName, c.dwHostNameLength);
    out.path.assign(c.lpszUrlPath, c.dwUrlPathLength);
    out.path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
    out.port = c.nPort;
    return !out.host.empty();
}

// "HTTP/1.1 200 OK\r\nName: value\r\n..." -> "Name: value\n..."
std::string headerLines(const std::wstring& raw) {
    const std::string utf8 = narrow(raw);
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i < utf8.size()) {
        size_t e = utf8.find("\r\n", i);
        if (e == std::string::npos) e = utf8.size();
        if (!first && e > i) { out.append(utf8, i, e - i); out.push_back('\n'); }
        first = false;
        i = e + 2;
    }
    return out;
}

}  // namespace

int app_http_request(int handle, std::string call, std::string method, std::string url, std::vector<std::string> headers, std::string body,
                     int timeout_ms, int max_bytes, int& status_code, std::string& resp_headers, std::string& resp_body, int& flags) {
    status_code = 0;
    resp_headers.clear();
    resp_body.clear();
    flags = 0;
    Url u;
    if (!splitUrl(url, u)) return 4;
    const std::wstring wmethod = widen(method);
    if (wmethod.empty()) return 4;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(1, timeout_ms));

    HINTERNET session = WinHttpOpen(L"Pronao/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (session == nullptr) session = WinHttpOpen(L"Pronao/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (session == nullptr) return 6;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x2000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    auto state = std::make_shared<State>();
    State* const ctx = state.get();
    WinHttpSetStatusCallback(session, onStatus, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES, 0);

    HINTERNET connection = WinHttpConnect(session, u.host.c_str(), u.port, 0);
    if (connection == nullptr) { WinHttpCloseHandle(session); return 2; }
    HINTERNET request = WinHttpOpenRequest(connection, wmethod.c_str(), u.path.empty() ? L"/" : u.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) { WinHttpCloseHandle(connection); WinHttpCloseHandle(session); return 6; }
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(ctx);
    WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context);

    int result = 6;
    bool requestOpen = true;
    const auto closeRequest = [&] {
        if (!requestOpen) return;
        requestOpen = false;
        WinHttpCloseHandle(request);
        WaitForSingleObject(state->closed, 2000);       // the callback has seen its last event; ctx may go
    };
    do {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);       // redirects are followed in CeeLing, hop by hop
        DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;                                    // never send the user's Windows credentials unasked
        WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
        DWORD features = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features);
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);      // (a failure just leaves the body as it came)

        bool headersOk = true;
        for (const std::string& line : headers) {
            // one header per element: WinHTTP would split a line with a line break into two headers
            if (line.find_first_of(std::string("\r\n\0", 3)) != std::string::npos || line.find(':') == std::string::npos) { headersOk = false; continue; }
            const std::wstring w = widen(line);
            if (w.empty() || !WinHttpAddRequestHeaders(request, w.c_str(), static_cast<DWORD>(w.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) headersOk = false;
        }
        if (!headersOk) { result = 4; break; }

        const DWORD length = static_cast<DWORD>(body.size());
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, length > 0 ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA, length, length, reinterpret_cast<DWORD_PTR>(ctx))) {
            result = codeForError(GetLastError());
            break;
        }
        Wait w = waitStep(*state, handle, call, deadline);
        if (w == Wait::Cancelled) { flags |= 8; result = 0; break; }
        if (w == Wait::TimedOut) { flags |= 1; result = 0; break; }
        if (w == Wait::Failed) { result = codeForError(state->error); break; }

        if (!WinHttpReceiveResponse(request, nullptr)) { result = codeForError(GetLastError()); break; }
        w = waitStep(*state, handle, call, deadline);
        if (w == Wait::Cancelled) { flags |= 8; result = 0; break; }
        if (w == Wait::TimedOut) { flags |= 1; result = 0; break; }
        if (w == Wait::Failed) { result = codeForError(state->error); break; }

        DWORD code = 0, size = sizeof code;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) { result = 6; break; }
        status_code = static_cast<int>(code);
        DWORD rawSize = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &rawSize, WINHTTP_NO_HEADER_INDEX);
        if (rawSize > 0 && rawSize < 1024 * 1024) {
            std::wstring raw(rawSize / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &rawSize, WINHTTP_NO_HEADER_INDEX)) {
                raw.resize(rawSize / sizeof(wchar_t));
                resp_headers = headerLines(raw);
            }
        }

        result = 0;
        std::vector<char> chunk(64 * 1024);
        const size_t limit = static_cast<size_t>(std::max(0, max_bytes));
        bool stop = false;
        while (!stop) {
            if (!WinHttpQueryDataAvailable(request, nullptr)) { result = codeForError(GetLastError()); break; }
            w = waitStep(*state, handle, call, deadline);
            if (w == Wait::Cancelled) { flags |= 8; break; }
            if (w == Wait::TimedOut) { flags |= 1; break; }
            if (w == Wait::Failed) { result = codeForError(state->error); break; }
            const DWORD available = state->amount;
            if (available == 0) break;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(std::min<size_t>(available, chunk.size()), limit + 1 - std::min(limit + 1, resp_body.size())));
            if (want == 0) { flags |= 2; break; }
            if (!WinHttpReadData(request, chunk.data(), want, nullptr)) { result = codeForError(GetLastError()); break; }
            w = waitStep(*state, handle, call, deadline);
            if (w == Wait::Cancelled) { flags |= 8; break; }
            if (w == Wait::TimedOut) { flags |= 1; break; }
            if (w == Wait::Failed) { result = codeForError(state->error); break; }
            const DWORD got = state->amount;
            if (got == 0) break;
            resp_body.append(chunk.data(), got);
            if (resp_body.size() > limit) { resp_body.resize(limit); flags |= 2; stop = true; }
        }
    } while (false);
    closeRequest();
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}
