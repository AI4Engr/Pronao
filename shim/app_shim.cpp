#include "app_shim.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifndef APP_HEADLESS
#include "webview/webview.h"
#endif

namespace {

struct Call {
    std::string id;
    std::string request;
};

struct Reply {
    std::string id;
    int status;
    std::string json;
};

struct App {
    std::mutex m;
    std::condition_variable cv;        // signalled on: a call queued, the window closed, creation finished
    std::deque<Call> calls;
    bool ready = false;                // creation finished (successfully or not)
    bool created = false;
    bool closed = false;               // the message loop has ended
    std::thread ui;
#ifndef APP_HEADLESS
    webview::webview* w = nullptr;     // valid only while !closed && created, and only touched under `m`
#else
    std::deque<Reply> replies;         // what CeeLing answered, for the tests to inspect
#endif
};

// A timed wait on `cv`. Under ThreadSanitizer only, the deadline is a system_clock one: GCC 11's TSan does not
// intercept pthread_cond_clockwait (what a steady_clock wait compiles to), so it misses the unlock/relock inside
// the wait and reports a "double lock" and races that are not there. Normal builds use wait_for unchanged.
template <typename Pred>
bool timedWait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
               std::chrono::milliseconds timeout, Pred pred) {
#if defined(__SANITIZE_THREAD__)
    return cv.wait_until(lock, std::chrono::system_clock::now() + timeout, pred);
#else
    return cv.wait_for(lock, timeout, pred);
#endif
}

std::mutex g_tableMutex;
std::vector<std::shared_ptr<App>> g_table;   // handle - 1 = index; a closed slot is reset, not reused

std::shared_ptr<App> lookup(int handle) {
    std::lock_guard<std::mutex> lock(g_tableMutex);
    if (handle < 1 || static_cast<size_t>(handle) > g_table.size()) return nullptr;
    return g_table[static_cast<size_t>(handle) - 1];
}

}  // namespace

int app_open(std::string title, int width, int height, std::string html, std::string init_js, int debug) {
    auto app = std::make_shared<App>();
    int handle = 0;
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);
        g_table.push_back(app);
        handle = static_cast<int>(g_table.size());
    }
#ifdef APP_HEADLESS
    (void)title; (void)width; (void)height; (void)html; (void)init_js; (void)debug;
    {
        std::lock_guard<std::mutex> lock(app->m);
        app->created = true;
        app->ready = true;
    }
    return handle;
#else
    App* raw = app.get();
    app->ui = std::thread([raw, title, width, height, html, init_js, debug] {
        try {
            webview::webview w(debug != 0, nullptr);
            w.set_title(title);
            w.set_size(width, height, WEBVIEW_HINT_NONE);
            w.bind("__cling_invoke",
                   [raw](const std::string& id, const std::string& req, void* /*unused*/) {
                       {
                           std::lock_guard<std::mutex> lock(raw->m);
                           raw->calls.push_back(Call{id, req});
                       }
                       raw->cv.notify_all();
                   },
                   nullptr);
            if (!init_js.empty()) w.init(init_js);
            w.set_html(html);
            {
                std::lock_guard<std::mutex> lock(raw->m);
                raw->w = &w;
                raw->created = true;
                raw->ready = true;
            }
            raw->cv.notify_all();
            w.run();
            {
                // Forget the pointer BEFORE `w` is destroyed at the end of this scope.
                std::lock_guard<std::mutex> lock(raw->m);
                raw->w = nullptr;
                raw->closed = true;
            }
            raw->cv.notify_all();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "app_shim: window creation failed: %s\n", e.what());
            {
                std::lock_guard<std::mutex> lock(raw->m);
                raw->w = nullptr;
                raw->closed = true;
                raw->ready = true;
            }
            raw->cv.notify_all();
        } catch (...) {
            std::fprintf(stderr, "app_shim: window creation failed (unknown error)\n");
            {
                std::lock_guard<std::mutex> lock(raw->m);
                raw->w = nullptr;
                raw->closed = true;
                raw->ready = true;
            }
            raw->cv.notify_all();
        }
    });
    std::unique_lock<std::mutex> lock(app->m);
    app->cv.wait(lock, [&] { return app->ready; });
    if (!app->created) {
        lock.unlock();
        app->ui.join();
        std::lock_guard<std::mutex> t(g_tableMutex);
        g_table[static_cast<size_t>(handle) - 1].reset();
        return 0;
    }
    return handle;
