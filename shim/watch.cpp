// Watching a folder (see app_native.h): the threads and their bookkeeping. What an event means and which watches may exist
// is decided in src/watches.clg; the loops that talk to the OS are in win/watch.cpp and posix/watch.cpp.
#include "watch.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "app_native.h"

namespace {
const int kMaxQueued = 2000;                // records waiting for the main loop, per window; beyond this they are dropped

// Every live watch, by window. A function-local static, so that it is built after the shim's own tables and torn down BEFORE
// them: at process exit it stops and joins whatever is still running (a joinable std::thread that is destroyed calls
// std::terminate), and those threads may still be queueing records until they have stopped.
struct Registry {
    std::mutex mutex;
    std::map<int, std::vector<std::shared_ptr<Watch>>> watches;
    int nextId = 1;
    ~Registry() { for (auto& entry : watches) stopAll(entry.second); }
    static void stopAll(std::vector<std::shared_ptr<Watch>>& list) {
        for (auto& w : list) { w->stop = true; signalStop(*w); }
        for (auto& w : list) { if (w->thread.joinable()) w->thread.join(); releaseStop(*w); }
        list.clear();
    }
};
Registry& registry() { static Registry r; return r; }

std::string pack(int id, const std::string& kind, const std::string& path, const std::string& from) {
    std::string r = "watch";
    r += '\0'; r += std::to_string(id); r += '\0'; r += kind; r += '\0'; r += path; r += '\0'; r += from; r += '\0';
    return r;
}
}  // namespace

void pushEvent(Watch& w, const std::string& kind, const std::string& path, const std::string& from) {
    const bool urgent = kind == "stopped" || kind == "overflow";      // these two must get through
    if (app_push(w.app, std::string(), pack(w.id, kind, path, from), urgent ? kMaxQueued + 10 : kMaxQueued) == 0) w.dropped = true;
}

void flushOverflow(Watch& w) {
    if (w.dropped.exchange(false)) pushEvent(w, "overflow", "", "");
}

int app_watch_start(int handle, std::string root, int recursive, int& watch_id) {
    watch_id = 0;
    auto w = std::make_shared<Watch>();
    w->app = handle; w->root = root; w->recursive = recursive != 0;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto& list = reg.watches[handle];
        list.erase(std::remove_if(list.begin(), list.end(), [](const std::shared_ptr<Watch>& x) {      // ones that ended by themselves
            if (!x->finished.load()) return false;
            if (x->thread.joinable()) x->thread.join();
            releaseStop(*x);
            return true;
        }), list.end());
        w->id = reg.nextId++;
        prepareStop(*w);
        list.push_back(w);
    }
    w->thread = std::thread(watchMain, w);
    watch_id = w->id;
    return 0;
}

int app_watch_stop(int handle, int watch_id) {
    std::vector<std::shared_ptr<Watch>> one;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.watches.find(handle);
        if (it == reg.watches.end()) return 2;
        auto& list = it->second;
        for (size_t i = 0; i < list.size(); ++i) if (list[i]->id == watch_id) { one.push_back(list[i]); list.erase(list.begin() + static_cast<long>(i)); break; }
    }
    if (one.empty()) return 2;
    Registry::stopAll(one);
    return 0;
}

int app_watch_stop_all(int handle) {
    std::vector<std::shared_ptr<Watch>> all;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.watches.find(handle);
        if (it == reg.watches.end()) return 0;
        all.swap(it->second);
        reg.watches.erase(it);
    }
    Registry::stopAll(all);
    return 0;
}
