// Windows: running a program (see app_native.h). Which program and arguments is decided in src/programs.clg.
#include <algorithm>
#include <string>
#include <vector>

#include "../app_native.h"
#include "../cancel.h"
#include "util.h"

namespace {
// Quotes one argument by the C runtime's rules, which CommandLineToArgvW reads back.
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

struct AttrList {
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
}

int app_os_env(std::string name, std::string& out) {
    const std::wstring w = widen(name);
    std::vector<wchar_t> buf(GetEnvironmentVariableW(w.c_str(), nullptr, 0) + 1);
    const DWORD got = GetEnvironmentVariableW(w.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (got == 0) return 0;
    out = narrow(std::wstring(buf.data(), got));
    return 1;
}

// 0 nothing there, 1 a file (+4 if it can be executed: always reported here, CeeLing judges by name), 2 a folder
int app_os_path_kind(std::string path) {
    const DWORD a = GetFileAttributesW(widen(path).c_str());
    return a == INVALID_FILE_ATTRIBUTES ? 0 : (a & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 5;
}

// Runs `exe` in `cwd` with exactly the environment `env` (vetted by CeeLing: toolRunOk): no stdin, no console window, only three handles inherited, inside a job object
// so that everything it starts dies with it. flags: 1 timed out, 2 stdout cut at max_bytes, 4 stderr cut, 8 cancelled.
int app_proc_run(int handle, std::string call, std::string exe, std::vector<std::string> args, std::string cwd, std::vector<std::string> env,
                 int timeout_ms, int max_bytes, std::string& out, std::string& err, int& exit_code, int& flags) {
    out.clear(); err.clear(); exit_code = -1; flags = 0;
    const std::wstring wexe = widen(exe), wcwd = widen(cwd);
    const size_t cap = static_cast<size_t>(max_bytes);
    std::wstring cmd = quoteArg(wexe), block;
    for (const auto& a : args) cmd += L" " + quoteArg(widen(a));
    for (const auto& e : env) block += widen(e) + L'\0';
    block += env.empty() ? std::wstring(2, wchar_t(0)) : std::wstring(1, wchar_t(0));
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    SECURITY_ATTRIBUTES inheritable = {sizeof inheritable, nullptr, TRUE};
    Handle outR, outW, errR, errW, job, process, thread;
    if (!CreatePipe(outR.put(), outW.put(), &inheritable, 0) || !CreatePipe(errR.put(), errW.put(), &inheritable, 0)) return 4;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr));
    HANDLE inherit[3] = {nul, outW, errW};
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
    // wait in slices, so that a cancel (the call was abandoned, the window is closing) is noticed within 50 ms
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    bool exited = false;
    while (!exited) {
        const bool cancelled = cancelRegistry().cancelled(handle, call);
        const bool late = GetTickCount64() >= deadline;
        if (cancelled || late) {
            flags |= cancelled ? 8 : 1;
            TerminateJobObject(job, 1);
            WaitForSingleObject(process, 5000);
            break;
        }
        exited = WaitForSingleObject(process, 50) == WAIT_OBJECT_0;
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
    exit_code = (flags & 9) ? -1 : static_cast<int>(code);
    if (rOut->truncated) flags |= 2;
    if (rErr->truncated) flags |= 4;
    if (finished) {                     // else (a reader that could not be woken): leaked rather than freed under it
        for (HANDLE t : readers) CloseHandle(t);
        delete rOut; delete rErr;
    } else {
        outR.h = errR.h = nullptr;
    }
    return 0;                           // the job handle closes here: kill-on-close, nothing the child started outlives this call
}
