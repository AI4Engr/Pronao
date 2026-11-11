// shim-internal: which calls the application asked to cancel. A blocking primitive (running a program) polls cancelled() while it waits; the registry only
// remembers. Keyed by window and call id; a closing window cancels every call it has, also ones that start later (handles are never reused).
#pragma once
#include <mutex>
#include <set>
#include <string>
#include <utility>

class CancelRegistry {
    std::mutex m_;
    std::set<std::pair<int, std::string>> calls_;
    std::set<int> closing_;
public:
    void cancel(int handle, const std::string& call) { std::lock_guard<std::mutex> l(m_); calls_.insert({handle, call}); }
    void cancelAll(int handle) { std::lock_guard<std::mutex> l(m_); closing_.insert(handle); }
    void forget(int handle, const std::string& call) { std::lock_guard<std::mutex> l(m_); calls_.erase({handle, call}); }
    bool cancelled(int handle, const std::string& call) {
        std::lock_guard<std::mutex> l(m_);
        return closing_.count(handle) > 0 || calls_.count({handle, call}) > 0;
    }
};
inline CancelRegistry& cancelRegistry() { static CancelRegistry r; return r; }
