#include "app_fs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <system_error>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(APP_HEADLESS)
#include <filesystem>
#endif

// ---------------------------------------------------------------------------------------------------------------
// shared: UTF-8 validation, record formatting
// ---------------------------------------------------------------------------------------------------------------
int app_fs_is_utf8(std::string s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        size_t len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return 0;
        if (i + len > n) return 0;
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
        if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
        i += len;
    }
    return 1;
}

namespace {

std::string record(char kind, unsigned long long size, long long mtime, const std::string& name) {
    std::string r;
    r += kind;
    r += '\0';
    r += std::to_string(size);
    r += '\0';
    r += std::to_string(mtime);
    r += '\0';
    r += name;
    r += '\0';
    return r;
}

bool validInput(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) return false;
    return app_fs_is_utf8(path) == 1;
}

std::mutex g_rootsMutex;

}  // namespace

#ifdef _WIN32
// ===============================================================================================================
// Windows
// ===============================================================================================================
namespace {

struct Root {
    std::wstring finalForm;   // what GetFinalPathNameByHandle returns for the folder: \\?\C:\dir  or  \\?\UNC\srv\share\dir
    std::wstring plain;       // the same without the \\?\ prefix:                         C:\dir  or  \\srv\share\dir
    bool exact = false;       // a granted FILE: only this very path matches, not anything under it
    bool grant = false;       // added by app_fs_grant_*, withdrawable with app_fs_revoke_grants
};
std::vector<Root> g_roots;

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

bool equalNoCase(const std::wstring& a, size_t aOff, const std::wstring& b, size_t len) {
    if (aOff + len > a.size() || len > b.size()) return false;
    return CompareStringOrdinal(a.c_str() + aOff, static_cast<int>(len), b.c_str(), static_cast<int>(len), TRUE) == CSTR_EQUAL;
}

// `path` is `root` or lies under it, on a component boundary (so C:\app2 is NOT inside C:\app).
bool isInside(const std::wstring& path, const std::wstring& root) {
    if (path.size() < root.size() || !equalNoCase(path, 0, root, root.size())) return false;
    if (path.size() == root.size()) return true;
    if (!root.empty() && root.back() == L'\\') return true;
    return path[root.size()] == L'\\';
}

std::wstring plainOf(const std::wstring& finalForm) {
    if (finalForm.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\" + finalForm.substr(7);
    if (finalForm.compare(0, 4, L"\\\\?\\") == 0) return finalForm.substr(4);
    return finalForm;
}

bool finalPathOf(HANDLE h, std::wstring& out) {
    std::vector<wchar_t> buf(512);
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0) return false;
        if (n < buf.size()) { out.assign(buf.data(), n); return true; }
        buf.resize(n + 1);
    }
    return false;
}

bool sameNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && equalNoCase(a, 0, b, b.size());
}

bool insideAnyFinal(const std::wstring& finalPath) {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    for (const auto& r : g_roots) {
        if (r.exact ? sameNoCase(finalPath, r.finalForm) : isInside(finalPath, r.finalForm)) return true;
    }
    return false;
}

bool insideAnyPlain(const std::wstring& plainPath) {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    for (const auto& r : g_roots) {
        if (r.exact ? sameNoCase(plainPath, r.plain) : isInside(plainPath, r.plain)) return true;
    }
    return false;
}

// an allowed FOLDER itself (never removable); a granted file may be removed, the user chose it
bool isRootItself(const std::wstring& finalPath) {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    for (const auto& r : g_roots) {
        if (!r.exact && sameNoCase(finalPath, r.finalForm)) return true;
    }
    return false;
}

