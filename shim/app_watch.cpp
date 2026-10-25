#include "app_watch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_fs.h"
#include "app_shim_internal.h"

namespace {

const size_t kMaxWatchesPerWindow = 32;
const size_t kMaxQueued = 1000;           // records waiting for the main loop, per window; beyond this events are dropped
const size_t kMaxPerBatch = 500;
const int kQuietMs = 40;                  // a change is delivered once nothing more arrived for this long

struct Event {
    std::string kind, path, from;
    bool operator==(const Event& o) const { return kind == o.kind && path == o.path && from == o.from; }
};

struct Watch {
    int id = 0;
    int app = 0;
    std::string root;                      // canonical form, as app_fs_resolve gave it
    bool recursive = false;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
#ifdef _WIN32
    void* stopEvent = nullptr;
#else
    int stopPipe[2] = {-1, -1};
#endif
};

void signalStop(Watch& w);
void release(Watch& w);

// Every live watch, by window. A function-local static, so that it is built after the shim's own tables and torn down
// BEFORE them: at process exit it stops and joins whatever is still running (a joinable std::thread that is destroyed
// calls std::terminate), and those threads may still be queueing records into the shim until they have stopped.
struct Registry {
    std::mutex mutex;
    std::map<int, std::vector<std::shared_ptr<Watch>>> watches;   // window handle -> its watches
    int nextId = 1;
    ~Registry() {
        std::vector<std::shared_ptr<Watch>> all;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& entry : watches) for (auto& w : entry.second) all.push_back(w);
            watches.clear();
        }
        for (auto& w : all) { w->stop = true; signalStop(*w); }
        for (auto& w : all) {
            if (w->thread.joinable()) w->thread.join();
            release(*w);
        }
    }
};
Registry& registry() {
    static Registry r;
    return r;
}

std::string packRecord(int id, const Event& e) {
    std::string r;
    r += "watch"; r += '\0';
    r += std::to_string(id); r += '\0';
    r += e.kind; r += '\0';
    r += e.path; r += '\0';
    r += e.from; r += '\0';
    return r;
}

// Delivers a batch. false if the window no longer exists.
bool deliver(Watch& w, std::vector<Event>& batch, bool& overflowed) {
    if (batch.empty() && !overflowed) return true;
    // the permission may have been withdrawn since the watch began, or the folder replaced: re-check, then say nothing more
    std::string now;
    int st = app_fs_resolve(w.root, 2, now);
    if (st != APP_FS_OK || now != w.root) {
        app_internal_push_call(w.app, std::string(), packRecord(w.id, Event{"stopped", st == APP_FS_NOT_FOUND ? "folder_gone" : "access_revoked", ""}), kMaxQueued + 10);
        w.stop = true;
        batch.clear();
        overflowed = false;
        return true;
    }
    if (batch.size() > kMaxPerBatch) { batch.resize(kMaxPerBatch); overflowed = true; }
    for (const auto& e : batch) {
        int r = app_internal_push_call(w.app, std::string(), packRecord(w.id, e), kMaxQueued);
        if (r < 0) return false;
        if (r == 0) overflowed = true;               // the main loop is behind: drop, and say so below
    }
    batch.clear();
    if (overflowed) {
        int r = app_internal_push_call(w.app, std::string(), packRecord(w.id, Event{"overflow", "", ""}), kMaxQueued + 10);
        if (r < 0) return false;
        overflowed = false;
    }
    return true;
}

void addMerged(std::vector<Event>& pending, const Event& e) {
    if (!pending.empty() && pending.back() == e) return;      // the same change again, straight away
    // a burst of "modified" for one file collapses to one
    if (e.kind == "modified") {
        for (const auto& p : pending) if (p == e) return;
    }
    pending.push_back(e);
}

void finishWatch(Watch& w, const char* why) {
    if (!w.stop.load()) {
        app_internal_push_call(w.app, std::string(), packRecord(w.id, Event{"stopped", why, ""}), kMaxQueued + 10);
    }
    w.finished = true;
}

}  // namespace

#ifdef _WIN32
// ===============================================================================================================
// Windows: ReadDirectoryChangesW
// ===============================================================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

std::wstring widen(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), &w[0], n);
    return w;
}

