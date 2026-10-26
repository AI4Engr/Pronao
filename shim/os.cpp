// The clock CeeLing code reads (see app_native.h).
#include <chrono>

double app_now_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
