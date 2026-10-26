// Windows: ReadDirectoryChangesW loop for one watch (see watch.h).
#include <vector>

#include "../watch.h"
#include "util.h"

namespace {
std::string slashes(const std::wstring& w) {
    std::string s = narrow(w);
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}
}  // namespace

void watchMain(std::shared_ptr<Watch> w) {
    HANDLE dir = CreateFileW(widen(w->root).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) { pushEvent(*w, "stopped", "failed", ""); w->finished = true; return; }
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<DWORD> storage(65536 / sizeof(DWORD));      // DWORD-aligned, as FILE_NOTIFY_INFORMATION needs
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    std::wstring renamedFrom;
    bool reading = false;
    const char* why = "closed";
    while (!w->stop.load()) {
        if (!reading) {
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(dir, storage.data(), static_cast<DWORD>(storage.size() * sizeof(DWORD)), w->recursive, filter, nullptr, &ov, nullptr)) { why = "failed"; break; }
            reading = true;
        }
        HANDLE waits[2] = {ov.hEvent, w->stopEvent};
        const DWORD got = WaitForMultipleObjects(2, waits, FALSE, w->dropped.load() ? kQuietMs : INFINITE);
        if (got == WAIT_OBJECT_0 + 1) break;
        flushOverflow(*w);
        if (got == WAIT_TIMEOUT) continue;
        DWORD n = 0;
        reading = false;
        if (got != WAIT_OBJECT_0) { why = "failed"; break; }
        if (!GetOverlappedResult(dir, &ov, &n, FALSE)) {
            const DWORD e = GetLastError();
            why = (e == ERROR_ACCESS_DENIED || e == ERROR_NETNAME_DELETED || e == ERROR_DIRECTORY || e == ERROR_FILE_NOT_FOUND) ? "folder_gone" : "failed";
            break;
        }
        if (n == 0) { pushEvent(*w, "overflow", "", ""); continue; }
        for (const char* p = reinterpret_cast<const char*>(storage.data());;) {
            const FILE_NOTIFY_INFORMATION* f = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
            const std::wstring name(f->FileName, f->FileNameLength / sizeof(wchar_t));
            switch (f->Action) {
                case FILE_ACTION_ADDED: pushEvent(*w, "created", slashes(name), ""); break;
                case FILE_ACTION_REMOVED: pushEvent(*w, "removed", slashes(name), ""); break;
                case FILE_ACTION_MODIFIED: pushEvent(*w, "modified", slashes(name), ""); break;
                case FILE_ACTION_RENAMED_OLD_NAME: renamedFrom = name; break;
                case FILE_ACTION_RENAMED_NEW_NAME: pushEvent(*w, "renamed", slashes(name), slashes(renamedFrom)); renamedFrom.clear(); break;
                default: break;
            }
            if (f->NextEntryOffset == 0) break;
            p += f->NextEntryOffset;
        }
    }
    if (reading) {
        CancelIoEx(dir, &ov);
        DWORD n = 0;
        GetOverlappedResult(dir, &ov, &n, TRUE);             // the kernel must be done with the buffer before it goes away
    }
    CloseHandle(ov.hEvent);
    CloseHandle(dir);
    if (!w->stop.load()) pushEvent(*w, "stopped", why, "");
    w->finished = true;
}

void prepareStop(Watch& w) { w.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
void signalStop(Watch& w) { SetEvent(static_cast<HANDLE>(w.stopEvent)); }
void releaseStop(Watch& w) { if (w.stopEvent) CloseHandle(static_cast<HANDLE>(w.stopEvent)); w.stopEvent = nullptr; }
