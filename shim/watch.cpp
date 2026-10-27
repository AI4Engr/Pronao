// Watching a folder (see app_native.h): the threads and their bookkeeping. What an event means and which watches may exist is
// decided in src/app.clg; the loops that talk to the OS are in win/watch.cpp and posix/watch.cpp.
#include "watch.h"

#include <mutex>
#include <vector>

#include "app_native.h"

namespace {
const int kMaxQueued = 2000;                // records waiting for the main loop, per window; beyond this they are dropped

// Every live watch. A function-local static, so that it is built after the shim's own tables and torn down BEFORE them: at process
// exit it stops and joins whatever is still running (a joinable std::thread that is destroyed calls std::terminate), and those
// threads may still be queueing records until they have stopped.
struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Watch>> all;
    int nextId = 1;
    ~Registry() { stop([](Watch&) { return true; }); }
    // stops and joins every watch `pick` selects; how many there were
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
}  // namespace

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