std::string narrowSlash(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

void watchMain(std::shared_ptr<Watch> w) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE dir = CreateFileW(widen(w->root).c_str(), FILE_LIST_DIRECTORY, share, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) { finishWatch(*w, "failed"); return; }
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<DWORD> storage(65536 / sizeof(DWORD));      // DWORD-aligned, as FILE_NOTIFY_INFORMATION needs
    char* buf = reinterpret_cast<char*>(storage.data());
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    std::vector<Event> pending;
    bool overflowed = false;
    std::wstring renamedFrom;
    bool reading = false;
    const char* why = "closed";

    while (!w->stop.load()) {
        if (!reading) {
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(dir, buf, static_cast<DWORD>(storage.size() * sizeof(DWORD)), w->recursive ? TRUE : FALSE,
                                       filter, nullptr, &ov, nullptr)) { why = "failed"; break; }
            reading = true;
        }
        HANDLE waits[2] = {ov.hEvent, w->stopEvent};
        DWORD got = WaitForMultipleObjects(2, waits, FALSE, pending.empty() && !overflowed ? INFINITE : kQuietMs);
        if (got == WAIT_OBJECT_0 + 1) break;
        if (got == WAIT_TIMEOUT) {                              // quiet for kQuietMs: deliver
            if (!deliver(*w, pending, overflowed)) break;
            continue;
        }
        if (got != WAIT_OBJECT_0) { why = "failed"; break; }
        DWORD n = 0;
        reading = false;
        if (!GetOverlappedResult(dir, &ov, &n, FALSE)) {
            DWORD e = GetLastError();
            why = (e == ERROR_ACCESS_DENIED || e == ERROR_NETNAME_DELETED || e == ERROR_DIRECTORY || e == ERROR_FILE_NOT_FOUND) ? "folder_gone" : "failed";
            break;
        }
        if (n == 0) { overflowed = true; continue; }
        const char* p = buf;
        for (;;) {
            const FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
            const std::wstring name(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            switch (fni->Action) {
                case FILE_ACTION_ADDED: addMerged(pending, Event{"created", narrowSlash(name), ""}); break;
                case FILE_ACTION_REMOVED: addMerged(pending, Event{"removed", narrowSlash(name), ""}); break;
                case FILE_ACTION_MODIFIED: addMerged(pending, Event{"modified", narrowSlash(name), ""}); break;
                case FILE_ACTION_RENAMED_OLD_NAME: renamedFrom = name; break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    addMerged(pending, Event{"renamed", narrowSlash(name), narrowSlash(renamedFrom)});
                    renamedFrom.clear();
                    break;
                default: break;
            }
            if (fni->NextEntryOffset == 0) break;
            p += fni->NextEntryOffset;
        }
        if (pending.size() > kMaxPerBatch * 4) { pending.resize(kMaxPerBatch); overflowed = true; }
    }
    if (reading) {
        CancelIoEx(dir, &ov);
        DWORD n = 0;
        GetOverlappedResult(dir, &ov, &n, TRUE);                 // the kernel must be done with `buf` before it goes away
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
    finishWatch(*w, why);
}

void signalStop(Watch& w) { SetEvent(static_cast<HANDLE>(w.stopEvent)); }
void prepare(Watch& w) { w.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
void release(Watch& w) { if (w.stopEvent) CloseHandle(static_cast<HANDLE>(w.stopEvent)); w.stopEvent = nullptr; }

}  // namespace

#else
// ===============================================================================================================
// Linux: inotify
// ===============================================================================================================
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void addTree(int ifd, const std::string& abs, const std::string& rel, bool recursive, std::map<int, std::string>& wds,
             int depth) {
    const uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                          IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR;
    int wd = inotify_add_watch(ifd, abs.c_str(), mask);
    if (wd < 0) return;
    wds[wd] = rel;
    if (!recursive || depth > 64) return;
    DIR* d = opendir(abs.c_str());
    if (d == nullptr) return;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat sb;
        if (fstatat(dirfd(d), e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(sb.st_mode)) continue;   // links are never followed
        addTree(ifd, abs + "/" + name, rel.empty() ? name : rel + "/" + name, true, wds, depth + 1);
    }
    closedir(d);
}

void watchMain(std::shared_ptr<Watch> w) {
    int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ifd < 0) { finishWatch(*w, "failed"); return; }
    std::map<int, std::string> wds;                    // watch descriptor -> folder relative to the root
    addTree(ifd, w->root, std::string(), w->recursive, wds, 0);
    if (wds.empty()) { close(ifd); finishWatch(*w, "failed"); return; }
    std::vector<Event> pending;
    bool overflowed = false;
    std::map<uint32_t, std::string> movedFrom;         // cookie -> old relative path
    std::vector<char> buf(65536);
    const char* why = "closed";
    while (!w->stop.load()) {
        struct pollfd fds[2] = {{ifd, POLLIN, 0}, {w->stopPipe[0], POLLIN, 0}};
        int pr = poll(fds, 2, pending.empty() && !overflowed ? -1 : kQuietMs);
        if (pr < 0) { if (errno == EINTR) continue; why = "failed"; break; }
        if (fds[1].revents & POLLIN) break;
        if (pr == 0) {
            for (auto& m : movedFrom) addMerged(pending, Event{"removed", m.second, ""});   // moved out of the tree
            movedFrom.clear();
            if (!deliver(*w, pending, overflowed)) break;
            continue;
        }
        ssize_t n = read(ifd, buf.data(), buf.size());
        if (n <= 0) continue;
        bool rootGone = false;
        for (char* p = buf.data(); p < buf.data() + n;) {
            const struct inotify_event* ev = reinterpret_cast<const struct inotify_event*>(p);
            p += sizeof(struct inotify_event) + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) { overflowed = true; continue; }
            auto dirIt = wds.find(ev->wd);
            if (dirIt == wds.end()) continue;
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                if (dirIt->second.empty()) rootGone = true;
                continue;
            }
            if (ev->len == 0) continue;
            const std::string rel = dirIt->second.empty() ? std::string(ev->name) : dirIt->second + "/" + ev->name;
            if (ev->mask & IN_CREATE) {
                addMerged(pending, Event{"created", rel, ""});
                if ((ev->mask & IN_ISDIR) && w->recursive) addTree(ifd, w->root + "/" + rel, rel, true, wds, 1);
            } else if (ev->mask & IN_DELETE) {
                addMerged(pending, Event{"removed", rel, ""});
            } else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                addMerged(pending, Event{"modified", rel, ""});
            } else if (ev->mask & IN_MOVED_FROM) {
                movedFrom[ev->cookie] = rel;
            } else if (ev->mask & IN_MOVED_TO) {
                auto from = movedFrom.find(ev->cookie);
                if (from != movedFrom.end()) {
                    addMerged(pending, Event{"renamed", rel, from->second});
                    movedFrom.erase(from);
                } else {
                    addMerged(pending, Event{"created", rel, ""});          // moved in from outside
                }
                if ((ev->mask & IN_ISDIR) && w->recursive) addTree(ifd, w->root + "/" + rel, rel, true, wds, 1);
            }
        }
        if (rootGone) { why = "folder_gone"; break; }
        if (pending.size() > kMaxPerBatch * 4) { pending.resize(kMaxPerBatch); overflowed = true; }
    }
    close(ifd);
    finishWatch(*w, why);
}