bool isDriveAbsolute(const std::wstring& w) {
    return w.size() >= 3 && ((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z')) && w[1] == L':' &&
           (w[2] == L'\\' || w[2] == L'/');
}

// Lexical: the absolute, normalized form of `path` (relative paths hang off the first allowed folder). Fails for
// device paths (\\?\ \\.\), drive-relative or root-relative paths, and anything with a colon after the drive (alternate
// data streams).
int normalize(const std::string& path, std::wstring& full, bool allowRelative);
int resolve(const std::string& path, std::wstring& full) {
    int st = normalize(path, full, true);
    if (st != APP_FS_OK) return st;
    return insideAnyPlain(full) ? APP_FS_OK : APP_FS_FORBIDDEN;
}

int normalize(const std::string& path, std::wstring& full, bool allowRelative) {
    if (!validInput(path)) return APP_FS_INVALID;
    std::wstring w = widen(path);
    if (w.empty()) return APP_FS_INVALID;
    if (w.compare(0, 4, L"\\\\?\\") == 0 || w.compare(0, 4, L"\\\\.\\") == 0 || w.compare(0, 4, L"//?/") == 0 ||
        w.compare(0, 4, L"//./") == 0) {
        return APP_FS_FORBIDDEN;
    }
    const bool unc = w.size() >= 2 && (w[0] == L'\\' || w[0] == L'/') && (w[1] == L'\\' || w[1] == L'/');
    if (!unc && !isDriveAbsolute(w)) {
        if (!allowRelative) return APP_FS_INVALID;
        if (w[0] == L'\\' || w[0] == L'/' || (w.size() >= 2 && w[1] == L':')) return APP_FS_FORBIDDEN;
        std::wstring base;
        {
            // relative paths hang off the FIRST folder the application allowed (never off a grant)
            std::lock_guard<std::mutex> lock(g_rootsMutex);
            for (const auto& r : g_roots) if (!r.exact && !r.grant) { base = r.plain; break; }
            if (base.empty()) return APP_FS_FORBIDDEN;
        }
        if (!base.empty() && base.back() != L'\\') base += L'\\';
        w = base + w;
    }
    // Windows silently drops a trailing dot or space from every component ("a." is "a"), so the page would get a
    // different file than the one it named: refuse such names rather than alias them.
    {
        size_t start = 0;
        while (start <= w.size()) {
            size_t end = w.find_first_of(L"\\/", start);
            if (end == std::wstring::npos) end = w.size();
            const std::wstring comp = w.substr(start, end - start);
            if (!comp.empty() && comp != L"." && comp != L".." && (comp.back() == L'.' || comp.back() == L' ')) return APP_FS_FORBIDDEN;
            start = end + 1;
        }
    }
    // a colon is only legal as the second character of a drive-absolute path
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L':' && !(i == 1 && isDriveAbsolute(w))) return APP_FS_FORBIDDEN;
    }
    std::vector<wchar_t> buf(MAX_PATH + 1);
    DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n >= buf.size()) {
        buf.resize(n + 1);
        n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    }
    if (n == 0) return APP_FS_INVALID;
    full.assign(buf.data(), n);
    if (full.compare(0, 4, L"\\\\.\\") == 0 || full.compare(0, 4, L"\\\\?\\") == 0) return APP_FS_FORBIDDEN;  // CON, NUL ...
    // strip trailing separators (not from a drive root)
    while (full.size() > 3 && full.back() == L'\\') full.pop_back();
    return APP_FS_OK;
}

bool reservedDeviceName(std::wstring name) {
    size_t dot = name.find(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    while (!name.empty() && name.back() == L' ') name.pop_back();
    for (auto& c : name) c = static_cast<wchar_t>(towupper(c));
    if (name == L"CON" || name == L"PRN" || name == L"AUX" || name == L"NUL" || name == L"CONIN$" || name == L"CONOUT$") return true;
    if (name.size() == 4 && (name.compare(0, 3, L"COM") == 0 || name.compare(0, 3, L"LPT") == 0) && name[3] >= L'1' && name[3] <= L'9') return true;
    return false;
}

// every component of a path that is going to be CREATED (write, mkdir)
bool nameOk(const std::wstring& leaf) {
    if (leaf.empty() || leaf == L"." || leaf == L"..") return false;
    for (wchar_t c : leaf) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'|' || c == L'?' || c == L'*' || c == L'/' || c == L'\\') return false;
    }
    if (leaf.back() == L'.' || leaf.back() == L' ') return false;
    return !reservedDeviceName(leaf);
}

