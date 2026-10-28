// The clock CeeLing code reads (see app_native.h).
#include <chrono>
#include <thread>

#include "app_native.h"
#include "cancel.h"

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
