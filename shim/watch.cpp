// Watching a folder (see app_native.h): the threads and their bookkeeping. What an event means is decided in src/watches.clg and which watches may exist in src/app.clg;
// the loops that talk to the OS are in win/watch.cpp and posix/watch.cpp.
#include "watch.h"

#include <chrono>
#include <mutex>
#include <vector>

#include "app_native.h"
#include "app_shim_internal.h"

namespace {
const int kMaxQueued = 2000;                // records waiting for the main loop, per window; beyond this they are dropped

// Every live watch. A function-local static, so it is destroyed BEFORE the shim's own tables: at process exit it stops and joins whatever still runs (a destroyed
// joinable std::thread calls std::terminate), and those threads may still be queueing records.
struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Watch>> all;
    int nextId = 1;
    ~Registry() { stop([](Watch&) { return true; }); }
    template <class F> int stop(F pick) {
        std::vector<std::shared_ptr<Watch>> gone;
        {
            std::lock_guard<std::mutex> lock(m);
            for (size_t i = 0; i < all.size();) {
                if (pick(*all[i])) { gone.push_back(all[i]); all.erase(all.begin() + static_cast<long>(i)); } else { ++i; }
            }
        }
        for (auto& w : gone) { w->stop = true; signalStop(*w); }
        for (auto& w : gone) { if (w->thread.joinable()) w->thread.join(); releaseStop(*w); }
        return static_cast<int>(gone.size());
    }
};
Registry& registry() { static Registry r; return r; }
}

void pushEvent(Watch& w, const std::string& kind, const std::string& path, const std::string& from) {
    const bool urgent = kind == "stopped" || kind == "overflow";      // these two must get through
    std::string r = "watch";
    for (const std::string& field : {std::to_string(w.id), kind, path, from}) { r += '\0'; r += field; }
    r += '\0';
    if (app_push(w.app, std::string(), r, urgent ? kMaxQueued + 10 : kMaxQueued) == 0) w.dropped = true;
}

void flushOverflow(Watch& w) {
    if (w.dropped.exchange(false)) pushEvent(w, "overflow", "", "");
}

int app_watch_start(int handle, std::string root, int recursive, int& watch_id) {
    Registry& reg = registry();
    reg.stop([](Watch& x) { return x.finished.load(); });             // ones that ended by themselves
    auto w = std::make_shared<Watch>();
    w->app = handle; w->root = root; w->recursive = recursive != 0;
    prepareStop(*w);
    {
        std::lock_guard<std::mutex> lock(reg.m);
        w->id = reg.nextId++;
        reg.all.push_back(w);
    }
    w->thread = std::thread(watchMain, w);
    {
        // wait (at most 5 s) until the loop has armed the watch
        std::unique_lock<std::mutex> lock(w->armMutex);
#if defined(__SANITIZE_THREAD__)
        // (TSan workaround: see queue.h)
        w->armCv.wait_until(lock, std::chrono::system_clock::now() + std::chrono::seconds(5), [&] { return w->armed; });
#else
        w->armCv.wait_for(lock, std::chrono::seconds(5), [&] { return w->armed; });
#endif
    }
    watch_id = w->id;
    return 0;
}

int app_watch_stop(int handle, int watch_id) {
    return registry().stop([&](Watch& x) { return x.app == handle && x.id == watch_id; }) > 0 ? 0 : 2;
}

int app_watch_stop_all(int handle) {
    registry().stop([&](Watch& x) { return x.app == handle; });
    return 0;
}
