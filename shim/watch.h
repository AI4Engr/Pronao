// shim-internal: what the common watcher code (watch.cpp) and the per-platform loops (win/watch.cpp, posix/watch.cpp) share.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

struct Watch {
    int id = 0, app = 0;
    std::string root;                          // an absolute folder, vetted by CeeLing before app_watch_start
    bool recursive = false;
    std::thread thread;
    std::atomic<bool> stop{false}, finished{false}, dropped{false};
    void* stopEvent = nullptr;                 // Windows
    int stopPipe[2] = {-1, -1};                // POSIX
};

const int kQuietMs = 40;                       // how often a loop with something left to say wakes up on its own

// Queues "watch id kind path from" for the CeeLing main loop. If the queue is full the record is dropped and an "overflow"
// follows as soon as there is room (flushOverflow, called whenever the loop wakes).
void pushEvent(Watch& w, const std::string& kind, const std::string& path, const std::string& from);
void flushOverflow(Watch& w);

// per platform
void watchMain(std::shared_ptr<Watch> w);      // runs on the watcher's thread until w->stop; ends with a "stopped" record
void prepareStop(Watch& w);
void signalStop(Watch& w);
void releaseStop(Watch& w);
