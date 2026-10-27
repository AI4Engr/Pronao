// Windows: UTF-8 <-> UTF-16 for the native files.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

// An empty result means the text was not valid UTF-8.
inline std::wstring widen(const std::string& u8) {
    const int n = u8.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    const int n = w.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// Closes the handle when it goes out of scope (an invalid handle counts as none).
struct Handle {
    HANDLE h = nullptr;
    Handle() {}
    explicit Handle(HANDLE x) : h(x == INVALID_HANDLE_VALUE ? nullptr : x) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset() { if (h) CloseHandle(h); h = nullptr; }
    HANDLE* put() { reset(); return &h; }
    operator HANDLE() const { return h; }
};
