// POSIX: running a program (see app_native.h). Which program and which arguments is decided in src/programs.clg.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include "../app_native.h"
#include "../cancel.h"

int app_os_env(std::string name, std::string& out) {
    const char* v = name.find('\0') == std::string::npos ? std::getenv(name.c_str()) : nullptr;
    if (v == nullptr) return 0;
    out = v;
    return 1;
}

// 0 nothing there, 1 a file (+4 if it can be executed), 2 a folder
int app_os_path_kind(std::string path) {
    struct stat sb;
    if (path.find('\0') != std::string::npos || stat(path.c_str(), &sb) != 0) return 0;
    return S_ISDIR(sb.st_mode) ? 2 : S_ISREG(sb.st_mode) ? (access(path.c_str(), X_OK) == 0 ? 5 : 1) : 0;
}

// Runs `exe` in `cwd` (both absolute) with exactly the environment `env`: no stdin, in its own process group so that everything it
// starts dies with it. flags: 1 timed out, 2 stdout cut at max_bytes, 4 stderr cut.
int app_proc_run(int handle, std::string call, std::string exe, std::vector<std::string> args, std::string cwd, std::vector<std::string> env,
                 int timeout_ms, int max_bytes, std::string& out, std::string& err, int& exit_code, int& flags) {
    out.clear(); err.clear(); exit_code = -1; flags = 0;
    if (exe.empty() || exe[0] != '/' || cwd.empty() || cwd[0] != '/' || exe.find('\0') != std::string::npos || cwd.find('\0') != std::string::npos) return 5;
    for (const auto& a : args) if (a.find('\0') != std::string::npos) return 5;
    for (const auto& e : env) if (e.find('\0') != std::string::npos || e.find('=') == std::string::npos) return 5;
    struct stat sb;
    if (stat(exe.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode) || stat(cwd.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) return 2;
    timeout_ms = std::max(1, std::min(timeout_ms, 600000));
    const size_t cap = static_cast<size_t>(std::max(0, std::min(max_bytes, 64 * 1024 * 1024)));
    std::vector<char*> argv{const_cast<char*>(exe.c_str())};
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    int outPipe[2], errPipe[2], failPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0 || pipe2(errPipe, O_CLOEXEC) != 0 || pipe2(failPipe, O_CLOEXEC) != 0) return 4;
    const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) return 4;
    if (pid == 0) {                                  // child: only async-signal-safe calls from here
        setpgid(0, 0);
        dup2(devnull, 0); dup2(outPipe[1], 1); dup2(errPipe[1], 2);
        if (chdir(cwd.c_str()) != 0) { int e = errno; if (write(failPipe[1], &e, sizeof e) < 0) {} _exit(127); }
        execve(exe.c_str(), argv.data(), envp.data());
        int e = errno;
        if (write(failPipe[1], &e, sizeof e) < 0) {}
        _exit(127);
    }
    setpgid(pid, pid);                               // also from the parent, so there is no window
    close(outPipe[1]); close(errPipe[1]); close(failPipe[1]); close(devnull);
    int execErr = 0;
    const bool execFailed = read(failPipe[0], &execErr, sizeof execErr) == static_cast<ssize_t>(sizeof execErr);
    close(failPipe[0]);

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    bool outOpen = true, errOpen = true, exited = false, killed = false;
    Clock::time_point exitedAt;
    int wstatus = 0;
    std::vector<char> chunk(65536);
    while (outOpen || errOpen) {
        struct pollfd fds[2];
        nfds_t nfds = 0;
        if (outOpen) { fds[nfds] = {outPipe[0], POLLIN, 0}; ++nfds; }
        if (errOpen) { fds[nfds] = {errPipe[0], POLLIN, 0}; ++nfds; }
        poll(fds, nfds, 25);
        for (nfds_t k = 0; k < nfds; ++k) {
            if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const bool isOut = fds[k].fd == outPipe[0];
            const ssize_t n = read(fds[k].fd, chunk.data(), chunk.size());
            if (n <= 0) { (isOut ? outOpen : errOpen) = false; continue; }
            std::string& buf = isOut ? out : err;
            const size_t room = cap > buf.size() ? cap - buf.size() : 0;
            if (room > 0) buf.append(chunk.data(), std::min<size_t>(static_cast<size_t>(n), room));
            if (static_cast<size_t>(n) > room) flags |= isOut ? 2 : 4;
        }
        if (!exited && waitpid(pid, &wstatus, WNOHANG) == pid) { exited = true; exitedAt = Clock::now(); }
        if (!killed && !exited && cancelRegistry().cancelled(handle, call)) { flags |= 8; killpg(pid, SIGKILL); killed = true; }
        if (!killed && !exited && Clock::now() >= deadline) { flags |= 1; killpg(pid, SIGKILL); killed = true; }
        // the child is gone but something it started still holds the pipes: one second, then kill the rest
        if (exited && !killed && Clock::now() - exitedAt > std::chrono::seconds(1)) { killpg(pid, SIGKILL); killed = true; }
        if (killed && Clock::now() - deadline > std::chrono::seconds(10)) break;
    }
    if (!exited) { killpg(pid, SIGKILL); waitpid(pid, &wstatus, 0); }
    killpg(pid, SIGKILL);                            // anything that outlived the child in its group
    close(outPipe[0]); close(errPipe[0]);
    if (execFailed) return execErr == ENOENT ? 2 : 4;
    if (flags & 9) exit_code = -1;
    else if (WIFEXITED(wstatus)) exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) exit_code = 128 + WTERMSIG(wstatus);
    return 0;
}