std::wstring leafOf(const std::wstring& full) {
    size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

int mapError(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_PATHNAME: return APP_FS_NOT_FOUND;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return APP_FS_DENIED;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return APP_FS_EXISTS;
        case ERROR_DIR_NOT_EMPTY: return APP_FS_NOT_EMPTY;
        case ERROR_DIRECTORY: return APP_FS_NOT_DIR;
        default: return APP_FS_IO;
    }
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

const DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

long long unixSeconds(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

char kindOf(DWORD attrs) {
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return 'l';
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : 'f';
}

}  // namespace

int app_fs_allow(std::string dir) {
    if (!validInput(dir)) return APP_FS_INVALID;
    std::wstring w = widen(dir);
    Handle h;
    h.h = CreateFileW(w.c_str(), FILE_LIST_DIRECTORY, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!h.ok()) return mapError(GetLastError());
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_NOT_DIR;
    Root r;
    if (!finalPathOf(h.h, r.finalForm)) return APP_FS_IO;
    while (r.finalForm.size() > 7 && r.finalForm.back() == L'\\') r.finalForm.pop_back();
    r.plain = plainOf(r.finalForm);
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    g_roots.push_back(r);
    return APP_FS_OK;
}

int app_fs_clear() {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    g_roots.clear();
    return APP_FS_OK;
}

namespace {
const size_t kMaxGrants = 256;
int addGrant(const Root& r, std::string& canonical) {
    canonical = narrow(r.plain);
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    size_t n = 0;
    for (const auto& x : g_roots) {
        if (!x.grant) continue;
        ++n;
        if (x.exact == r.exact && sameNoCase(x.finalForm, r.finalForm)) return APP_FS_OK;   // already granted
    }
    if (n >= kMaxGrants) return APP_FS_DENIED;
    g_roots.push_back(r);
    return APP_FS_OK;
}
}  // namespace

int app_fs_grant_folder(std::string path, std::string& canonical) {
    std::wstring full;
    int st = normalize(path, full, false);
    if (st != APP_FS_OK) return st;
    Handle h;
    h.h = CreateFileW(full.c_str(), FILE_LIST_DIRECTORY, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!h.ok()) return mapError(GetLastError());
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_NOT_DIR;
    Root r;
    if (!finalPathOf(h.h, r.finalForm)) return APP_FS_IO;
    while (r.finalForm.size() > 7 && r.finalForm.back() == L'\\') r.finalForm.pop_back();
    r.plain = plainOf(r.finalForm);
    r.grant = true;
    return addGrant(r, canonical);
}

int app_fs_grant_file(std::string path, std::string& canonical) {
    std::wstring full;
    int st = normalize(path, full, false);
    if (st != APP_FS_OK) return st;
    Root r;
    r.exact = true;
    r.grant = true;
    Handle h;
    h.h = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h.ok()) {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return APP_FS_IS_DIR;
        if (!finalPathOf(h.h, r.finalForm)) return APP_FS_IO;
    } else {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) return mapError(e);
        // a file that does not exist yet (a "save as" target): its folder must, and the name must be an ordinary one
        const std::wstring leaf = leafOf(full);
        if (!nameOk(leaf)) return APP_FS_FORBIDDEN;
        std::wstring parent = full.substr(0, full.size() - leaf.size());
        while (parent.size() > 3 && parent.back() == L'\\') parent.pop_back();
        Handle ph;
        ph.h = CreateFileW(parent.c_str(), FILE_LIST_DIRECTORY, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (!ph.ok()) return mapError(GetLastError());
        std::wstring parentFinal;
        if (!finalPathOf(ph.h, parentFinal)) return APP_FS_IO;
        r.finalForm = parentFinal + (parentFinal.back() == L'\\' ? L"" : L"\\") + leaf;
    }
    r.plain = plainOf(r.finalForm);
    return addGrant(r, canonical);
}

int app_fs_revoke_grants() {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    std::vector<Root> kept;
    for (const auto& r : g_roots) if (!r.grant) kept.push_back(r);
    g_roots.swap(kept);
    return APP_FS_OK;
}

int app_fs_resolve(std::string path, int kind, std::string& canonical) {
    std::wstring full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    Handle h;
    h.h = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!h.ok()) return mapError(GetLastError());
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin)) return APP_FS_FORBIDDEN;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
    const bool isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (kind == 1 && isDir) return APP_FS_IS_DIR;
    if (kind == 2 && !isDir) return APP_FS_NOT_DIR;
    canonical = narrow(plainOf(fin));
    return APP_FS_OK;
}

