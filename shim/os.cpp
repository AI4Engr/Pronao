// Release flag, clock, sleep and the cancel registry (see app_native.h).
#include <chrono>
#include <thread>

#include "app_native.h"
#include "cancel.h"

// 1 in a RELEASE=1 build (-DAPP_RELEASE): App.openDevServer in src/app.clg then refuses a development server
int app_release_build() {
#ifdef APP_RELEASE
    return 1;
#else
    return 0;
#endif
}

double app_now_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

int app_cancel(int handle, std::string call) { cancelRegistry().cancel(handle, call); return 1; }
int app_cancel_all(int handle) { cancelRegistry().cancelAll(handle); return 1; }
int app_cancel_forget(int handle, std::string call) { cancelRegistry().forget(handle, call); return 1; }
int app_cancelled(int handle, std::string call) { return cancelRegistry().cancelled(handle, call) ? 1 : 0; }
int app_sleep_ms(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 1;
}