void signalStop(Watch& w) { if (write(w.stopPipe[1], "x", 1) < 0) {} }
void prepare(Watch& w) { if (pipe2(w.stopPipe, O_CLOEXEC) != 0) { w.stopPipe[0] = w.stopPipe[1] = -1; } }
void release(Watch& w) {
    if (w.stopPipe[0] >= 0) close(w.stopPipe[0]);
    if (w.stopPipe[1] >= 0) close(w.stopPipe[1]);
}

}  // namespace
#endif

int app_watch_start(int handle, std::string path, int recursive, int& watch_id) {
    watch_id = 0;
    if (!app_internal_exists(handle)) return APP_FS_NOT_FOUND;
    std::string canonical;
    int st = app_fs_resolve(path, 2, canonical);
    if (st != APP_FS_OK) return st;
    auto w = std::make_shared<Watch>();
    w->app = handle;
    w->root = canonical;
    w->recursive = recursive != 0;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto& list = reg.watches[handle];
        // drop the bookkeeping of watches that ended by themselves (their thread is finished)
        list.erase(std::remove_if(list.begin(), list.end(), [](const std::shared_ptr<Watch>& x) {
            if (!x->finished.load()) return false;
            if (x->thread.joinable()) x->thread.join();
            release(*x);
            return true;
        }), list.end());
        if (list.size() >= kMaxWatchesPerWindow) return APP_FS_TOO_MANY;
        w->id = reg.nextId++;
        prepare(*w);
        list.push_back(w);
    }
    w->thread = std::thread(watchMain, w);
    watch_id = w->id;
    return APP_FS_OK;
}

int app_watch_stop(int handle, int watch_id) {
    std::shared_ptr<Watch> found;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.watches.find(handle);
        if (it == reg.watches.end()) return APP_FS_NOT_FOUND;
        auto& list = it->second;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i]->id == watch_id) { found = list[i]; list.erase(list.begin() + static_cast<long>(i)); break; }
        }
    }
    if (!found) return APP_FS_NOT_FOUND;
    found->stop = true;
    signalStop(*found);
    if (found->thread.joinable()) found->thread.join();
    release(*found);
    return APP_FS_OK;
}

int app_watch_stop_all(int handle) {
    std::vector<std::shared_ptr<Watch>> all;
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.watches.find(handle);
        if (it == reg.watches.end()) return APP_FS_OK;
        all.swap(it->second);
        reg.watches.erase(it);
    }
    for (auto& w : all) { w->stop = true; signalStop(*w); }
    for (auto& w : all) {
        if (w->thread.joinable()) w->thread.join();
        release(*w);
    }
    return APP_FS_OK;
}