int app_fs_read(std::string path, int max_bytes, std::string& out) {
    std::wstring full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    Handle h;
    h.h = CreateFileW(full.c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!h.ok()) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            DWORD a = GetFileAttributesW(full.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_IS_DIR;
        }
        return mapError(e);
    }
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin)) return APP_FS_FORBIDDEN;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return APP_FS_IS_DIR;
    const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (max_bytes < 0 || size > static_cast<unsigned long long>(max_bytes)) return APP_FS_TOO_LARGE;
    std::string data(static_cast<size_t>(size), '\0');
    size_t got = 0;
    while (got < data.size()) {
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - got, 1u << 20));
        if (!ReadFile(h.h, &data[got], want, &chunk, nullptr)) return APP_FS_IO;
        if (chunk == 0) break;
        got += chunk;
    }
    data.resize(got);
    out = std::move(data);
    return APP_FS_OK;
}

int app_fs_write(std::string path, std::string data, int mode) {
    std::wstring full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    if (!nameOk(leafOf(full))) return APP_FS_FORBIDDEN;
    Handle h;
    bool created = false;
    if (mode == 2) {
        h.h = CreateFileW(full.c_str(), GENERIC_WRITE | DELETE, kShareAll, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        created = h.ok();
    } else {
        h.h = CreateFileW(full.c_str(), GENERIC_WRITE | DELETE, kShareAll, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        created = h.ok() && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    if (!h.ok()) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            DWORD a = GetFileAttributesW(full.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_IS_DIR;
        }
        return mapError(e);
    }
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin)) {
        if (created) {                       // it was made outside by following a link: take it back
            FILE_DISPOSITION_INFO d;
            d.DeleteFile = TRUE;
            SetFileInformationByHandle(h.h, FileDispositionInfo, &d, sizeof d);
        }
        return APP_FS_FORBIDDEN;
    }
    if (mode == 1) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h.h, zero, nullptr, FILE_END)) return APP_FS_IO;
    } else {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h.h, zero, nullptr, FILE_BEGIN)) return APP_FS_IO;
    }
    size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
        if (!WriteFile(h.h, data.data() + done, want, &wrote, nullptr) || wrote == 0) return APP_FS_IO;
        done += wrote;
    }
    if (mode != 1 && !SetEndOfFile(h.h)) return APP_FS_IO;
    return APP_FS_OK;
}

int app_fs_list(std::string dir, std::string& out) {
    std::wstring full;
    int st = resolve(dir, full);
    if (st != APP_FS_OK) return st;
    Handle h;
    h.h = CreateFileW(full.c_str(), FILE_LIST_DIRECTORY, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!h.ok()) return mapError(GetLastError());
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin)) return APP_FS_FORBIDDEN;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_NOT_DIR;
    std::wstring pattern = fin;
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) { out.clear(); return APP_FS_OK; }
        return mapError(e);
    }
    std::string result;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const unsigned long long size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        result += record(kindOf(fd.dwFileAttributes), size, unixSeconds(fd.ftLastWriteTime), narrow(name));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    out = std::move(result);
    return APP_FS_OK;
}

int app_fs_stat(std::string path, std::string& out) {
    std::wstring full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    Handle h;
    h.h = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!h.ok()) return mapError(GetLastError());
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin)) return APP_FS_FORBIDDEN;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h.h, &info)) return APP_FS_IO;
    const unsigned long long size = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    out = record(kindOf(info.dwFileAttributes), size, unixSeconds(info.ftLastWriteTime), narrow(leafOf(fin)));
    return APP_FS_OK;
}

namespace {
// creates one folder and checks, through a handle, that it really is inside an allowed folder; undoes it if not
int makeOne(const std::wstring& full) {
    if (!CreateDirectoryW(full.c_str(), nullptr)) {
        DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS) {
            DWORD a = GetFileAttributesW(full.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_EXISTS;
            return APP_FS_EXISTS;
        }
        return mapError(e);
    }
    Handle h;
    h.h = CreateFileW(full.c_str(), FILE_LIST_DIRECTORY | DELETE, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!h.ok()) return APP_FS_IO;
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin)) {
        FILE_DISPOSITION_INFO d;
        d.DeleteFile = TRUE;
        SetFileInformationByHandle(h.h, FileDispositionInfo, &d, sizeof d);
        return APP_FS_FORBIDDEN;
    }
    return APP_FS_OK;
}
}  // namespace

