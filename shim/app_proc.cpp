#include "app_proc.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "app_fs.h"

namespace {
const int kMaxArgs = 256;
const size_t kMaxCommandBytes = 30000;     // stays under CreateProcess's 32767-character limit after quoting

// one argument per NUL-terminated part
bool splitArgs(const std::string& packed, std::vector<std::string>& out) {
    std::string cur;
    for (char c : packed) {
        if (c == '\0') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) return false;          // the last argument must be terminated
    if (static_cast<int>(out.size()) > kMaxArgs) return false;
    size_t total = 0;
    for (const auto& a : out) {
        if (app_fs_is_utf8(a) != 1) return false;
        total += a.size() + 3;
    }
    return total <= kMaxCommandBytes;
}
}  // namespace

int app_proc_clean_text(std::string in, std::string& out) {
    std::string result;
    result.reserve(in.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    int replaced = 0;
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { result += static_cast<char>(c); ++i; continue; }
        size_t len = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        bool ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (ok && len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (ok && len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ok = false;
        if (ok) { result.append(in, i, len); i += len; }
        else { result += "\xEF\xBF\xBD"; ++i; replaced = 1; }
    }
    out = std::move(result);
    return replaced;
}

#ifdef APP_HEADLESS
int app_proc_test_helper(std::string& path) {
    const char* v = std::getenv("APP_PROC_ECHO");
    if (v == nullptr || *v == '\0') return 0;
    path = v;
    return 1;
}
#endif

#ifdef _WIN32
// ===============================================================================================================
// Windows
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

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

bool isDriveAbsolute(const std::wstring& w) {
    return w.size() >= 3 && ((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z')) && w[1] == L':' &&
           (w[2] == L'\\' || w[2] == L'/');
}

bool isUncAbsolute(const std::wstring& w) {
    return w.size() >= 3 && (w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/') && w[2] != L'?' && w[2] != L'.';
}

bool endsWithExe(const std::wstring& w) {
    return w.size() >= 4 && CompareStringOrdinal(w.c_str() + w.size() - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL;
}

bool isFile(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isDirectory(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring fullPath(const std::wstring& w) {
    std::vector<wchar_t> buf(MAX_PATH + 1);
    DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n >= buf.size()) {
        buf.resize(n + 1);
        n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    }
    return n == 0 ? std::wstring() : std::wstring(buf.data(), n);
}

// the C runtime's rules (CommandLineToArgvW reads them back): wrap in quotes when needed, double backslashes that
// precede a quote, escape the quote
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++backslashes; }
        if (i == a.size()) { out.append(backslashes * 2, L'\\'); break; }
        if (a[i] == L'"') { out.append(backslashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else { out.append(backslashes, L'\\'); out.push_back(a[i]); }
    }
    out.push_back(L'"');
    return out;
}

struct Reader {
    HANDLE pipe;
    std::string* buffer;
    size_t cap;
    bool truncated = false;
};

DWORD WINAPI readerMain(LPVOID p) {
    Reader* r = static_cast<Reader*>(p);
    std::vector<char> chunk(65536);
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(r->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &n, nullptr) || n == 0) break;
        const size_t room = r->cap > r->buffer->size() ? r->cap - r->buffer->size() : 0;
        if (room > 0) r->buffer->append(chunk.data(), std::min<size_t>(n, room));
        if (n > room) r->truncated = true;
    }
    return 0;
}

}  // namespace

int app_proc_resolve(std::string name, std::string& absolute) {
    if (name.empty() || name.find('\0') != std::string::npos || app_fs_is_utf8(name) != 1) return APP_PROC_INVALID;
    const std::wstring w = widen(name);
    if (w.empty()) return APP_PROC_INVALID;
    const bool hasSeparator = w.find_first_of(L"\\/:") != std::wstring::npos;
    if (hasSeparator) {
        if (!isDriveAbsolute(w) && !isUncAbsolute(w)) return APP_PROC_INVALID;       // relative, device or drive-relative
        if (w.find(L':', 2) != std::wstring::npos) return APP_PROC_INVALID;           // alternate data streams
        const std::wstring full = fullPath(w);
        if (full.empty() || !endsWithExe(full)) return APP_PROC_INVALID;
        if (!isFile(full)) return APP_PROC_NOT_FOUND;
        absolute = narrow(full);
        return APP_PROC_OK;
    }
    for (wchar_t c : w) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' || c == L'-';
        if (!ok) return APP_PROC_INVALID;
    }
    if (w == L"." || w == L"..") return APP_PROC_INVALID;
    const std::wstring file = endsWithExe(w) ? w : w + L".exe";
    DWORD need = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (need == 0) return APP_PROC_NOT_FOUND;
    std::vector<wchar_t> pathVar(need + 1);
    DWORD got = GetEnvironmentVariableW(L"PATH", pathVar.data(), static_cast<DWORD>(pathVar.size()));
    const std::wstring path(pathVar.data(), got);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (!isDriveAbsolute(dir) && !isUncAbsolute(dir)) continue;                 // "" and "." would mean the current folder
        while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        const std::wstring candidate = dir + (dir.back() == L'\\' ? L"" : L"\\") + file;
        if (isFile(candidate)) { absolute = narrow(fullPath(candidate)); return APP_PROC_OK; }
    }
    return APP_PROC_NOT_FOUND;
}

int app_proc_run(std::string exe, std::string args, std::string cwd, int timeout_ms, int max_bytes,
                 std::string& out, std::string& err, int& exit_code, int& flags) {
    out.clear();
    err.clear();
    exit_code = -1;
    flags = 0;
    std::vector<std::string> argv;
    if (!splitArgs(args, argv)) return APP_PROC_INVALID;
    const std::wstring wexe = widen(exe), wcwd = widen(cwd);
    if (wexe.empty() || wcwd.empty() || !(isDriveAbsolute(wexe) || isUncAbsolute(wexe)) || !endsWithExe(wexe)) return APP_PROC_INVALID;
    if (!(isDriveAbsolute(wcwd) || isUncAbsolute(wcwd))) return APP_PROC_INVALID;
    if (!isFile(wexe) || !isDirectory(wcwd)) return APP_PROC_NOT_FOUND;
    timeout_ms = std::max(1, std::min(timeout_ms, 600000));
    const size_t cap = static_cast<size_t>(std::max(0, std::min(max_bytes, 64 * 1024 * 1024)));

    std::wstring cmd = quoteArg(wexe);
    for (const auto& a : argv) cmd += L" " + quoteArg(widen(a));
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    SECURITY_ATTRIBUTES inheritable;
    inheritable.nLength = sizeof inheritable;
    inheritable.lpSecurityDescriptor = nullptr;
    inheritable.bInheritHandle = TRUE;
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr, nul = INVALID_HANDLE_VALUE;
    HANDLE job = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    std::vector<char> attrMem;
    int status = APP_PROC_SPAWN_FAILED;
    PROCESS_INFORMATION pi = {};
    bool started = false;

    do {
        if (!CreatePipe(&outR, &outW, &inheritable, 0) || !CreatePipe(&errR, &errW, &inheritable, 0)) break;
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);          // only the write ends are inherited
        SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
        if (nul == INVALID_HANDLE_VALUE) break;

        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrMem.resize(size);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrMem.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) { attrs = nullptr; break; }
        HANDLE inherit[3] = {nul, outW, errW};       // the child inherits exactly these three handles and nothing else
        if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr)) break;

        job = CreateJobObjectW(nullptr, nullptr);
        if (job == nullptr) break;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) break;

        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = outW;
        si.StartupInfo.hStdError = errW;
        si.lpAttributeList = attrs;
        // suspended, so that it is inside the job before it can start anything of its own
        if (!CreateProcessW(wexe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, wcwd.c_str(),
                            &si.StartupInfo, &pi)) break;
        started = true;
        if (!AssignProcessToJobObject(job, pi.hProcess)) { TerminateProcess(pi.hProcess, 1); break; }
        ResumeThread(pi.hThread);
        status = APP_PROC_OK;
    } while (false);

    if (attrs != nullptr) DeleteProcThreadAttributeList(attrs);
    if (outW) CloseHandle(outW);       // the parent's copies of the write ends: without this the readers never see EOF
    if (errW) CloseHandle(errW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (status != APP_PROC_OK) {
        if (outR) CloseHandle(outR);
        if (errR) CloseHandle(errR);
        if (job) CloseHandle(job);
        if (started) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        return status;
    }
    CloseHandle(pi.hThread);

    Reader* rOut = new Reader{outR, &out, cap};
    Reader* rErr = new Reader{errR, &err, cap};
    HANDLE tOut = CreateThread(nullptr, 0, readerMain, rOut, 0, nullptr);
    HANDLE tErr = CreateThread(nullptr, 0, readerMain, rErr, 0, nullptr);

    DWORD waited = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms));
    if (waited == WAIT_TIMEOUT) {
        flags |= 1;
        TerminateJobObject(job, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    // The child is gone. Something it started may still hold the pipes: give the readers a second, then kill the rest.
    HANDLE readers[2] = {tOut, tErr};
    bool finished = tOut && tErr && WaitForMultipleObjects(2, readers, TRUE, 1000) == WAIT_OBJECT_0;
    if (!finished) {
        TerminateJobObject(job, 1);
        if (tOut) CancelSynchronousIo(tOut);
        if (tErr) CancelSynchronousIo(tErr);
        finished = tOut && tErr && WaitForMultipleObjects(2, readers, TRUE, 3000) == WAIT_OBJECT_0;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exit_code = (flags & 1) ? -1 : static_cast<int>(code);
    if (rOut->truncated) flags |= 2;
    if (rErr->truncated) flags |= 4;
    CloseHandle(pi.hProcess);
    CloseHandle(job);                       // kill-on-close: nothing the child started outlives this call
    if (finished) {
        CloseHandle(tOut); CloseHandle(tErr);
        CloseHandle(outR); CloseHandle(errR);
        delete rOut; delete rErr;
    }                                       // else (a reader that could not be woken): leaked rather than freed under it
    return APP_PROC_OK;
}

#else
// ===============================================================================================================
// POSIX
// ===============================================================================================================
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool isFileAt(const std::string& p) {
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 && S_ISREG(sb.st_mode);
}
bool isDirAt(const std::string& p) {
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}
}  // namespace

int app_proc_resolve(std::string name, std::string& absolute) {
    if (name.empty() || name.find('\0') != std::string::npos || app_fs_is_utf8(name) != 1) return APP_PROC_INVALID;
    if (name.find('/') != std::string::npos) {
        if (name[0] != '/') return APP_PROC_INVALID;                    // relative with a separator
        char buf[4096];
        if (realpath(name.c_str(), buf) == nullptr) return errno == ENOENT ? APP_PROC_NOT_FOUND : APP_PROC_INVALID;
        if (!isFileAt(buf) || access(buf, X_OK) != 0) return APP_PROC_NOT_FOUND;
        absolute = buf;
        return APP_PROC_OK;
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return APP_PROC_INVALID;
    }
    if (name == "." || name == "..") return APP_PROC_INVALID;
    const char* pv = std::getenv("PATH");
    if (pv == nullptr) return APP_PROC_NOT_FOUND;
    const std::string path = pv;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.empty() || dir[0] != '/') continue;                     // "" and relative entries mean the current folder
        const std::string candidate = dir + "/" + name;
        if (isFileAt(candidate) && access(candidate.c_str(), X_OK) == 0) {
            char buf[4096];
            absolute = realpath(candidate.c_str(), buf) ? std::string(buf) : candidate;
            return APP_PROC_OK;
        }
    }
    return APP_PROC_NOT_FOUND;
}

