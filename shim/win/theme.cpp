// Windows: the system's colour scheme, the title bar's, and the page's (what the CSS media query prefers-color-scheme answers). See app_theme_system / app_window_theme in app_native.h.
#include "../app_native.h"
#include "../app_shim_internal.h"
#include <WebView2.h>
#include "util.h"

int app_theme_system() {
    DWORD light = 1, size = sizeof light;
    // 0 = the user wants dark applications; the value is missing on a Windows that has no such setting, and then it is light
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS) return 0;
    return light == 0 ? 1 : 0;
}

void app_theme_titlebar(void* nativeWindow, bool dark) {
    // DwmSetWindowAttribute from dwmapi.dll, looked up here so that the program does not need to link it (and still starts on a Windows that lacks it)
    static auto set = reinterpret_cast<HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD)>(
        [] { HMODULE m = LoadLibraryW(L"dwmapi.dll"); return m ? reinterpret_cast<void*>(GetProcAddress(m, "DwmSetWindowAttribute")) : nullptr; }());
    if (!set || !nativeWindow) return;
    const BOOL value = dark ? TRUE : FALSE;
    // attribute 20 is DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1 and later, Windows 11); 19 is what builds before it called it
    if (FAILED(set(static_cast<HWND>(nativeWindow), 20, &value, sizeof value))) set(static_cast<HWND>(nativeWindow), 19, &value, sizeof value);
}

int app_theme_page(void* controller, int mode) {
    ICoreWebView2* view = nullptr;
    if (!controller || FAILED(static_cast<ICoreWebView2Controller*>(controller)->get_CoreWebView2(&view)) || !view) return 4;
    ICoreWebView2_13* v13 = nullptr;                                   // (the profile is reached from here on; an older runtime does not have the interface)
    if (FAILED(view->QueryInterface(IID_ICoreWebView2_13, reinterpret_cast<void**>(&v13))) || !v13) return 4;
    ICoreWebView2Profile* profile = nullptr;
    int rc = 4;
    if (SUCCEEDED(v13->get_Profile(&profile)) && profile) {
        const COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme = mode == 1 ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT
                                                          : mode == 2 ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
        if (SUCCEEDED(profile->put_PreferredColorScheme(scheme))) rc = 0;
        profile->Release();
    }
    v13->Release();
    return rc;
}