int app_fs_mkdir(std::string path, int parents) {
    std::wstring full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    if (!nameOk(leafOf(full))) return APP_FS_FORBIDDEN;
    if (!parents) return makeOne(full);
    // missing ancestors, outermost first, each checked as it is made
    std::vector<std::wstring> chain;
    std::wstring cur = full;
    for (;;) {
        DWORD a = GetFileAttributesW(cur.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) {
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return APP_FS_NOT_DIR;
            break;
        }
        chain.push_back(cur);
        size_t slash = cur.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash < 3) return APP_FS_NOT_FOUND;
        cur.resize(slash);
        if (!insideAnyPlain(cur)) return APP_FS_FORBIDDEN;
    }
    if (chain.empty()) return APP_FS_EXISTS;
    for (size_t i = chain.size(); i-- > 0;) {
        if (!nameOk(leafOf(chain[i]))) return APP_FS_FORBIDDEN;
        int r = makeOne(chain[i]);
        if (r != APP_FS_OK && r != APP_FS_EXISTS) return r;
    }
    return APP_FS_OK;
}

int app_fs_remove(std::string path) {
    std::wstring full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    Handle h;
    h.h = CreateFileW(full.c_str(), DELETE | FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (!h.ok()) return mapError(GetLastError());
    std::wstring fin;
    if (!finalPathOf(h.h, fin) || !insideAnyFinal(fin) || isRootItself(fin)) return APP_FS_FORBIDDEN;
    FILE_DISPOSITION_INFO d;
    d.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(h.h, FileDispositionInfo, &d, sizeof d)) return mapError(GetLastError());
    return APP_FS_OK;
}

#ifdef APP_HEADLESS
int app_fs_test_make_temp_dir(std::string& out) {
    wchar_t tmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n > MAX_PATH) return APP_FS_IO;
    static std::atomic<int> counter{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::wstring dir = std::wstring(tmp, n) + L"app-cling-fs-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(counter++);
        if (CreateDirectoryW(dir.c_str(), nullptr)) { out = narrow(dir); return APP_FS_OK; }
    }
    return APP_FS_IO;
}
int app_fs_test_remove_tree(std::string path) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(widen(path)), ec);
    return ec ? APP_FS_IO : APP_FS_OK;
}
int app_fs_test_make_dir_link(std::string link, std::string target) {
    // a directory junction: needs no privilege, unlike a symbolic link
    std::string cmd = "cmd /c mklink /J \"" + link + "\" \"" + target + "\" >nul 2>nul";
    return std::system(cmd.c_str()) == 0 ? APP_FS_OK : APP_FS_IO;
}
#endif

#else
// ===============================================================================================================
// POSIX (Linux). Same idea as the Windows half: open first, ask the kernel where the open descriptor really points,
// and only then read or write through it.
// ===============================================================================================================
namespace {

struct Root {
    std::string path;      // a realpath(), no trailing slash (except "/")
    bool exact = false;    // a granted FILE: only this very path matches
    bool grant = false;    // added by app_fs_grant_*
};
std::vector<Root> g_roots;

bool isInside(const std::string& path, const std::string& root) {
    if (path.size() < root.size() || path.compare(0, root.size(), root) != 0) return false;
    if (path.size() == root.size()) return true;
    if (root == "/") return true;
    return path[root.size()] == '/';
}

bool insideAny(const std::string& p) {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    for (const auto& r : g_roots) if (r.exact ? p == r.path : isInside(p, r.path)) return true;
    return false;
}

// an allowed FOLDER itself (never removable); a granted file may be removed, the user chose it
bool isRoot(const std::string& p) {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    for (const auto& r : g_roots) if (!r.exact && p == r.path) return true;
    return false;
}

bool fdPath(int fd, std::string& out) {
    char buf[PATH_MAX + 1];
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    ssize_t n = readlink(link.c_str(), buf, PATH_MAX);
    if (n <= 0) return false;
    out.assign(buf, static_cast<size_t>(n));
    return true;
}

struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) close(fd); }
    bool ok() const { return fd >= 0; }
};

// absolute, with "." and ".." collapsed lexically; relative paths hang off the first allowed folder
int normalize(const std::string& path, std::string& full, bool allowRelative) {
    if (!validInput(path)) return APP_FS_INVALID;
    std::string p = path;
    if (p[0] != '/') {
        if (!allowRelative) return APP_FS_INVALID;
        std::lock_guard<std::mutex> lock(g_rootsMutex);
        std::string base;
        for (const auto& r : g_roots) if (!r.exact && !r.grant) { base = r.path; break; }   // never off a grant
        if (base.empty()) return APP_FS_FORBIDDEN;
        p = base + "/" + p;
    }
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!c.empty() && c != ".") parts.push_back(c);
        i = j + 1;
    }
    full.clear();
    for (const auto& c : parts) full += "/" + c;
    if (full.empty()) full = "/";
    return APP_FS_OK;
}

