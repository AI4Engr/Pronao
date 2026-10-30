// POSIX: hand an address or a path to xdg-open (see app_native.h). What may be opened is decided in src/links.clg. No shell is involved: the program is
// started with the target as its one argument, and a target that starts with "-" is refused so it cannot be read as an option.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "../app_native.h"

int app_shell_open(std::string target, int mode) {
    if (target.empty() || target[0] == '-' || target.find('\0') != std::string::npos) return 5;
    if (mode == 1) {                                          // "show it": open the folder it is in (a folder opens itself)
        const size_t cut = target.find_last_of('/');
        if (cut != std::string::npos && cut > 0) target = target.substr(0, cut);
    }
    const pid_t first = fork();
    if (first < 0) return 4;
    if (first == 0) {
        if (fork() != 0) _exit(0);                            // (the grandchild is not ours to wait for)
        setsid();
        execlp("xdg-open", "xdg-open", target.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    waitpid(first, &status, 0);
    return 0;                                                 // (whether xdg-open found a handler is not known here)
}
