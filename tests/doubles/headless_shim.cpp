// A stand-in for shim/app_shim.cpp with no window: the same API and the same queue (shim/queue.h), but a "page" that the tests
// drive by hand (app_test_inject) and read back (app_test_take_response / app_test_take_eval). Window control and dialogs are
// recorded or scripted. Not part of the product.
#include "../../shim/app_native.h"
#include "../../shim/app_shim_internal.h"
#include "../../shim/queue.h"
#include "test_support.h"

#include <memory>
#include <vector>

namespace {
struct Reply { std::string id; int status; std::string json; };

struct App {
    CallQueue calls;
    std::mutex m;                       // guards the three below
    std::condition_variable cv;
    std::deque<Reply> replies;          // what CeeLing answered
    std::deque<std::string> evals;      // every script passed to app_eval, in order
    std::deque<std::string> windowLog;  // window control calls, as "title|x", "size|w|h", "state|n", "guard|n"
    bool guard = false;
    int maxRequest = 16 << 20, maxQueued = 1000;
};

std::mutex g_tableMutex;
std::vector<std::shared_ptr<App>> g_table;

std::shared_ptr<App> lookup(int handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    return (handle < 1 || static_cast<size_t>(handle) > g_table.size()) ? nullptr : g_table[static_cast<size_t>(handle) - 1];
}
void logWindow(int handle, const std::string& line) {
    auto app = lookup(handle);
    if (!app) return;
    std::lock_guard<std::mutex> lock(app->m);
    app->windowLog.push_back(line);
}
}  // namespace

int app_open(std::string, int, int, std::string, std::string url, std::string, int) {
    int handle;
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_table.push_back(std::make_shared<App>());
        handle = static_cast<int>(g_table.size());
    }
    if (!url.empty()) logWindow(handle, "open|" + url);
    return handle;
}
int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request) {
    auto app = lookup(handle);
    return app ? app->calls.pop(timeout_ms, id, request) : 0;
}
int app_push(int handle, std::string id, std::string request, int max_queued) {
    auto app = lookup(handle);
    return app ? app->calls.push(std::move(id), std::move(request), max_queued) : -1;
}
int app_return(int handle, std::string id, int status, std::string json) {
    auto app = lookup(handle);
    if (!app || !app->calls.answer(id)) return 0;
    { std::lock_guard<std::mutex> lock(app->m); app->replies.push_back(Reply{std::move(id), status, std::move(json)}); }
    app->cv.notify_all();
    return 1;
}
int app_eval(int handle, std::string js) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    app->evals.push_back(std::move(js));
    return 1;
}
int app_terminate(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    app->calls.close();
    return 1;
}
int app_ui_call(int handle, const std::function<void(void*)>& fn) {
    if (!lookup(handle)) return 0;
    fn(nullptr);
    return 1;
}
int app_pending(int handle, std::string call) {
    auto app = lookup(handle);
    return app && app->calls.waiting(call) ? 1 : 0;
}
int app_window_closed(int handle) {
    auto app = lookup(handle);
    return !app || app->calls.closed() ? 1 : 0;
}
int app_close(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    app_watch_stop_all(handle);
    app->calls.close();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table[static_cast<size_t>(handle) - 1].reset();
    return 1;
}
int app_window_title(int handle, std::string title) { logWindow(handle, "title|" + title); return lookup(handle) ? 1 : 0; }
int app_window_size(int handle, int w, int h) { logWindow(handle, "size|" + std::to_string(w) + "|" + std::to_string(h)); return lookup(handle) ? 1 : 0; }
int app_window_state(int handle, int state) { logWindow(handle, "state|" + std::to_string(state)); return lookup(handle) ? 1 : 0; }
int app_limits(int handle, int max_request, int max_queued) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    app->maxRequest = max_request; app->maxQueued = max_queued;
    return 1;
}
int app_navigation_allow(int handle, std::vector<std::string> prefixes) {
    std::string line = "navigation";
    for (const auto& p : prefixes) line += "|" + p;
    logWindow(handle, line);
    return lookup(handle) ? 1 : 0;
}
int app_window_guard(int handle, int on) {
    auto app = lookup(handle);
    if (!app) return 0;
    { std::lock_guard<std::mutex> lock(app->m); app->guard = on != 0; app->windowLog.push_back("guard|" + std::to_string(on)); }
    return 1;
}

// ---- what the tests add --------------------------------------------------------------------------------------------------
// as the page's bind callback does: refused calls are answered at once with the shim's own texts. Returns CallQueue::admit's code
int app_test_inject(int handle, std::string id, std::string request) {
    auto app = lookup(handle);
    if (!app) return 3;
    int refused;
    { std::lock_guard<std::mutex> lock(app->m); refused = app->calls.admit(id, request, app->maxRequest, app->maxQueued); }
    if (refused == 1 || refused == 2) {      // answered at once, as the page's bind callback does (the call was never pending)
        { std::lock_guard<std::mutex> lock(app->m); app->replies.push_back(Reply{id, 1, refusalJson(refused)}); }
        app->cv.notify_all();
    }
    return refused;
}
int app_test_take_response(int handle, std::string& id, int& status, std::string& json) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::unique_lock<std::mutex> lock(app->m);
#if defined(__SANITIZE_THREAD__)
    if (!app->cv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::seconds(10), [&] { return !app->replies.empty(); })) return 2;
#else
    if (!app->cv.wait_for(lock, std::chrono::seconds(10), [&] { return !app->replies.empty(); })) return 2;
#endif
    Reply r = std::move(app->replies.front());
    app->replies.pop_front();
    id = std::move(r.id);
    status = r.status;
    json = std::move(r.json);
    return 1;
}
int app_test_response_ready(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    return app->replies.empty() ? 0 : 1;
}
int app_test_push_internal(int handle, std::string request) { return app_push(handle, std::string(), std::move(request), 1 << 30); }
int app_test_take_eval(int handle, std::string& js) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    if (app->evals.empty()) return 0;
    js = std::move(app->evals.front());
    app->evals.pop_front();
    return 1;
}
int app_test_take_window_log(int handle, std::string& line) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
    if (app->windowLog.empty()) return 0;
    line = std::move(app->windowLog.front());
    app->windowLog.pop_front();
    return 1;
}
// as if the user pressed the window's X: with the guard on it only asks CeeLing, without it the window closes
int app_test_close_window(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    bool guarded;
    { std::lock_guard<std::mutex> lock(app->m); guarded = app->guard; }
    if (guarded) return app_push(handle, std::string(), std::string("window\0close_requested\0", 23), 100);
    return app_terminate(handle);
}
