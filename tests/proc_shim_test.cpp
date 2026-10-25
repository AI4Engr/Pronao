// Tests for shim/app_proc.cpp against a real helper program (tests/proc_helper.cpp, path in $APP_PROC_ECHO).
// The interesting cases: arguments with every character a command line treats specially must arrive unchanged (there
// is no shell), a child that hangs or leaves something running is killed, output is capped, and a program found by name
// is looked up on PATH only.
#include "app_fs.h"
#include "app_proc.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int g_failed = 0;
static int g_passed = 0;
static void check(const char* name, bool ok) {
    if (ok) { ++g_passed; std::printf("PASS %s\n", name); }
    else { ++g_failed; std::printf("FAIL %s\n", name); }
}

static std::string packArgs(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& a : v) { s += a; s += '\0'; }
    return s;
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

static Result run(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd, int timeoutMs = 10000, int maxBytes = 1 << 20) {
    Result r;
    auto t0 = std::chrono::steady_clock::now();
    r.status = app_proc_run(exe, packArgs(args), cwd, timeoutMs, maxBytes, r.out, r.err, r.code, r.flags);
    r.ms = millis(t0);
    return r;
}

int main() {
    std::string helper;
    if (app_proc_test_helper(helper) != 1) { std::printf("FAIL APP_PROC_ECHO is not set\n"); return 2; }
    std::string exe;
    check("resolve the helper by absolute path", app_proc_resolve(helper, exe) == APP_PROC_OK && !exe.empty());
    std::string cwd;
    {
        std::string t;
        if (app_fs_test_make_temp_dir(t) != APP_FS_OK) { std::printf("FAIL cannot make a temp dir\n"); return 2; }
        cwd = t;
    }

    // ---- resolving ----
    std::string tmp;
    check("a missing absolute program", app_proc_resolve(cwd + "/nothing-here.exe", tmp) == APP_PROC_NOT_FOUND);
    check("a relative path with a separator", app_proc_resolve("./helper.exe", tmp) == APP_PROC_INVALID);
    check("a relative path with a folder", app_proc_resolve("bin/tool", tmp) == APP_PROC_INVALID);
    check("dot-dot", app_proc_resolve("..", tmp) == APP_PROC_INVALID);
    check("a name with a space", app_proc_resolve("git status", tmp) == APP_PROC_INVALID);
    check("a name with shell characters", app_proc_resolve("git&calc", tmp) == APP_PROC_INVALID);
    check("a name with a semicolon", app_proc_resolve("a;b", tmp) == APP_PROC_INVALID);
    check("an empty name", app_proc_resolve("", tmp) == APP_PROC_INVALID);
    check("invalid UTF-8", app_proc_resolve("\xff\xfe", tmp) == APP_PROC_INVALID);
    check("an unknown bare name", app_proc_resolve("surely-not-installed-xyz", tmp) == APP_PROC_NOT_FOUND);
    {
        // a program that sits in the CURRENT folder, but not on PATH, must not be found by name
        const std::string ghost = "ghost-program-for-the-test";
#ifdef _WIN32
        std::ofstream f(ghost + ".exe", std::ios::binary);
#else
        std::ofstream f(ghost, std::ios::binary);
#endif
        f << "x";
        f.close();
        check("the current folder is not searched", app_proc_resolve(ghost, tmp) == APP_PROC_NOT_FOUND);
        std::remove((ghost + ".exe").c_str());
        std::remove(ghost.c_str());
    }
#ifdef _WIN32
    check("a .bat is refused", app_proc_resolve("C:\\Windows\\System32\\whatever.bat", tmp) == APP_PROC_INVALID);
    check("a .cmd is refused", app_proc_resolve("C:\\x\\y.cmd", tmp) == APP_PROC_INVALID);
    check("a stream name is refused", app_proc_resolve("C:\\Windows\\System32\\cmd.exe:s", tmp) == APP_PROC_INVALID);
    check("a device path is refused", app_proc_resolve("\\\\.\\C:\\x.exe", tmp) == APP_PROC_INVALID);
    check("a bare name is found on PATH", app_proc_resolve("where", tmp) == APP_PROC_OK && tmp.size() > 4);
#else
    check("a bare name is found on PATH", app_proc_resolve("ls", tmp) == APP_PROC_OK && tmp[0] == '/');
#endif

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
        check("awkward arguments arrive unchanged", r.status == APP_PROC_OK && r.code == 0 && asciiOut == asciiExp);
#else
        check("awkward arguments arrive unchanged", r.status == APP_PROC_OK && r.code == 0 && r.out == expected);
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
        check("exit code", r.status == APP_PROC_OK && r.code == 7 && !(r.flags & 1));
    }
    {
        Result r = run(exe, {"--stderr", "oops"}, cwd);
        check("stderr is separate from stdout", r.status == APP_PROC_OK && r.err == "oops" && r.out.empty());
    }
    {
        Result r = run(exe, {"--stdin"}, cwd);
        std::string o;
        for (char c : r.out) if (c != '\r') o += c;
        check("the child gets no standard input", r.status == APP_PROC_OK && o == "stdin bytes: 0\n");
    }
    {
        Result r = run(exe, {"--cwd"}, cwd);
        std::string leaf = cwd.substr(cwd.find_last_of("/\\") + 1);
        check("the working folder is the one given", r.status == APP_PROC_OK && r.out.find(leaf) != std::string::npos);
    }
    {
        Result r = run(exe, {"--env", "PATH"}, cwd);
        check("the environment is inherited", r.status == APP_PROC_OK && r.out.size() > 3 && r.out.find("(unset)") == std::string::npos);
    }

    // ---- limits ----
    {
        Result r = run(exe, {"--sleep", "20000"}, cwd, 400);
        check("a program that takes too long is killed", r.status == APP_PROC_OK && (r.flags & 1) && r.code == -1 && r.ms < 6000);
    }
    {
        Result r = run(exe, {"--sleep", "100"}, cwd, 5000);
        check("a program within the limit is left alone", r.status == APP_PROC_OK && !(r.flags & 1) && r.out.find("woke") != std::string::npos);
    }
    {
        Result r = run(exe, {"--leave-sleeper"}, cwd, 20000);
        check("something left running behind it is killed too", r.status == APP_PROC_OK && r.code == 0 &&
              r.out.find("left a sleeper") != std::string::npos && r.ms < 10000);
    }
    {
        Result r = run(exe, {"--big", "1000000"}, cwd, 10000, 1000);
        check("output is capped", r.status == APP_PROC_OK && r.out.size() == 1000 && (r.flags & 2) && !(r.flags & 4));
    }
    {
        Result r = run(exe, {"--big", "5000"}, cwd, 10000, 100000);
        check("output under the cap is whole", r.status == APP_PROC_OK && r.out.size() == 5000 && !(r.flags & 2));
    }
    {
        Result r = run(exe, {"--big", "3000000"}, cwd, 20000, 0);
        check("a cap of zero discards but still drains", r.status == APP_PROC_OK && r.out.empty() && (r.flags & 2) && r.code == 0);
    }

    // ---- refused input ----
    {
        Result r;
        r.status = app_proc_run(exe, "no terminator", cwd, 1000, 1000, r.out, r.err, r.code, r.flags);
        check("arguments must be NUL terminated", r.status == APP_PROC_INVALID);
        r.status = app_proc_run(exe, std::string("\xff\0", 2), cwd, 1000, 1000, r.out, r.err, r.code, r.flags);
        check("an argument that is not UTF-8", r.status == APP_PROC_INVALID);
        std::vector<std::string> many(300, "x");
        r.status = app_proc_run(exe, packArgs(many), cwd, 1000, 1000, r.out, r.err, r.code, r.flags);
        check("too many arguments", r.status == APP_PROC_INVALID);
        r.status = app_proc_run(exe, packArgs({}), "relative/dir", 1000, 1000, r.out, r.err, r.code, r.flags);
        check("a relative working folder", r.status == APP_PROC_INVALID);
        r.status = app_proc_run(exe, packArgs({}), cwd + "/no-such-folder", 1000, 1000, r.out, r.err, r.code, r.flags);
        check("a missing working folder", r.status == APP_PROC_NOT_FOUND);
        r.status = app_proc_run(cwd + "/missing-program.exe", packArgs({}), cwd, 1000, 1000, r.out, r.err, r.code, r.flags);
        check("a missing program", r.status == APP_PROC_NOT_FOUND);
        r.status = app_proc_run("relative-program.exe", packArgs({}), cwd, 1000, 1000, r.out, r.err, r.code, r.flags);
        check("a relative program", r.status == APP_PROC_INVALID);
    }

    // ---- output that is not text ----
    {
        Result r = run(exe, {"--bytes", "41ff42"}, cwd);
        std::string cleaned;
        int replaced = app_proc_clean_text(r.out, cleaned);
        check("raw bytes survive the capture", r.status == APP_PROC_OK && r.out.size() >= 3 && r.out[1] == '\xff');
        check("ill-formed bytes become U+FFFD", replaced == 1 && cleaned.find("A\xEF\xBF\xBD" "B") != std::string::npos);
        std::string same;
        check("clean text is left alone", app_proc_clean_text("caf\xc3\xa9", same) == 0 && same == "caf\xc3\xa9");
        std::string trunc;
        check("a truncated sequence", app_proc_clean_text("x\xe2\x82", trunc) == 1 && app_fs_is_utf8(trunc) == 1);
        std::string overlong;
        check("an overlong encoding", app_proc_clean_text("\xc0\xaf", overlong) == 1 && app_fs_is_utf8(overlong) == 1);
    }

    // ---- many runs in a row ----
    {
        int good = 0;
        for (int i = 0; i < 100; ++i) {
            Result r = run(exe, {"n"}, cwd);
            if (r.status == APP_PROC_OK && r.code == 0 && r.out.find("[1]=n") != std::string::npos) ++good;
        }
        check("100 runs in a row", good == 100);
    }

    app_fs_test_remove_tree(cwd);
    std::printf("proc shim test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
