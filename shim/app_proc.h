// app-cling -- running an external program on behalf of the application. Trusted C++, like the rest of the shim.
//
// This file only RUNS what it is told to run, safely: no shell is ever involved (the program and its arguments are
// separate strings, never one command line that something re-splits), the child gets no stdin, no console window and
// only the handles it needs, it is killed together with everything it started when the time is up (a job object on
// Windows, a process group elsewhere), and what it prints is capped. WHICH programs may run, and with what arguments,
// is decided one level up, in CeeLing code, from a table the application builds (see allowProgram in src/app.clg): a
// web page never supplies a program name or a command line, only values for the slots of an entry in that table.
//
// Windows note: the arguments are joined into one command line with the C runtime's quoting rules, and a program that
// parses its command line differently (cmd.exe, and so every .bat / .cmd, msiexec, ...) could read them differently, so
// app_proc_resolve refuses anything that is not a plain .exe.
#pragma once
#include <string>

enum AppProcStatus {
    APP_PROC_OK = 0,
    APP_PROC_NOT_FOUND = 2,    // no such program / folder
    APP_PROC_SPAWN_FAILED = 4, // the operating system refused to start it
    APP_PROC_INVALID = 5       // an argument that cannot be passed (not UTF-8, too many), or a name that is not allowed
};

// Resolves a program to an absolute path. `name` is either an absolute path to a .exe, or a bare name ("git") that is
// searched for on PATH only (never the current folder, never the folder of the running program), with .exe appended
// on Windows. Anything with a path separator that is not absolute, or not a plain .exe on Windows, is refused.
int app_proc_resolve(std::string name, std::string& absolute);

// Runs `exe` (from app_proc_resolve) with the arguments in `args`, each one followed by a NUL, in folder `cwd` (absolute,
// must exist). Waits at most `timeout_ms` (then the child and everything it started are killed); a child that has exited
// but left something running that still holds its output is given one second, then the leftovers are killed too.
// Output beyond `max_bytes` per stream is read and discarded. Both streams are returned as raw bytes.
// `exit_code` is the child's, or -1 if it was killed for taking too long.
// `flags` bit 0: timed out; bit 1: stdout was cut at max_bytes; bit 2: stderr was cut at max_bytes.
int app_proc_run(std::string exe, std::string args, std::string cwd, int timeout_ms, int max_bytes,
                 std::string& out, std::string& err, int& exit_code, int& flags);

// A program's output is not always UTF-8, and it travels to the page inside a JSON string. This copies `in` with every
// ill-formed byte replaced by U+FFFD. Returns 1 if anything was replaced.
int app_proc_clean_text(std::string in, std::string& out);

#ifdef APP_HEADLESS
// Test-only: the path of the helper program that the tests run (the environment variable APP_PROC_ECHO). 0 if unset.
int app_proc_test_helper(std::string& path);
#endif