int resolve(const std::string& path, std::string& full) {
    int st = normalize(path, full, true);
    if (st != APP_FS_OK) return st;
    return insideAny(full) ? APP_FS_OK : APP_FS_FORBIDDEN;
}

bool nameOk(const std::string& leaf) { return !leaf.empty() && leaf != "." && leaf != ".." && leaf.find('/') == std::string::npos; }

std::string parentOf(const std::string& full) {
    size_t s = full.find_last_of('/');
    return s == 0 ? std::string("/") : full.substr(0, s);
}
std::string leafOf(const std::string& full) { return full.substr(full.find_last_of('/') + 1); }

int mapErrno(int e) {
    switch (e) {
        case ENOENT: return APP_FS_NOT_FOUND;
        case EACCES:
        case EPERM: return APP_FS_DENIED;
        case EEXIST: return APP_FS_EXISTS;
        case ENOTEMPTY: return APP_FS_NOT_EMPTY;
        case EISDIR: return APP_FS_IS_DIR;
        case ENOTDIR: return APP_FS_NOT_DIR;
        default: return APP_FS_IO;
    }
}

char kindOfMode(mode_t m) { return S_ISLNK(m) ? 'l' : S_ISDIR(m) ? 'd' : 'f'; }

bool isExactGrant(const std::string& full) {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    for (const auto& r : g_roots) if (r.exact && r.path == full) return true;
    return false;
}

// opens the parent folder of `full` and checks it is inside an allowed folder. A file the user granted by name has no
// allowed parent; for exactly that file the parent is opened anyway (only to act on that one entry through it).
int openParent(const std::string& full, Fd& parent) {
    parent.fd = open(parentOf(full).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!parent.ok()) return mapErrno(errno);
    std::string fin;
    if (!fdPath(parent.fd, fin)) return APP_FS_FORBIDDEN;
    if (!insideAny(fin) && !isExactGrant(full)) return APP_FS_FORBIDDEN;
    return APP_FS_OK;
}

}  // namespace

int app_fs_allow(std::string dir) {
    if (!validInput(dir)) return APP_FS_INVALID;
    char buf[PATH_MAX + 1];
    if (realpath(dir.c_str(), buf) == nullptr) return mapErrno(errno);
    struct stat sb;
    if (stat(buf, &sb) != 0) return mapErrno(errno);
    if (!S_ISDIR(sb.st_mode)) return APP_FS_NOT_DIR;
    Root r;
    r.path = buf;
    while (r.path.size() > 1 && r.path.back() == '/') r.path.pop_back();
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    g_roots.push_back(r);
    return APP_FS_OK;
}

int app_fs_clear() {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    g_roots.clear();
    return APP_FS_OK;
}

namespace {
const size_t kMaxGrants = 256;
int addGrant(const Root& r, std::string& canonical) {
    canonical = r.path;
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    size_t n = 0;
    for (const auto& x : g_roots) {
        if (!x.grant) continue;
        ++n;
        if (x.exact == r.exact && x.path == r.path) return APP_FS_OK;   // already granted
    }
    if (n >= kMaxGrants) return APP_FS_DENIED;
    g_roots.push_back(r);
    return APP_FS_OK;
}
}  // namespace

int app_fs_grant_folder(std::string path, std::string& canonical) {
    std::string full;
    int st = normalize(path, full, false);
    if (st != APP_FS_OK) return st;
    char buf[PATH_MAX + 1];
    if (realpath(full.c_str(), buf) == nullptr) return mapErrno(errno);
    struct stat sb;
    if (stat(buf, &sb) != 0) return mapErrno(errno);
    if (!S_ISDIR(sb.st_mode)) return APP_FS_NOT_DIR;
    Root r;
    r.path = buf;
    while (r.path.size() > 1 && r.path.back() == '/') r.path.pop_back();
    r.grant = true;
    return addGrant(r, canonical);
}

