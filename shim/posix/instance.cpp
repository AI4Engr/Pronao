// POSIX: one running copy of a program per user (see app_native.h), and the command line.
//   claim   an exclusive flock on "<runtime dir>/pronao/<name>.lock", held until the process ends (the kernel drops it if the process dies, so a crash leaves nothing stale):
//           the process that gets it is the instance.
//   handoff a later process connects to the instance's unix socket "<name>.sock" in the same directory and writes its working folder and arguments (a message is at most
//           64 KiB). The directory belongs to the user and is mode 0700 (checked, not assumed), the socket is 0600, and the instance also checks the connecting
//           process's user id (SO_PEERCRED), so only this user's own processes can write to it.
//   serve   one thread in the instance queues each message as an "instance" record on the window given to app_instance_listen (up to 16 are kept before that).
// What may be done with the arguments is decided in src/app.clg; this only moves bytes between two processes of the same user.
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../app_native.h"

namespace {
const size_t kMaxMessage = 64 * 1024;
const size_t kMaxPending = 16;

struct Instance {
    int lockFd = -1;
    int listenFd = -1;
    std::mutex m;
    int handle = 0;
    std::vector<std::string> pending;
};
Instance* g_instance = nullptr;

std::string recordFor(const std::string& message) {
    std::string r = "instance";
    r += '\0';
    r += message;
    return r;
}

void deliver(Instance* in, const std::string& message) {
    std::lock_guard<std::mutex> lock(in->m);
    if (in->handle != 0) {
        app_push(in->handle, std::string(), recordFor(message), 100);
    } else if (in->pending.size() < kMaxPending) {
        in->pending.push_back(message);
    }
}

// "<runtime dir>/pronao", made if it is not there, and only used if it is a real directory of this user that nobody else can enter
bool privateDirectory(std::string& dir) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string base = runtime && runtime[0] == '/' ? std::string(runtime) : "/tmp/pronao-" + std::to_string(static_cast<long>(getuid()));
    if (!(runtime && runtime[0] == '/')) {
        if (mkdir(base.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    dir = base + "/pronao";
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) return false;
    return true;
}

bool socketAddress(const std::string& path, sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

void serve(Instance* in) {
    while (true) {
        const int client = accept(in->listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            return;
        }
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
            close(client);
            continue;
        }
        std::string message;
        bool tooLong = false;
        char chunk[4096];
        while (true) {
            pollfd p = {client, POLLIN, 0};
            if (poll(&p, 1, 2000) <= 0) break;                  // a client that goes quiet is dropped after two seconds
            const ssize_t got = read(client, chunk, sizeof chunk);
            if (got <= 0) break;
            if (message.size() + static_cast<size_t>(got) > kMaxMessage) { tooLong = true; break; }
            message.append(chunk, static_cast<size_t>(got));
        }
        close(client);
        if (!tooLong && !message.empty()) deliver(in, message);
    }
}

std::string currentFolder() {
    char buffer[4096];
    return getcwd(buffer, sizeof buffer) ? std::string(buffer) : std::string();
}
}

int app_command_line(std::string& out) {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    if (!in) return 4;
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t program = all.find('\0');
    out = program == std::string::npos ? std::string() : all.substr(program + 1);
    return 0;
}

int app_instance_claim(std::string name, int& first) {
    first = 1;
    if (name.empty() || name.size() > 64 || g_instance != nullptr) return 5;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 5;
    }
    std::string dir;
    if (!privateDirectory(dir)) return 4;
    const std::string lockPath = dir + "/" + name + ".lock", sockPath = dir + "/" + name + ".sock";
    const int lockFd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lockFd < 0) return 4;
    if (flock(lockFd, LOCK_EX | LOCK_NB) == 0) {
        // the instance: (any socket file left here is from a process that is gone, since the lock was free)
        sockaddr_un addr;
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || !socketAddress(sockPath, addr)) { if (fd >= 0) close(fd); close(lockFd); return 4; }
        unlink(sockPath.c_str());
        const mode_t old = umask(077);
        const int bound = bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        umask(old);
        if (bound != 0 || listen(fd, 16) != 0) { close(fd); close(lockFd); return 4; }
        chmod(sockPath.c_str(), 0600);
        Instance* in = new Instance();
        in->lockFd = lockFd;
        in->listenFd = fd;
        g_instance = in;
        std::thread(serve, in).detach();
        return 0;
    }
    close(lockFd);
    first = 0;
    // tell the one that is running: its working folder and arguments (the instance may still be starting its socket: try for a couple of seconds)
    std::string message = currentFolder();
    message += '\0';
    std::string args;
    app_command_line(args);
    message += args;
    if (message.size() > kMaxMessage) message.resize(kMaxMessage);
    sockaddr_un addr;
    if (!socketAddress(sockPath, addr)) return 4;
    for (int attempt = 0; attempt < 20; attempt++) {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
            size_t sent = 0;
            while (sent < message.size()) {
                const ssize_t n = write(fd, message.data() + sent, message.size() - sent);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
            close(fd);
            return sent == message.size() ? 0 : 4;
        }
        if (fd >= 0) close(fd);
        usleep(100 * 1000);
    }
    return 4;
}

int app_instance_listen(int handle) {
    if (g_instance == nullptr || handle == 0) return 5;
    std::vector<std::string> now;
    {
        std::lock_guard<std::mutex> lock(g_instance->m);
        g_instance->handle = handle;
        now.swap(g_instance->pending);
    }
    for (const auto& message : now) app_push(handle, std::string(), recordFor(message), 100);
    return 0;
}