#endif
}

int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::unique_lock<std::mutex> lock(app->m);
    auto ready = [&] { return !app->calls.empty() || app->closed; };
    if (timeout_ms < 0) {
        app->cv.wait(lock, ready);
    } else if (!timedWait(app->cv, lock, std::chrono::milliseconds(timeout_ms), ready)) {
        return 2;
    }
    if (app->calls.empty()) return 0;   // closed and drained
    Call c = std::move(app->calls.front());
    app->calls.pop_front();
    id = std::move(c.id);
    request = std::move(c.request);
    return 1;
}

int app_return(int handle, std::string id, int status, std::string json) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::lock_guard<std::mutex> lock(app->m);
#ifdef APP_HEADLESS
    app->replies.push_back(Reply{std::move(id), status, std::move(json)});
    app->cv.notify_all();
    return 1;
#else
    if (app->w == nullptr) return 0;   // the window is gone; the answer has nowhere to go
    // resolve() posts to the UI thread itself, so calling it from this thread is safe.
    return app->w->resolve(id, status, json).ok() ? 1 : 0;
#endif
}

int app_eval(int handle, std::string js) {
    auto app = lookup(handle);
    if (!app) return 0;
#ifdef APP_HEADLESS
    (void)js;
    return 1;
#else
    std::lock_guard<std::mutex> lock(app->m);
    if (app->w == nullptr) return 0;
    webview::webview* w = app->w;
    return w->dispatch([w, js] { w->eval(js); }).ok() ? 1 : 0;
#endif
}

int app_terminate(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
#ifdef APP_HEADLESS
    {
        std::lock_guard<std::mutex> lock(app->m);
        app->closed = true;
    }
    app->cv.notify_all();
    return 1;
#else
    std::lock_guard<std::mutex> lock(app->m);
    if (app->w == nullptr) return 0;
    // NOT w->terminate() directly: on Windows that is PostQuitMessage(0), which posts WM_QUIT to the CALLING
    // thread's queue -- from here, the wrong thread, so the UI loop would never stop (the library documents it as
    // thread-safe; that holds on Linux, where it dispatches internally). Run it on the UI thread.
    webview::webview* w = app->w;
    return w->dispatch([w] { w->terminate(); }).ok() ? 1 : 0;
#endif
}

int app_close(int handle) {
    auto app = lookup(handle);
    if (!app) return 0;
    app_terminate(handle);
    if (app->ui.joinable()) app->ui.join();
    std::lock_guard<std::mutex> lock(g_tableMutex);
    g_table[static_cast<size_t>(handle) - 1].reset();
    return 1;
}

#ifdef APP_HEADLESS
int app_test_inject(int handle, std::string id, std::string request) {
    auto app = lookup(handle);
    if (!app) return 0;
    {
        std::lock_guard<std::mutex> lock(app->m);
        app->calls.push_back(Call{std::move(id), std::move(request)});
    }
    app->cv.notify_all();
    return 1;
}

int app_test_take_response(int handle, std::string& id, int& status, std::string& json) {
    auto app = lookup(handle);
    if (!app) return 0;
    std::unique_lock<std::mutex> lock(app->m);
    if (!timedWait(app->cv, lock, std::chrono::seconds(10), [&] { return !app->replies.empty(); })) return 2;
    Reply r = std::move(app->replies.front());
    app->replies.pop_front();
    id = std::move(r.id);
    status = r.status;
    json = std::move(r.json);
    return 1;
}

int app_test_close_window(int handle) {
    return app_terminate(handle);
}
#endif

int app_sleep_ms(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 1;
}

namespace {
std::atomic<int> g_gaugeNow{0};
std::atomic<int> g_gaugePeak{0};
}  // namespace

int app_gauge_enter() {
    const int now = g_gaugeNow.fetch_add(1) + 1;
    int peak = g_gaugePeak.load();
    while (now > peak && !g_gaugePeak.compare_exchange_weak(peak, now)) {}
    return now;
}

int app_gauge_leave() {
    return g_gaugeNow.fetch_sub(1) - 1;
}

int app_gauge_peak() {
    return g_gaugePeak.load();
}
