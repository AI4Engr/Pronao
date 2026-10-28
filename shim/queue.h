// shim-internal: the queue between the threads that produce calls (UI thread, watchers, workers) and the one that consumes them
// (CeeLing's main loop). Header-only, so the real window and the test double share this very code and the sanitizer runs cover it.
#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <utility>

// What the page's promise is rejected with when `admit` refuses (constant texts: nothing from the request is echoed)
inline const char* refusalJson(int code) {
    return code == 1 ? "{\"code\":\"too_large\",\"message\":\"the request is larger than the application allows\"}"
                     : "{\"code\":\"busy\",\"message\":\"the application has too many requests waiting\"}";
}

class CallQueue {
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::string>> q_;
    bool closed_ = false;
    std::set<std::string> pending_;     // page calls that have not been answered yet
public:
    // 1 queued, 0 if `max_queued` are already waiting
    int push(std::string id, std::string request, int max_queued) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (static_cast<int>(q_.size()) >= max_queued) return 0;
            q_.emplace_back(std::move(id), std::move(request));
        }
        cv_.notify_all();
        return 1;
    }
    // What the page's call gets: 0 queued, 1 refused as too large, 2 refused because the queue is full, 3 refused (a bad id; an
    // empty id marks the shim's own records, which a page must never be able to forge)
    int admit(const std::string& id, const std::string& request, int max_request, int max_queued) {
        if (id.empty() || id.size() > 64) return 3;
        if (static_cast<int>(request.size()) > max_request) return 1;
        std::lock_guard<std::mutex> lock(m_);
        if (pending_.count(id) > 0) return 3;
        if (static_cast<int>(q_.size()) >= max_queued) return 2;
        pending_.insert(id);
        q_.emplace_back(id, request);
        cv_.notify_all();
        return 0;
    }
    // A page call is answered ONCE: true the first time for an id that is waiting for an answer, false for a second answer, an unknown id
    // or one made up by a worker. The window's promise then settles exactly once, whatever the jobs do.
    bool answer(const std::string& id) { std::lock_guard<std::mutex> l(m_); return pending_.erase(id) > 0; }
    bool waiting(const std::string& id) { std::lock_guard<std::mutex> l(m_); return pending_.count(id) > 0; }
    // 1 got one, 0 closed and drained, 2 timed out (timeout_ms < 0 waits forever)
    int pop(int timeout_ms, std::string& id, std::string& request) {
        std::unique_lock<std::mutex> lock(m_);
        auto ready = [&] { return !q_.empty() || closed_; };
        if (timeout_ms < 0) {
            cv_.wait(lock, ready);
        } else {
            // Under ThreadSanitizer only, the deadline is a system_clock one: GCC 11's TSan does not intercept
            // pthread_cond_clockwait (what a steady_clock wait compiles to), so it misses the unlock/relock inside the wait and
            // reports a "double lock" and races that are not there. Normal builds use wait_for unchanged.
#if defined(__SANITIZE_THREAD__)
            if (!cv_.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms), ready)) return 2;
#else
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) return 2;
#endif
        }
        if (q_.empty()) return 0;
        id = std::move(q_.front().first);
        request = std::move(q_.front().second);
        q_.pop_front();
        return 1;
    }
    void close() {
        { std::lock_guard<std::mutex> lock(m_); closed_ = true; }
        cv_.notify_all();
    }
    bool closed() {
        std::lock_guard<std::mutex> lock(m_);
        return closed_;
    }
};
