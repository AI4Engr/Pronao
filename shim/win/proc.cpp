// Windows: running a program (see app_native.h). Which program and which arguments is decided in src/programs.clg.
#include <algorithm>
#include <string>
#include <vector>

#include "../app_native.h"
#include "util.h"

namespace {
// the C runtime's rules (CommandLineToArgvW reads them back): wrap in quotes when needed, double backslashes that precede a
// quote, escape the quote
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

struct Reader { HANDLE pipe; std::string* buffer; size_t cap; bool truncated = false; };

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

bool absolute(const std::wstring& w) {
    return w.size() >= 3 && (((w[0] | 32) >= L'a' && (w[0] | 32) <= L'z' && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/')) || (w[0] == L'\\' && w[1] == L'\\' && w[2] != L'?' && w[2] != L'.'));
}
}  // namespace

int app_os_env(std::string name, std::string& out) {
    const DWORD need = GetEnvironmentVariableW(widen(name).c_str(), nullptr, 0);
    if (need == 0) return 0;
    std::vector<wchar_t> buf(need + 1);
    const DWORD got = GetEnvironmentVariableW(widen(name).c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    out = narrow(std::wstring(buf.data(), got));
    return 1;
}

// 0 nothing there, 1 a file (+4 if it can be executed: on Windows that is for src/programs.clg to judge by name), 2 a folder
int app_os_path_kind(std::string path) {
    const DWORD a = GetFileAttributesW(widen(path).c_str());
    return a == INVALID_FILE_ATTRIBUTES ? 0 : (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 5;
}

// Runs `exe` (an absolute path) in `cwd` (an absolute folder): no stdin, no console window, only three handles inherited,
// inside a job object so that everything it starts dies with it. flags: 1 timed out, 2 stdout cut at max_bytes, 4 stderr cut.
int app_proc_run(std::string exe, std::vector<std::string> args, std::string cwd, int timeout_ms, int max_bytes,
                 std::string& out, std::string& err, int& exit_code, int& flags) {
    out.clear(); err.clear(); exit_code = -1; flags = 0;
    const std::wstring wexe = widen(exe), wcwd = widen(cwd);
    if (!absolute(wexe) || !absolute(wcwd) || exe.find('\0') != std::string::npos || cwd.find('\0') != std::string::npos) return 5;
    timeout_ms = std::max(1, std::min(timeout_ms, 600000));
    const size_t cap = static_cast<size_t>(std::max(0, std::min(max_bytes, 64 * 1024 * 1024)));
    std::wstring cmd = quoteArg(wexe);
    for (const auto& a : args) {
        const std::wstring w = widen(a);
        if (w.empty() && !a.empty()) return 5;
        if (a.find('\0') != std::string::npos) return 5;
        cmd += L" " + quoteArg(w);
    }
    if (cmd.size() > 32000) return 5;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr, nul = INVALID_HANDLE_VALUE, job = nullptr;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    std::vector<char> attrMem;
    int status = 4;
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
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job == nullptr || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) break;
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = outW; si.StartupInfo.hStdError = errW;
        si.lpAttributeList = attrs;
        // suspended, so that it is inside the job before it can start anything of its own
        if (!CreateProcessW(wexe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, wcwd.c_str(), &si.StartupInfo, &pi)) {
            const DWORD e = GetLastError();
            status = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY) ? 2 : 4;
            break;
        }
        started = true;
        if (!AssignProcessToJobObject(job, pi.hProcess)) { TerminateProcess(pi.hProcess, 1); break; }
        ResumeThread(pi.hThread);
        status = 0;
    } while (false);

    if (attrs != nullptr) DeleteProcThreadAttributeList(attrs);
    if (outW) CloseHandle(outW);       // the parent's copies of the write ends: without this the readers never see EOF
    if (errW) CloseHandle(errW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (status != 0) {
        if (outR) CloseHandle(outR);
        if (errR) CloseHandle(errR);
        if (job) CloseHandle(job);
        if (started) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        return status;
    }
    CloseHandle(pi.hThread);
    Reader* rOut = new Reader{outR, &out, cap};
    Reader* rErr = new Reader{errR, &err, cap};
    HANDLE readers[2] = {CreateThread(nullptr, 0, readerMain, rOut, 0, nullptr), CreateThread(nullptr, 0, readerMain, rErr, 0, nullptr)};
    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) {
        flags |= 1;
        TerminateJobObject(job, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    // The child is gone. Something it started may still hold the pipes: give the readers a second, then kill the rest.
    bool finished = readers[0] && readers[1] && WaitForMultipleObjects(2, readers, TRUE, 1000) == WAIT_OBJECT_0;
    if (!finished) {
        TerminateJobObject(job, 1);
        for (HANDLE t : readers) if (t) CancelSynchronousIo(t);
        finished = readers[0] && readers[1] && WaitForMultipleObjects(2, readers, TRUE, 3000) == WAIT_OBJECT_0;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exit_code = (flags & 1) ? -1 : static_cast<int>(code);
    if (rOut->truncated) flags |= 2;
    if (rErr->truncated) flags |= 4;
    CloseHandle(pi.hProcess);
    CloseHandle(job);                       // kill-on-close: nothing the child started outlives this call
    if (finished) {
        for (HANDLE t : readers) CloseHandle(t);
        CloseHandle(outR); CloseHandle(errR);
        delete rOut; delete rErr;
    }                                       // else (a reader that could not be woken): leaked rather than freed under it
    return 0;
}
