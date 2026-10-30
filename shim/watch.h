// shim-internal: what the common watcher code (watch.cpp) and the per-platform loops (win/watch.cpp, posix/watch.cpp) share.
#pragma once
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct Watch {
    int id = 0, app = 0;
    std::string root;                          // an absolute folder, vetted by CeeLing before app_watch_start
    bool recursive = false;
    std::thread thread;
    std::atomic<bool> stop{false}, finished{false}, dropped{false};
    std::mutex armMutex;                       // the thread tells app_watch_start that the OS is now watching (or that it could not)
    std::condition_variable armCv;
    bool armed = false;
    void* stopEvent = nullptr;                 // Windows
    int stopPipe[2] = {-1, -1};                // POSIX
};

const int kQuietMs = 40;                       // how often a loop with something left to say wakes up on its own

// Queues "watch id kind path from" for the CeeLing main loop. If the queue is full the record is dropped and an "overflow"
// follows as soon as there is room (flushOverflow, called whenever the loop wakes).
void pushEvent(Watch& w, const std::string& kind, const std::string& path, const std::string& from);
void flushOverflow(Watch& w);

// Called by the loop once the operating system is really watching the folder (the first ReadDirectoryChangesW is issued / the inotify watches are
// added), or has failed to. app_watch_start does not return before that: a change made right after fs.watch answers would otherwise be lost.
inline void markArmed(Watch& w) {
    { std::lock_guard<std::mutex> lock(w.armMutex); w.armed = true; }
    w.armCv.notify_all();
}

// per platform
void watchMain(std::shared_ptr<Watch> w);      // runs on the watcher's thread until w->stop; ends with a "stopped" record
void prepareStop(Watch& w);
void signalStop(Watch& w);
void releaseStop(Watch& w);
