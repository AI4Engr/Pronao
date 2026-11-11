// Windows: file primitives (see app_native.h). The policy is in src/files.clg.
#include <algorithm>
#include <string>
#include <vector>

#include "../app_native.h"
#include "../handles.h"
#include "util.h"

namespace {
HandleTable<HANDLE> g_handles;
const DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

int mapError(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_NETPATH: case ERROR_BAD_PATHNAME: return 2;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return 10;
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return 6;
        case ERROR_DIR_NOT_EMPTY: return 7;
        case ERROR_DIRECTORY: return 9;
        default: return 4;
    }
}

bool get(int h, HANDLE& out) { return g_handles.get(h, out); }
long long unixSeconds(long long ft) { return ft / 10000000LL - 11644473600LL; }
std::string kindLetter(DWORD attrs) { return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? "l" : (attrs & FILE_ATTRIBUTE_DIRECTORY) ? "d" : "f"; }
}

int app_os_windows() { return 1; }

int app_fs_open(std::string path, int mode, int& h, std::string& fin, int& created) {
    created = 0;
    const std::wstring w = widen(path);
    DWORD access = FILE_READ_ATTRIBUTES, disposition = OPEN_EXISTING, flags = FILE_FLAG_BACKUP_SEMANTICS;
    switch (mode) {
        case 0: access = GENERIC_READ; flags = FILE_ATTRIBUTE_NORMAL; break;
        case 1: access = FILE_LIST_DIRECTORY; break;
        case 2: break;
        case 3: flags |= FILE_FLAG_OPEN_REPARSE_POINT; break;
        case 4: access |= DELETE; flags |= FILE_FLAG_OPEN_REPARSE_POINT; break;
        case 5: access = GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES; disposition = OPEN_ALWAYS; flags = FILE_ATTRIBUTE_NORMAL; break;
        case 6: access = GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES; disposition = CREATE_NEW; flags = FILE_ATTRIBUTE_NORMAL; break;
        default: return 5;
    }
    HANDLE f = CreateFileW(w.c_str(), access, kShare, nullptr, disposition, flags, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED && (mode == 0 || mode == 5 || mode == 6)) {      // a folder cannot be opened as a file
            const DWORD a = GetFileAttributesW(w.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return 8;
        }
        return mapError(e);
    }
    created = mode == 6 || (mode == 5 && GetLastError() != ERROR_ALREADY_EXISTS);
    std::vector<wchar_t> buf(512);
    DWORD n = GetFinalPathNameByHandleW(f, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buf.size()) { buf.resize(n + 1); n = GetFinalPathNameByHandleW(f, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS); }
    if (n == 0) {                                        // a device or anything else that has no place on disk
        if (created) { FILE_DISPOSITION_INFO d; d.DeleteFile = TRUE; SetFileInformationByHandle(f, FileDispositionInfo, &d, sizeof d); }
        CloseHandle(f);
        return 1;
    }
    fin = narrow(std::wstring(buf.data(), n));
    h = g_handles.put(f);
    return 0;
}

int app_fs_close(int h) {
    HANDLE f;
    if (g_handles.take(h, f)) CloseHandle(f);
    return 0;
}

int app_fs_info(int h, int& kind, std::string& size, std::string& mtime) {
    HANDLE f;
    BY_HANDLE_FILE_INFORMATION i;
    if (!get(h, f) || !GetFileInformationByHandle(f, &i)) return 4;
    const bool disk = GetFileType(f) == FILE_TYPE_DISK;
    kind = !disk ? 'o' : kindLetter(i.dwFileAttributes)[0];
    size = std::to_string((static_cast<unsigned long long>(i.nFileSizeHigh) << 32) | i.nFileSizeLow);
    mtime = std::to_string(unixSeconds((static_cast<long long>(i.ftLastWriteTime.dwHighDateTime) << 32) | i.ftLastWriteTime.dwLowDateTime));
    return 0;
}

int app_fs_read(int h, int max_bytes, std::string& out) {
    HANDLE f;
    LARGE_INTEGER size;
    if (!get(h, f) || !GetFileSizeEx(f, &size)) return 4;
    if (max_bytes < 0 || size.QuadPart > max_bytes) return 3;
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    size_t got = 0;
    while (got < data.size()) {
        DWORD n = 0;
        if (!ReadFile(f, &data[got], static_cast<DWORD>(std::min<size_t>(data.size() - got, 1u << 20)), &n, nullptr)) return 4;
        if (n == 0) break;
        got += n;
    }
    data.resize(got);
    out = std::move(data);
    return 0;
}

int app_fs_write(int h, std::string data, int append) {
    HANDLE f;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!get(h, f) || !SetFilePointerEx(f, zero, nullptr, append ? FILE_END : FILE_BEGIN)) return 4;
    for (size_t done = 0; done < data.size();) {
        DWORD n = 0;
        if (!WriteFile(f, data.data() + done, static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20)), &n, nullptr) || n == 0) return 4;
        done += n;
    }
    return (!append && !SetEndOfFile(f)) ? 4 : 0;
}

int app_fs_list(int h, std::string& out) {
    HANDLE f;
    if (!get(h, f)) return 4;
    std::vector<DWORD> buf(16384);
    std::string result;
    for (bool first = true;; first = false) {
        if (!GetFileInformationByHandleEx(f, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORD)))) {
            const DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_FILES) break;
            return mapError(e);
        }
        for (const char* p = reinterpret_cast<const char*>(buf.data());;) {
            const FILE_ID_BOTH_DIR_INFO* d = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(p);
            const std::wstring name(d->FileName, d->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                result += kindLetter(d->FileAttributes) + '\0' + std::to_string(d->EndOfFile.QuadPart) + '\0' + std::to_string(unixSeconds(d->LastWriteTime.QuadPart)) + '\0' + narrow(name) + '\0';
            }
            if (d->NextEntryOffset == 0) break;
            p += d->NextEntryOffset;
        }
    }
    out = std::move(result);
    return 0;
}

int app_fs_mkdir(std::string path) {
    const std::wstring w = widen(path);
    return CreateDirectoryW(w.c_str(), nullptr) ? 0 : mapError(GetLastError());
}

int app_fs_replace(std::string from, std::string to) {
    const std::wstring a = widen(from), b = widen(to);
    return MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : mapError(GetLastError());
}

int app_fs_delete(int h) {
    HANDLE f;
    FILE_DISPOSITION_INFO d;
    d.DeleteFile = TRUE;
    return (!get(h, f)) ? 4 : SetFileInformationByHandle(f, FileDispositionInfo, &d, sizeof d) ? 0 : mapError(GetLastError());
}
