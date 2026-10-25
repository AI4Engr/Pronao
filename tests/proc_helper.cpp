// A small program for the tests of shim/app_proc.cpp: it reports how it was started and does what the arguments say.
//   (no option)         print every argument as  [i]=<text>  on stdout
//   --exit N            exit with code N
//   --stderr TEXT       write TEXT to stderr
//   --sleep MS          sleep, then print "woke"
//   --big N             write N bytes of 'x' to stdout
//   --bytes HEX         write the bytes given in hex (e.g. 41ff42) to stdout
//   --cwd               print the current folder
//   --stdin             read standard input and print how many bytes arrived
//   --env NAME          print the value of an environment variable
//   --leave-sleeper     start a copy of this program that sleeps for a minute (holding our output), then exit at once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

static void pause(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--exit") == 0) return std::atoi(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--stderr") == 0) { std::fputs(argv[2], stderr); return 0; }
    if (argc >= 3 && std::strcmp(argv[1], "--sleep") == 0) { pause(std::atoi(argv[2])); std::puts("woke"); return 0; }
    if (argc >= 3 && std::strcmp(argv[1], "--big") == 0) {
        long n = std::atol(argv[2]);
        std::string block(65536, 'x');
        while (n > 0) {
            size_t k = static_cast<size_t>(n < 65536 ? n : 65536);
            std::fwrite(block.data(), 1, k, stdout);
            n -= static_cast<long>(k);
        }
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--bytes") == 0) {
        const char* h = argv[2];
        for (size_t i = 0; h[i] != '\0' && h[i + 1] != '\0'; i += 2) {
            char pair[3] = {h[i], h[i + 1], 0};
            std::fputc(static_cast<int>(std::strtol(pair, nullptr, 16)), stdout);
        }
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--cwd") == 0) {
#ifdef _WIN32
        char buf[MAX_PATH * 2];
        _getcwd(buf, sizeof buf);
#else
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof buf) == nullptr) buf[0] = '\0';
#endif
        std::puts(buf);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--stdin") == 0) {
        long total = 0;
        char buf[256];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) total += static_cast<long>(n);
        std::printf("stdin bytes: %ld\n", total);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--env") == 0) {
        const char* v = std::getenv(argv[2]);
        std::printf("%s\n", v ? v : "(unset)");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--leave-sleeper") == 0) {
#ifdef _WIN32
        char self[MAX_PATH * 2];
        GetModuleFileNameA(nullptr, self, sizeof self);
        std::string cmd = std::string("\"") + self + "\" --sleep 60000";
        STARTUPINFOA si = {};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi = {};
        CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
#else
        if (fork() == 0) { execl(argv[0], argv[0], "--sleep", "60000", static_cast<char*>(nullptr)); _exit(1); }
#endif
        std::puts("left a sleeper");
        std::fflush(stdout);
        return 0;
    }
    for (int i = 1; i < argc; ++i) std::printf("[%d]=%s\n", i, argv[i]);
    return 0;
}