int app_fs_grant_file(std::string path, std::string& canonical) {
    std::string full;
    int st = normalize(path, full, false);
    if (st != APP_FS_OK) return st;
    Root r;
    r.exact = true;
    r.grant = true;
    char buf[PATH_MAX + 1];
    if (realpath(full.c_str(), buf) != nullptr) {
        struct stat sb;
        if (stat(buf, &sb) != 0) return mapErrno(errno);
        if (S_ISDIR(sb.st_mode)) return APP_FS_IS_DIR;
        r.path = buf;
    } else {
        if (errno != ENOENT) return mapErrno(errno);
        // a file that does not exist yet (a "save as" target): its folder must
        const std::string leaf = leafOf(full);
        if (!nameOk(leaf)) return APP_FS_FORBIDDEN;
        if (realpath(parentOf(full).c_str(), buf) == nullptr) return mapErrno(errno);
        r.path = std::string(buf) + (std::string(buf) == "/" ? "" : "/") + leaf;
    }
    return addGrant(r, canonical);
}

int app_fs_revoke_grants() {
    std::lock_guard<std::mutex> lock(g_rootsMutex);
    std::vector<Root> kept;
    for (const auto& r : g_roots) if (!r.grant) kept.push_back(r);
    g_roots.swap(kept);
    return APP_FS_OK;
}