int app_proc_run(std::string exe, std::string args, std::string cwd, int timeout_ms, int max_bytes,
                 std::string& out, std::string& err, int& exit_code, int& flags) {
    out.clear();
    err.clear();
    exit_code = -1;
    flags = 0;
    std::vector<std::string> argv;
    if (!splitArgs(args, argv)) return APP_PROC_INVALID;
    if (exe.empty() || exe[0] != '/' || cwd.empty() || cwd[0] != '/') return APP_PROC_INVALID;
    if (!isFileAt(exe) || !isDirAt(cwd)) return APP_PROC_NOT_FOUND;
    timeout_ms = std::max(1, std::min(timeout_ms, 600000));
    const size_t cap = static_cast<size_t>(std::max(0, std::min(max_bytes, 64 * 1024 * 1024)));

    std::vector<char*> cargv;
    cargv.push_back(const_cast<char*>(exe.c_str()));
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    int outPipe[2], errPipe[2], failPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0 || pipe2(errPipe, O_CLOEXEC) != 0 || pipe2(failPipe, O_CLOEXEC) != 0) return APP_PROC_SPAWN_FAILED;
    const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) return APP_PROC_SPAWN_FAILED;
    if (pid == 0) {                                  // child: only async-signal-safe calls from here
        setpgid(0, 0);
        dup2(devnull, 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        if (chdir(cwd.c_str()) != 0) { int e = errno; if (write(failPipe[1], &e, sizeof e) < 0) {} _exit(127); }
        execv(exe.c_str(), cargv.data());
        int e = errno;
        if (write(failPipe[1], &e, sizeof e) < 0) {}
        _exit(127);
    }
    setpgid(pid, pid);                               // also from the parent, so there is no window
    close(outPipe[1]); close(errPipe[1]); close(failPipe[1]); close(devnull);
    int execErr = 0;
    bool execFailed = read(failPipe[0], &execErr, sizeof execErr) == static_cast<ssize_t>(sizeof execErr);
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
        if (outOpen) { fds[nfds].fd = outPipe[0]; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds; }
        if (errOpen) { fds[nfds].fd = errPipe[0]; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds; }
        poll(fds, nfds, 25);
        for (nfds_t k = 0; k < nfds; ++k) {
            if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const bool isOut = fds[k].fd == outPipe[0];
            ssize_t n = read(fds[k].fd, chunk.data(), chunk.size());
            if (n <= 0) { (isOut ? outOpen : errOpen) = false; continue; }
            std::string& buf = isOut ? out : err;
            const size_t room = cap > buf.size() ? cap - buf.size() : 0;
            if (room > 0) buf.append(chunk.data(), std::min<size_t>(static_cast<size_t>(n), room));
            if (static_cast<size_t>(n) > room) flags |= isOut ? 2 : 4;
        }
        if (!exited && waitpid(pid, &wstatus, WNOHANG) == pid) { exited = true; exitedAt = Clock::now(); }
        if (!killed && !exited && Clock::now() >= deadline) {
            flags |= 1;
            killpg(pid, SIGKILL);
            killed = true;
        }
        // the child is gone but something it started still holds the pipes: one second, then kill the rest
        if (exited && !killed && Clock::now() - exitedAt > std::chrono::seconds(1)) { killpg(pid, SIGKILL); killed = true; }
        if (killed && Clock::now() - deadline > std::chrono::seconds(10)) break;
    }
    if (!exited) {
        killpg(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
    }
    killpg(pid, SIGKILL);                            // anything that outlived the child in its group
    close(outPipe[0]);
    close(errPipe[0]);
    if (execFailed) return execErr == ENOENT ? APP_PROC_NOT_FOUND : APP_PROC_SPAWN_FAILED;
    if (flags & 1) exit_code = -1;
    else if (WIFEXITED(wstatus)) exit_code = WEXITSTATUS(wstatus);
    else if (WIFSIGNALED(wstatus)) exit_code = 128 + WTERMSIG(wstatus);
    return APP_PROC_OK;
}
#endif
