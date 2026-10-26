// shim-internal: a table of small integers standing for OS handles, so CeeLing never holds a pointer or a raw descriptor.
#pragma once
#include <map>
#include <mutex>

template <class T>
class HandleTable {
    std::mutex m_;
    std::map<int, T> items_;
    int next_ = 1;
public:
    int put(const T& v) { std::lock_guard<std::mutex> l(m_); items_[next_] = v; return next_++; }
    bool get(int id, T& out) { std::lock_guard<std::mutex> l(m_); auto it = items_.find(id); if (it == items_.end()) return false; out = it->second; return true; }
    bool take(int id, T& out) { std::lock_guard<std::mutex> l(m_); auto it = items_.find(id); if (it == items_.end()) return false; out = it->second; items_.erase(it); return true; }
};