int app_fs_resolve(std::string path, int kind, std::string& canonical) {
    std::string full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    Fd f;
    f.fd = open(full.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (!f.ok()) return mapErrno(errno);
    std::string fin;
    if (!fdPath(f.fd, fin) || !insideAny(fin)) return APP_FS_FORBIDDEN;
    struct stat sb;
    if (fstat(f.fd, &sb) != 0) return APP_FS_IO;
    const bool isDir = S_ISDIR(sb.st_mode);
    if (kind == 1 && isDir) return APP_FS_IS_DIR;
    if (kind == 2 && !isDir) return APP_FS_NOT_DIR;
    canonical = fin;
    return APP_FS_OK;
}

int app_fs_read(std::string path, int max_bytes, std::string& out) {
    std::string full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    Fd f;
    f.fd = open(full.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (!f.ok()) return mapErrno(errno);
    std::string fin;
    if (!fdPath(f.fd, fin) || !insideAny(fin)) return APP_FS_FORBIDDEN;
    struct stat sb;
    if (fstat(f.fd, &sb) != 0) return APP_FS_IO;
    if (S_ISDIR(sb.st_mode)) return APP_FS_IS_DIR;
    if (!S_ISREG(sb.st_mode)) return APP_FS_FORBIDDEN;
    if (max_bytes < 0 || static_cast<unsigned long long>(sb.st_size) > static_cast<unsigned long long>(max_bytes)) return APP_FS_TOO_LARGE;
    std::string data(static_cast<size_t>(sb.st_size), '\0');
    size_t got = 0;
    while (got < data.size()) {
        ssize_t n = read(f.fd, &data[got], data.size() - got);
        if (n < 0) return APP_FS_IO;
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    data.resize(got);
    out = std::move(data);
    return APP_FS_OK;
}

int app_fs_write(std::string path, std::string data, int mode) {
    std::string full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    if (!nameOk(leafOf(full))) return APP_FS_FORBIDDEN;
    Fd f;
    bool created = false;
    if (mode == 2) {
        f.fd = open(full.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        created = f.ok();
    } else {
        f.fd = open(full.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);   // never block on a pipe or device
        if (!f.ok() && errno == ENOENT) {
            f.fd = open(full.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            created = f.ok();
        }
    }
    if (!f.ok()) return mapErrno(errno);
    std::string fin;
    if (!fdPath(f.fd, fin) || !insideAny(fin)) {
        if (created) unlink(full.c_str());
        return APP_FS_FORBIDDEN;
    }
    struct stat sb;
    if (fstat(f.fd, &sb) != 0) return APP_FS_IO;
    if (S_ISDIR(sb.st_mode)) return APP_FS_IS_DIR;
    if (!S_ISREG(sb.st_mode)) return APP_FS_FORBIDDEN;   // a pipe, socket or device is not a file the page may write
    if (mode != 1 && ftruncate(f.fd, 0) != 0) return APP_FS_IO;
    if (mode == 1 && lseek(f.fd, 0, SEEK_END) < 0) return APP_FS_IO;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(f.fd, data.data() + done, data.size() - done);
        if (n <= 0) return APP_FS_IO;
        done += static_cast<size_t>(n);
    }
    return APP_FS_OK;
}

int app_fs_list(std::string dir, std::string& out) {
    std::string full;
    int st = resolve(dir, full);
    if (st != APP_FS_OK) return st;
    int fd = open(full.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return mapErrno(errno);
    std::string fin;
    if (!fdPath(fd, fin) || !insideAny(fin)) { close(fd); return APP_FS_FORBIDDEN; }
    DIR* d = fdopendir(fd);
    if (d == nullptr) { close(fd); return APP_FS_IO; }
    std::string result;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat sb;
        if (fstatat(dirfd(d), e->d_name, &sb, AT_SYMLINK_NOFOLLOW) != 0) continue;
        result += record(kindOfMode(sb.st_mode), static_cast<unsigned long long>(sb.st_size), static_cast<long long>(sb.st_mtime), name);
    }
    closedir(d);
    out = std::move(result);
    return APP_FS_OK;
}

int app_fs_stat(std::string path, std::string& out) {
    std::string full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    struct stat sb;
    if (isRoot(full)) {
        if (stat(full.c_str(), &sb) != 0) return mapErrno(errno);
    } else {
        Fd parent;
        st = openParent(full, parent);
        if (st != APP_FS_OK) return st;
        if (fstatat(parent.fd, leafOf(full).c_str(), &sb, AT_SYMLINK_NOFOLLOW) != 0) return mapErrno(errno);
    }
    out = record(kindOfMode(sb.st_mode), static_cast<unsigned long long>(sb.st_size), static_cast<long long>(sb.st_mtime), leafOf(full));
    return APP_FS_OK;
}

namespace {
int makeOne(const std::string& full) {
    Fd parent;
    int st = openParent(full, parent);
    if (st != APP_FS_OK) return st;
    if (mkdirat(parent.fd, leafOf(full).c_str(), 0755) != 0) return mapErrno(errno);
    return APP_FS_OK;
}
}  // namespace

int app_fs_mkdir(std::string path, int parents) {
    std::string full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    if (!nameOk(leafOf(full))) return APP_FS_FORBIDDEN;
    if (!parents) return makeOne(full);
    std::vector<std::string> chain;
    std::string cur = full;
    for (;;) {
        struct stat sb;
        if (stat(cur.c_str(), &sb) == 0) {
            if (!S_ISDIR(sb.st_mode)) return APP_FS_NOT_DIR;
            break;
        }
        chain.push_back(cur);
        if (cur == "/") return APP_FS_NOT_FOUND;
        cur = parentOf(cur);
        if (!insideAny(cur)) return APP_FS_FORBIDDEN;
    }
    if (chain.empty()) return APP_FS_EXISTS;
    for (size_t i = chain.size(); i-- > 0;) {
        int r = makeOne(chain[i]);
        if (r != APP_FS_OK && r != APP_FS_EXISTS) return r;
    }
    return APP_FS_OK;
}

int app_fs_remove(std::string path) {
    std::string full;
    int st = resolve(path, full);
    if (st != APP_FS_OK) return st;
    if (isRoot(full)) return APP_FS_FORBIDDEN;
    Fd parent;
    st = openParent(full, parent);
    if (st != APP_FS_OK) return st;
    struct stat sb;
    if (fstatat(parent.fd, leafOf(full).c_str(), &sb, AT_SYMLINK_NOFOLLOW) != 0) return mapErrno(errno);
    if (unlinkat(parent.fd, leafOf(full).c_str(), S_ISDIR(sb.st_mode) ? AT_REMOVEDIR : 0) != 0) return mapErrno(errno);
    return APP_FS_OK;
}

#ifdef APP_HEADLESS
int app_fs_test_make_temp_dir(std::string& out) {
    char tmpl[] = "/tmp/app-cling-fs-XXXXXX";
    if (mkdtemp(tmpl) == nullptr) return APP_FS_IO;
    out = tmpl;
    return APP_FS_OK;
}
int app_fs_test_remove_tree(std::string path) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path), ec);
    return ec ? APP_FS_IO : APP_FS_OK;
}
int app_fs_test_make_dir_link(std::string link, std::string target) {
    return symlink(target.c_str(), link.c_str()) == 0 ? APP_FS_OK : APP_FS_IO;
}
#endif

#endif
