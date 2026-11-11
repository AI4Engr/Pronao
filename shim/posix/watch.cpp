// Linux: inotify loop for one watch (see watch.h).
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <map>
#include <vector>

#include "../watch.h"

namespace {
void addTree(int ifd, const std::string& abs, const std::string& rel, bool recursive, std::map<int, std::string>& wds, int depth) {
    const uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW | IN_ONLYDIR;
    const int wd = inotify_add_watch(ifd, abs.c_str(), mask);
    if (wd < 0) return;
    wds[wd] = rel;
    if (!recursive || depth > 64) return;
    DIR* d = opendir(abs.c_str());
    if (d == nullptr) return;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        struct stat sb;
        if (name == "." || name == ".." || fstatat(dirfd(d), e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(sb.st_mode)) continue;   // links are never followed
        addTree(ifd, abs + "/" + name, rel.empty() ? name : rel + "/" + name, true, wds, depth + 1);
    }
    closedir(d);
}
}

void watchMain(std::shared_ptr<Watch> w) {
    const int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    std::map<int, std::string> wds;                        // watch descriptor -> folder relative to the root
    if (ifd >= 0) addTree(ifd, w->root, std::string(), w->recursive, wds, 0);
    markArmed(*w);                                         // (the watches are added: from here on the kernel queues every change)
    if (ifd < 0 || wds.empty()) { if (ifd >= 0) close(ifd); pushEvent(*w, "stopped", "failed", ""); w->finished = true; return; }
    std::map<uint32_t, std::string> movedFrom;
    std::vector<char> buf(65536);
    const char* why = "closed";
    while (!w->stop.load()) {
        struct pollfd fds[2] = {{ifd, POLLIN, 0}, {w->stopPipe[0], POLLIN, 0}};
        const int pr = poll(fds, 2, (movedFrom.empty() && !w->dropped.load()) ? -1 : kQuietMs);
        if (pr < 0) { if (errno == EINTR) continue; why = "failed"; break; }
        if (fds[1].revents & POLLIN) break;
        flushOverflow(*w);
        if (pr == 0) {                                     // quiet: a rename whose second half never came was a move out of the tree
            for (auto& m : movedFrom) pushEvent(*w, "removed", m.second, "");
            movedFrom.clear();
            continue;
        }
        const ssize_t n = read(ifd, buf.data(), buf.size());
        if (n <= 0) continue;
        bool rootGone = false;
        for (char* p = buf.data(); p < buf.data() + n;) {
            const struct inotify_event* ev = reinterpret_cast<const struct inotify_event*>(p);
            p += sizeof(struct inotify_event) + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) { pushEvent(*w, "overflow", "", ""); continue; }
            auto dirIt = wds.find(ev->wd);
            if (dirIt == wds.end()) continue;
            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) { if (dirIt->second.empty()) rootGone = true; continue; }
            if (ev->len == 0) continue;
            const std::string rel = dirIt->second.empty() ? std::string(ev->name) : dirIt->second + "/" + ev->name;
            if (ev->mask & IN_CREATE) {
                pushEvent(*w, "created", rel, "");
            } else if (ev->mask & IN_DELETE) {
                pushEvent(*w, "removed", rel, "");
            } else if (ev->mask & (IN_MODIFY | IN_CLOSE_WRITE)) {
                pushEvent(*w, "modified", rel, "");
            } else if (ev->mask & IN_MOVED_FROM) {
                movedFrom[ev->cookie] = rel;
            } else if (ev->mask & IN_MOVED_TO) {
                auto from = movedFrom.find(ev->cookie);
                if (from != movedFrom.end()) { pushEvent(*w, "renamed", rel, from->second); movedFrom.erase(from); }
                else pushEvent(*w, "created", rel, "");
            }
            if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO)) && w->recursive) addTree(ifd, w->root + "/" + rel, rel, true, wds, 1);
        }
        if (rootGone) { why = "folder_gone"; break; }
    }
    close(ifd);
    if (!w->stop.load()) pushEvent(*w, "stopped", why, "");
    w->finished = true;
}

void prepareStop(Watch& w) { if (pipe2(w.stopPipe, O_CLOEXEC) != 0) w.stopPipe[0] = w.stopPipe[1] = -1; }
void signalStop(Watch& w) { if (write(w.stopPipe[1], "x", 1) < 0) {} }
void releaseStop(Watch& w) { if (w.stopPipe[0] >= 0) close(w.stopPipe[0]); if (w.stopPipe[1] >= 0) close(w.stopPipe[1]); }
