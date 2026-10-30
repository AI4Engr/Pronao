// POSIX: file primitives (see app_native.h). The policy is in src/files.clg.
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "../app_native.h"
#include "../handles.h"

namespace {
// `parent` + `leaf` are kept for modes 3 and 4 so a delete acts on the entry through the folder it was opened in
struct Entry { int fd = -1, parent = -1; std::string leaf, path; };
HandleTable<Entry> g_handles;

int mapErrno(int e) {
    switch (e) {
        case ENOENT: return 2;
        case EACCES: case EPERM: return 10;
        case EEXIST: return 6;
        case ENOTEMPTY: return 7;
        case EISDIR: return 8;
        case ENOTDIR: return 9;
        default: return 4;
    }
}
}  // namespace

int app_os_windows() { return 0; }

int app_fs_open(std::string path, int mode, int& h, std::string& fin, int& created) {
    created = 0;
    Entry e;
    e.path = path;
    const int base = O_CLOEXEC | O_NOCTTY;
    switch (mode) {
        case 0: e.fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | base); break;
        case 1: e.fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | base); break;
        case 2: e.fd = open(path.c_str(), O_PATH | base); break;
        case 3: case 4: {
            const size_t slash = path.find_last_of('/');
            e.leaf = path.substr(slash + 1);
            if (e.leaf.empty()) { e.fd = open(path.c_str(), O_PATH | base); break; }
            e.parent = open(slash == 0 ? "/" : path.substr(0, slash).c_str(), O_PATH | O_DIRECTORY | base);
            if (e.parent < 0) return mapErrno(errno);
            e.fd = openat(e.parent, e.leaf.c_str(), O_PATH | O_NOFOLLOW | base);
            break;
        }
        case 5: case 6:
            e.fd = mode == 5 ? open(path.c_str(), O_WRONLY | O_NONBLOCK | base) : -1;
            if (e.fd < 0 && (mode == 6 || errno == ENOENT)) {
                e.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | base, 0644);
                created = e.fd >= 0;
            }
            break;
        default: return 5;
    }
    if (e.fd < 0) {
        const int err = errno;
        if (e.parent >= 0) close(e.parent);
        return mapErrno(err);
    }
    char buf[PATH_MAX + 1];
    const ssize_t n = readlink(("/proc/self/fd/" + std::to_string(e.fd)).c_str(), buf, PATH_MAX);
    if (n <= 0) {
        if (created) unlink(path.c_str());
        if (e.parent >= 0) close(e.parent);
        close(e.fd);
        return 1;
    }
    fin.assign(buf, static_cast<size_t>(n));
    e.path = fin;                                       // where it REALLY is (a created file is removed by this, not by a link's name)
    h = g_handles.put(e);
    return 0;
}

int app_fs_close(int h) {
    Entry e;
    if (g_handles.take(h, e)) { close(e.fd); if (e.parent >= 0) close(e.parent); }
    return 0;
}

int app_fs_info(int h, int& kind, std::string& size, std::string& mtime) {
    Entry e;
    struct stat sb;
    if (!g_handles.get(h, e) || fstat(e.fd, &sb) != 0) return 4;
    kind = S_ISLNK(sb.st_mode) ? 'l' : S_ISDIR(sb.st_mode) ? 'd' : S_ISREG(sb.st_mode) ? 'f' : 'o';
    size = std::to_string(static_cast<unsigned long long>(sb.st_size));
    mtime = std::to_string(static_cast<long long>(sb.st_mtime));
    return 0;
}

int app_fs_read(int h, int max_bytes, std::string& out) {
    Entry e;
    struct stat sb;
    if (!g_handles.get(h, e) || fstat(e.fd, &sb) != 0) return 4;
    if (max_bytes < 0 || static_cast<unsigned long long>(sb.st_size) > static_cast<unsigned long long>(max_bytes)) return 3;
    std::string data(static_cast<size_t>(sb.st_size), '\0');
    size_t got = 0;
    while (got < data.size()) {
        const ssize_t n = read(e.fd, &data[got], data.size() - got);
        if (n < 0) return 4;
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    data.resize(got);
    out = std::move(data);
    return 0;
}

int app_fs_write(int h, std::string data, int append) {
    Entry e;
    if (!g_handles.get(h, e)) return 4;
    if (append ? lseek(e.fd, 0, SEEK_END) < 0 : ftruncate(e.fd, 0) != 0) return 4;
    for (size_t done = 0; done < data.size();) {
        const ssize_t n = write(e.fd, data.data() + done, data.size() - done);
        if (n <= 0) return 4;
        done += static_cast<size_t>(n);
    }
    return 0;
}

int app_fs_list(int h, std::string& out) {
    Entry e;
    if (!g_handles.get(h, e)) return 4;
    DIR* d = fdopendir(dup(e.fd));
    if (d == nullptr) return 4;
    std::string result;
    while (struct dirent* de = readdir(d)) {
        const std::string name = de->d_name;
        struct stat sb;
        if (name == "." || name == ".." || fstatat(dirfd(d), de->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) continue;
        result += std::string(S_ISLNK(sb.st_mode) ? "l" : S_ISDIR(sb.st_mode) ? "d" : "f") + '\0' + std::to_string(static_cast<unsigned long long>(sb.st_size)) + '\0' +
                  std::to_string(static_cast<long long>(sb.st_mtime)) + '\0' + name + '\0';
    }
    closedir(d);
    out = std::move(result);
    return 0;
}

int app_fs_mkdir(std::string path) {
    return mkdir(path.c_str(), 0755) == 0 ? 0 : mapErrno(errno);
}

int app_fs_delete(int h) {
    Entry e;
    struct stat sb;
    if (!g_handles.get(h, e) || fstat(e.fd, &sb) != 0) return 4;
    if (e.parent < 0) return unlink(e.path.c_str()) == 0 ? 0 : mapErrno(errno);          // a file this call created
    struct stat again;                                                                    // still the entry we opened?
    if (fstatat(e.parent, e.leaf.c_str(), &again, AT_SYMLINK_NOFOLLOW) != 0) return mapErrno(errno);
    if (again.st_ino != sb.st_ino || again.st_dev != sb.st_dev) return 4;
    return unlinkat(e.parent, e.leaf.c_str(), S_ISDIR(sb.st_mode) ? AT_REMOVEDIR : 0) == 0 ? 0 : mapErrno(errno);
}
