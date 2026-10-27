// Windows: running a program (see app_native.h). Which program and which arguments is decided in src/app.clg.
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
    return out + L'"';
}

struct Reader { HANDLE pipe; std::string* buffer; size_t cap; bool truncated = false; };

DWORD WINAPI readerMain(LPVOID p) {
    Reader* r = static_cast<Reader*>(p);
    std::vector<char> chunk(65536);
    for (DWORD n = 0; ReadFile(r->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &n, nullptr) && n > 0;) {
        const size_t room = r->cap > r->buffer->size() ? r->cap - r->buffer->size() : 0;
        r->buffer->append(chunk.data(), std::min<size_t>(n, room));
        if (n > room) r->truncated = true;
    }
    return 0;
}

bool absolute(const std::wstring& w) {   // C:\... or \\server\...; never relative, never a device path
    return w.size() >= 3 && (((w[0] | 32) >= L'a' && (w[0] | 32) <= L'z' && w[1] == L':' && (w[2] == L'\\' || w[2] == L'/')) ||
                             (w[0] == L'\\' && w[1] == L'\\' && w[2] != L'?' && w[2] != L'.'));
}

struct AttrList {   // the list that says which handles the child inherits
    std::vector<char> mem;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    bool make(HANDLE* handles, size_t bytes) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        mem.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(mem.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return list = nullptr, false;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, bytes, nullptr, nullptr) != 0;
    }
    ~AttrList() { if (list) DeleteProcThreadAttributeList(list); }
};
}  // namespace

int app_os_env(std::string name, std::string& out) {
    const std::wstring w = widen(name);
    std::vector<wchar_t> buf(GetEnvironmentVariableW(w.c_str(), nullptr, 0) + 1);
    const DWORD got = GetEnvironmentVariableW(w.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (got == 0) return 0;
    out = narrow(std::wstring(buf.data(), got));
    return 1;
}

// 0 nothing there, 1 a file (+4 if it can be executed: on Windows that is for CeeLing to judge by name), 2 a folder
int app_os_path_kind(std::string path) {
    const DWORD a = GetFileAttributesW(widen(path).c_str());
    return a == INVALID_FILE_ATTRIBUTES ? 0 : (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 5;
}

// Runs `exe` in `cwd` (both absolute) with exactly the environment `env`: no stdin, no console window, only three handles inherited,
// inside a job object so that everything it starts dies with it. flags: 1 timed out, 2 stdout cut at max_bytes, 4 stderr cut.
int app_proc_run(std::string exe, std::vector<std::string> args, std::string cwd, std::vector<std::string> env, int timeout_ms,
                 int max_bytes, std::string& out, std::string& err, int& exit_code, int& flags) {
    out.clear(); err.clear(); exit_code = -1; flags = 0;
    const std::wstring wexe = widen(exe), wcwd = widen(cwd);
    if (!absolute(wexe) || !absolute(wcwd)) return 5;
    timeout_ms = std::max(1, std::min(timeout_ms, 600000));
    const size_t cap = static_cast<size_t>(std::max(0, std::min(max_bytes, 64 * 1024 * 1024)));
    std::wstring cmd = quoteArg(wexe), block;
    for (const auto& a : args) {
        const std::wstring w = widen(a);
        if ((w.empty() && !a.empty()) || w.find(wchar_t(0)) != std::wstring::npos) return 5;
        cmd += L" " + quoteArg(w);
    }
    for (const auto& e : env) {
        const std::wstring w = widen(e);
        if (w.empty() || e.find('=') == std::string::npos || w.find(wchar_t(0)) != std::wstring::npos) return 5;
        block += w + L'\0';
    }
    block += env.empty() ? std::wstring(2, wchar_t(0)) : std::wstring(1, wchar_t(0));
    if (cmd.size() > 32000 || exe.find('\0') != std::string::npos || cwd.find('\0') != std::string::npos) return 5;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, nullptr, TRUE};
    Handle outR, outW, errR, errW, job, process, thread;
    if (!CreatePipe(outR.put(), outW.put(), &inheritable, 0) || !CreatePipe(errR.put(), errW.put(), &inheritable, 0)) return 4;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);          // only the write ends are inherited
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr));
    HANDLE inherit[3] = {nul, outW, errW};                      // the child inherits exactly these three handles and nothing else
    AttrList attrs;
    job.h = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!nul || !job || !attrs.make(inherit, sizeof inherit) || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) return 4;
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = outW; si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrs.list;
    PROCESS_INFORMATION pi = {};
    // suspended, so that it is inside the job before it can start anything of its own
    if (!CreateProcessW(wexe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        block.data(), wcwd.c_str(), &si.StartupInfo, &pi)) {
        const DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_DIRECTORY) ? 2 : 4;
    }
    process.h = pi.hProcess; thread.h = pi.hThread;
    if (!AssignProcessToJobObject(job, process)) { TerminateProcess(process, 1); return 4; }
    ResumeThread(thread);
    outW.reset(); errW.reset();         // the parent's copies of the write ends: without this the readers never see EOF

    Reader* rOut = new Reader{outR, &out, cap};
    Reader* rErr = new Reader{errR, &err, cap};
    HANDLE readers[2] = {CreateThread(nullptr, 0, readerMain, rOut, 0, nullptr), CreateThread(nullptr, 0, readerMain, rErr, 0, nullptr)};
    if (WaitForSingleObject(process, static_cast<DWORD>(timeout_ms)) == WAIT_TIMEOUT) {
        flags |= 1;
        TerminateJobObject(job, 1);
        WaitForSingleObject(process, 5000);
    }
    // The child is gone. Something it started may still hold the pipes: give the readers a second, then kill the rest.
    bool finished = readers[0] && readers[1] && WaitForMultipleObjects(2, readers, TRUE, 1000) == WAIT_OBJECT_0;
    if (!finished) {
        TerminateJobObject(job, 1);
        for (HANDLE t : readers) if (t) CancelSynchronousIo(t);
        finished = readers[0] && readers[1] && WaitForMultipleObjects(2, readers, TRUE, 3000) == WAIT_OBJECT_0;
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    exit_code = (flags & 1) ? -1 : static_cast<int>(code);
    if (rOut->truncated) flags |= 2;
    if (rErr->truncated) flags |= 4;
    if (finished) {                     // else (a reader that could not be woken): leaked rather than freed under it
        for (HANDLE t : readers) CloseHandle(t);
        delete rOut; delete rErr;
    } else {
        outR.h = errR.h = nullptr;      // (the pipes stay open for the stuck readers)
    }
    return 0;                           // the job handle closes here: kill-on-close, nothing the child started outlives this call
}
