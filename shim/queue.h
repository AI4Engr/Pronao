// shim-internal: the queue between the threads that produce calls (UI thread, watchers, workers) and the one that consumes them (CeeLing's main loop).
// Header-only, so the real window and the test double share this code and the sanitizer runs cover it.
#pragma once
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// What the page's promise is rejected with when `admit` refuses (constant texts: nothing from the request is echoed)
inline const char* refusalJson(int code) {
    return code == 1 ? "{\"code\":\"too_large\",\"message\":\"the request is larger than the application allows\"}"
                     : "{\"code\":\"busy\",\"message\":\"the application has too many requests waiting\"}";
}

// Lets one thread wait for a change in ANY window's queue. Every queue bumps a generation counter after it changes; a waiter reads it BEFORE looking at the
// queues and sleeps only while it is unchanged, so a change between the look and the sleep is not lost.
class WaitSet {
    std::mutex m_;
    std::condition_variable cv_;
    unsigned long gen_ = 0;
public:
    void bump() {
        { std::lock_guard<std::mutex> lock(m_); ++gen_; }
        cv_.notify_all();
    }
    unsigned long generation() {
        std::lock_guard<std::mutex> lock(m_);
        return gen_;
    }
    // true if the generation moved on from `seen`
    bool waitChange(unsigned long seen, int timeout_ms) {
        std::unique_lock<std::mutex> lock(m_);
        auto changed = [&] { return gen_ != seen; };
        if (timeout_ms < 0) {
            cv_.wait(lock, changed);
            return true;
        }
#if defined(__SANITIZE_THREAD__)
        return cv_.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms), changed);
#else
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), changed);
#endif
    }
};

inline WaitSet& waitSet() {
    static WaitSet s;
    return s;
}

class CallQueue {
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::string>> q_;
    bool closed_ = false;
    std::set<std::string> pending_;     // page calls not answered yet
    bool closedReported_ = false;       // pop has told its caller (by returning 0) that the queue is closed and drained
    WaitSet* notify_ = nullptr;         // bumped after every change, for waitAny (set before the queue is visible to other threads)
public:
    void setNotify(WaitSet* w) { notify_ = w; }
    // Something to take, or a close that nobody has been told about yet (what waitAny looks for)
    bool ready() {
        std::lock_guard<std::mutex> lock(m_);
        return !q_.empty() || (closed_ && !closedReported_);
    }
    // The close has been reported: this queue has nothing more to say
    bool retired() {
        std::lock_guard<std::mutex> lock(m_);
        return closed_ && q_.empty() && closedReported_;
    }
    int push(std::string id, std::string request, int max_queued) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (static_cast<int>(q_.size()) >= max_queued) return 0;
            q_.emplace_back(std::move(id), std::move(request));
        }
        cv_.notify_all();
        if (notify_) notify_->bump();
        return 1;
    }
    // What the page's call gets: 0 queued, 1 refused as too large, 2 refused because the queue is full, 3 refused (a bad id; an
    // empty id marks the shim's own records, which a page must never be able to forge)
    int admit(const std::string& id, const std::string& request, int max_request, int max_queued) {
        if (id.empty() || id.size() > 64) return 3;
        if (static_cast<int>(request.size()) > max_request) return 1;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (pending_.count(id) > 0) return 3;
            if (static_cast<int>(q_.size()) >= max_queued) return 2;
            pending_.insert(id);
            q_.emplace_back(id, request);
        }
        cv_.notify_all();
        if (notify_) notify_->bump();
        return 0;
    }
// A page call is answered ONCE: true the first time for an id that is waiting, false for a second answer, an unknown id or a worker's.
    bool answer(const std::string& id) { std::lock_guard<std::mutex> l(m_); return pending_.erase(id) > 0; }
    bool waiting(const std::string& id) { std::lock_guard<std::mutex> l(m_); return pending_.count(id) > 0; }
    // 1 got one, 0 closed and drained, 2 timed out (timeout_ms < 0 waits forever)
    int pop(int timeout_ms, std::string& id, std::string& request) {
        std::unique_lock<std::mutex> lock(m_);
        auto ready = [&] { return !q_.empty() || closed_; };
        if (timeout_ms < 0) {
            cv_.wait(lock, ready);
        } else {
            // Under ThreadSanitizer only: GCC 11's TSan does not intercept pthread_cond_clockwait (what a steady_clock wait compiles to) and reports a double lock
            // that is not there, so the deadline is a system_clock one.
#if defined(__SANITIZE_THREAD__)
            if (!cv_.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms), ready)) return 2;
#else
            if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) return 2;
#endif
        }
        if (q_.empty()) {
            closedReported_ = true;
            return 0;
        }
        id = std::move(q_.front().first);
        request = std::move(q_.front().second);
        q_.pop_front();
        return 1;
    }
    void close() {
        { std::lock_guard<std::mutex> lock(m_); closed_ = true; }
        cv_.notify_all();
        if (notify_) notify_->bump();
    }
    bool closed() {
        std::lock_guard<std::mutex> lock(m_);
        return closed_;
    }
};

// Waits until one of the windows in `handles` has a call or record to take, or has closed. `lookup(handle)` returns its App (null if gone). Serves only ITS windows:
// other applications in the process (the tests start several) have queues of their own. `last` starts the scan after the window served last, so one busy window
// cannot starve the others. 1 and `handle` set: take it with pop(0, ...), which reports a close by returning 0; 2 timed out; 0 none has anything more to say.
template <typename AppT, typename Lookup>
int waitAny(const std::vector<int>& handles, Lookup lookup, std::atomic<int>& last, int timeout_ms, int& handle) {
    WaitSet& ws = waitSet();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    const int n = static_cast<int>(handles.size());
    if (n == 0) return 0;
    while (true) {
        const unsigned long seen = ws.generation();      // BEFORE looking: a later change makes waitChange return at once
        bool anyLive = false;
        for (int k = 0; k < n; ++k) {
            const int idx = (last.load() + k) % n;
            const std::shared_ptr<AppT> a = lookup(handles[static_cast<size_t>(idx)]);
            if (!a || a->calls.retired()) continue;
            anyLive = true;
            if (a->calls.ready()) {
                last = idx + 1;
                handle = handles[static_cast<size_t>(idx)];
                return 1;
            }
        }
        if (!anyLive) return 0;
        int remaining = -1;
        if (timeout_ms >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return 2;
            remaining = static_cast<int>(left);
        }
        ws.waitChange(seen, remaining);
    }
}
