// Tests for the native process runner (shim/{win,posix}/proc.cpp) against a real helper program (tests/proc_helper.cpp, path in
// $APP_PROC_ECHO). The interesting cases: arguments with every character a command line treats specially must arrive unchanged (there
// is no shell), a child that hangs or leaves something running is killed, and output is capped. Which programs may run, and the
// checks on names and arguments, are CeeLing's (tests/programs_main.clg and headless_main.clg).
#include "../shim/app_native.h"
#include "doubles/test_support.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

static int g_failed = 0;
static int g_passed = 0;
static void check(const char* name, bool ok) {
    if (ok) { ++g_passed; std::printf("PASS %s\n", name); }
    else { ++g_failed; std::printf("FAIL %s\n", name); }
}

static long long millis(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

struct Result {
    int status = -1;
    std::string out, err;
    int code = -99, flags = 0;
    long long ms = 0;
};

// the environment a test child gets: exactly these (CeeLing decides which; here just enough for the helper to start)
static std::vector<std::string> baseEnv() {
    std::vector<std::string> env;
    for (const char* name : {"PATH", "SystemRoot", "SYSTEMDRIVE", "TEMP", "TMP", "LD_LIBRARY_PATH"}) {
        std::string value;
        if (app_os_env(name, value) == 1) env.push_back(std::string(name) + "=" + value);
    }
    return env;
}

static Result run(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, int timeoutMs = 10000, int maxBytes = 1 << 20,
                  std::vector<std::string> env = baseEnv(), int handle = 0, const std::string& call = "") {
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    r.status = app_proc_run(handle, call, exe, args, cwd, env, timeoutMs, maxBytes, r.out, r.err, r.code, r.flags);
    r.ms = millis(t0);
    return r;
}

int main() {
    std::string exe;
    if (app_test_helper(exe) != 1) { std::printf("FAIL APP_PROC_ECHO is not set\n"); return 2; }
    std::string cwd;
    if (app_test_make_temp_dir(cwd) != 0) { std::printf("FAIL cannot make a temp dir\n"); return 2; }

    // ---- arguments arrive exactly as given (there is no shell) ----
    {
        std::vector<std::string> awkward = {
            "plain", "with space", "with \"quotes\"", "ends with backslash\\", "two\\\\", "\\\"", "a&b", "a|b", "a>b", "a<b",
            "%PATH%", "^caret", "semi;colon", "$HOME", "`tick`", "*.txt", "caf\xc3\xa9 \xe2\x82\xac", "-leading-dash", ""};
        Result r = run(exe, awkward, cwd);
        std::string expected;
        for (size_t i = 0; i < awkward.size(); ++i) expected += "[" + std::to_string(i + 1) + "]=" + awkward[i] + "\n";
#ifdef _WIN32
        // the C runtime prints through the console code page for non-ASCII text; compare only the ASCII lines there
        std::string asciiOut, asciiExp;
        for (char c : r.out) if (static_cast<unsigned char>(c) < 128 && c != '\r') asciiOut += c;
        for (char c : expected) if (static_cast<unsigned char>(c) < 128) asciiExp += c;
        check("awkward arguments arrive unchanged", r.status == 0 && r.code == 0 && asciiOut == asciiExp);
#else
        check("awkward arguments arrive unchanged", r.status == 0 && r.code == 0 && r.out == expected);
#endif
    }
    {
        Result r = run(exe, {"a b", "c"}, cwd);
        std::string o;
        for (char c : r.out) if (c != '\r') o += c;
        check("a program receives separate arguments", o == "[1]=a b\n[2]=c\n");
    }

    // ---- results ----
    {
        Result r = run(exe, {"--exit", "7"}, cwd);
        check("exit code", r.status == 0 && r.code == 7 && !(r.flags & 1));
    }
    {
        Result r = run(exe, {"--stderr", "oops"}, cwd);
        check("stderr is separate from stdout", r.status == 0 && r.err == "oops" && r.out.empty());
    }
    {
        Result r = run(exe, {"--stdin"}, cwd);
        std::string o;
        for (char c : r.out) if (c != '\r') o += c;
        check("the child gets no standard input", r.status == 0 && o == "stdin bytes: 0\n");
    }
    {
        Result r = run(exe, {"--cwd"}, cwd);
        std::string leaf = cwd.substr(cwd.find_last_of("/\\") + 1);
        check("the working folder is the one given", r.status == 0 && r.out.find(leaf) != std::string::npos);
    }
    {
        app_test_setenv("APP_CLING_SECRET", "hunter2");
        Result secret = run(exe, {"--env", "APP_CLING_SECRET"}, cwd);
        check("a variable of the parent is NOT inherited", secret.status == 0 && secret.out.find("hunter2") == std::string::npos && secret.out.find("(unset)") != std::string::npos);
        Result path = run(exe, {"--env", "PATH"}, cwd);
        check("a variable that was passed arrives", path.status == 0 && path.out.size() > 3 && path.out.find("(unset)") == std::string::npos);
        std::vector<std::string> withOne = baseEnv();
        withOne.push_back("ONLY_THIS=value with = and spaces");
        Result given = run(exe, {"--env", "ONLY_THIS"}, cwd, 10000, 1 << 20, withOne);
        check("an explicit variable arrives exactly", given.status == 0 && given.out.find("value with = and spaces") != std::string::npos);
        Result none = run(exe, {"--env", "ONLY_THIS"}, cwd);
        check("and is absent when not given", none.status == 0 && none.out.find("(unset)") != std::string::npos);
    }

    // ---- limits ----
    {
        Result r = run(exe, {"--sleep", "20000"}, cwd, 400);
        check("a program that takes too long is killed", r.status == 0 && (r.flags & 1) && r.code == -1 && r.ms < 6000);
    }
    {
        Result r = run(exe, {"--sleep", "100"}, cwd, 5000);
        check("a program within the limit is left alone", r.status == 0 && !(r.flags & 1) && r.out.find("woke") != std::string::npos);
    }
    {
        Result r = run(exe, {"--leave-sleeper"}, cwd, 20000);
        check("something left running behind it is killed too", r.status == 0 && r.code == 0 && r.out.find("left a sleeper") != std::string::npos && r.ms < 10000);
    }
    {
        Result r = run(exe, {"--big", "1000000"}, cwd, 10000, 1000);
        check("output is capped", r.status == 0 && r.out.size() == 1000 && (r.flags & 2) && !(r.flags & 4));
    }
    {
        Result r = run(exe, {"--big", "5000"}, cwd, 10000, 100000);
        check("output under the cap is whole", r.status == 0 && r.out.size() == 5000 && !(r.flags & 2));
    }
    {
        Result r = run(exe, {"--big", "3000000"}, cwd, 20000, 0);
        check("a cap of zero discards but still drains", r.status == 0 && r.out.empty() && (r.flags & 2) && r.code == 0);
    }

    // ---- cancelling ----
    {
        std::thread later([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); app_cancel(7, "call-a"); });
        Result r = run(exe, {"--sleep", "20000"}, cwd, 20000, 1 << 20, baseEnv(), 7, "call-a");
        later.join();
        check("a cancelled call kills its program within a moment", r.status == 0 && (r.flags & 8) && !(r.flags & 1) && r.code == -1 && r.ms < 4000);
        Result other = run(exe, {"--sleep", "100"}, cwd, 5000, 1 << 20, baseEnv(), 7, "call-b");
        check("another call of the same window is not affected", other.status == 0 && !(other.flags & 8) && other.out.find("woke") != std::string::npos);
        check("a call that ended can be forgotten", app_cancelled(7, "call-a") == 1 && app_cancel_forget(7, "call-a") == 1 && app_cancelled(7, "call-a") == 0);
        app_cancel_all(8);
        Result closing = run(exe, {"--sleep", "20000"}, cwd, 20000, 1 << 20, baseEnv(), 8, "later-call");
        check("when the window is closing a call that starts afterwards is killed at once", closing.status == 0 && (closing.flags & 8) && closing.ms < 4000);
        Result untouched = run(exe, {"--sleep", "100"}, cwd, 5000, 1 << 20, baseEnv(), 9, "x");
        check("other windows are not affected", untouched.status == 0 && !(untouched.flags & 8));
    }

    // ---- refused input ----
    {
        // (what is fit to pass -- absolute paths, no NUL, NAME=value, limits -- is decided and tested in CeeLing: toolRunOk, headless_main.clg)
        Result r;
        r.status = app_proc_run(0, "", exe, {}, cwd + "/no-such-folder", baseEnv(), 1000, 1000, r.out, r.err, r.code, r.flags);
        check("a missing working folder", r.status == 2);
        r.status = app_proc_run(0, "", cwd + "/missing-program.exe", {}, cwd, baseEnv(), 1000, 1000, r.out, r.err, r.code, r.flags);
        check("a missing program", r.status == 2);
    }

    // ---- the PATH lookup helpers CeeLing uses ----
    {
        std::string value;
        check("an environment variable is readable", app_os_env("PATH", value) == 1 && !value.empty());
        check("an unset one is reported", app_os_env("APP_CLING_SURELY_UNSET_VARIABLE", value) == 0);
        check("a folder is a folder", app_os_path_kind(cwd) == 2);
        check("the helper is a runnable file", (app_os_path_kind(exe) & 1) == 1);
        check("a missing path is nothing", app_os_path_kind(cwd + "/nothing-here") == 0);
    }

    // ---- many runs in a row ----
    {
        int good = 0;
        for (int i = 0; i < 100; ++i) {
            Result r = run(exe, {"n"}, cwd);
            if (r.status == 0 && r.code == 0 && r.out.find("[1]=n") != std::string::npos) ++good;
        }
        check("100 runs in a row", good == 100);
    }

    app_test_remove_tree(cwd);
    std::printf("proc shim test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
