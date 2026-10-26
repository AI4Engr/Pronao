// shim-internal: the queue between the threads that produce calls (UI thread, watchers, workers) and the one that consumes them
// (CeeLing's main loop). Header-only, so the real window and the test double share this very code and the sanitizer runs cover it.
#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

class CallQueue {
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::string>> q_;
    bool closed_ = false;
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
