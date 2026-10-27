// app-cling -- everything CeeLing calls in the native layer. Trusted C++ with no policy and no formats of its own: it opens, reads,
// runs, shows and waits; deciding what is allowed, parsing, validating and bookkeeping are in src/. Everything crossing is an int or a
// copied string (or a vector of them); every function is safe to call from any thread. Status codes: 0 ok, 1 forbidden, 2 not found, 3 too large, 4 io error, 5 invalid, 6 exists, 7 not empty, 8 is a folder,
// 9 not a folder, 10 permission denied, 11 too many.
#pragma once
#include <string>
#include <vector>

// ---- the window (app_shim.cpp) --------------------------------------------------------------------------------------
// A window runs `html`, with `init_js` executed before the page's own scripts, on its own UI thread. Returns a handle (> 0) or 0.
int app_open(std::string title, int width, int height, std::string html, std::string init_js, int debug);
// The next call from the page, or a record the shim queued itself (id empty). 1: got one (`request` is a JSON array text), 0: the
// window closed and nothing is left, 2: timed out (timeout_ms < 0 waits forever).
int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request);
int app_return(int handle, std::string id, int status, std::string json);   // status 0 resolves the page's promise, else rejects it
int app_eval(int handle, std::string js);                                   // runs a script in the page
int app_push(int handle, std::string id, std::string request, int max_queued);  // queues a record for app_next_call (id empty = the
                                                                                 // shim's own, never a page's); 1, 0 if max_queued wait, -1 no window
int app_terminate(int handle);                                              // ends the message loop; the window closes
int app_close(int handle);                                                  // terminates if needed, joins the UI thread, frees the handle
// Window control (Windows; -1 elsewhere). state: 0 restore, 1 minimize, 2 maximize, 3 bring to front. guard 1: closing the window
// (X, Alt+F4) no longer closes it but queues the record "window" "close_requested" for the main loop.
int app_window_title(int handle, std::string title);
int app_window_size(int handle, int width, int height);
int app_window_state(int handle, int state);
int app_window_guard(int handle, int on);
// Limits on what a page may send: a call over `max_request` bytes or arriving while `max_queued` wait is answered at once with
// {code: too_large} / {code: busy} and never reaches CeeLing. Defaults 16 MiB and 1000.
int app_limits(int handle, int max_request, int max_queued);
// After the application's own page, only addresses starting with one of these may load (and never a new window). Default: none.
int app_navigation_allow(int handle, std::vector<std::string> prefixes);

// ---- files: open first, then ask where the open handle really points (`fin`, as the OS spells it), then act on it -----------
// mode: 0 read a file | 1 a folder, to list | 2 attributes only, following a link | 3 attributes only, NOT following a final link
// | 4 as 3, with the right to delete | 5 write (create if missing, no truncation; `created` says whether it did)
// | 6 create, failing with 6 if it exists. A handle that cannot say where it points is refused (1).
int app_os_windows();   // 1 on Windows, else 0 (path syntax is CeeLing's job; this only says which)
int app_fs_open(std::string path, int mode, int& h, std::string& fin, int& created);
int app_fs_close(int h);
// kind is a character code: 'd' folder, 'f' regular file, 'l' link, 'o' anything else; decimal size; seconds since 1970.
int app_fs_info(int h, int& kind, std::string& size, std::string& mtime);
int app_fs_read(int h, int max_bytes, std::string& out);                  // 3 if the file is larger than max_bytes (not read)
int app_fs_write(int h, std::string data, int append);                    // replaces the contents unless `append`
int app_fs_list(int h, std::string& out);                                 // per entry, NUL terminated: kind letter, size, mtime, name
int app_fs_mkdir(std::string path);                                       // one folder; 6 if the name is taken
int app_fs_delete(int h);                                                 // a file, a link or an EMPTY folder (7 otherwise)

// ---- programs and the environment -----------------------------------------------------------------------------------------
double app_now_ms();                                                      // milliseconds since the process started (monotonic)
int app_os_env(std::string name, std::string& out);                       // 1 and the value, or 0 if unset
int app_os_path_kind(std::string path);                                   // 0 nothing, 1 file (+4 executable), 2 folder
// `exe` and `cwd` absolute, `args` already vetted, `env` the child's WHOLE environment as NAME=value (nothing is inherited). 0 ran, 2 no such program or folder, 4 could not start, 5 cannot be passed.
// flags: 1 timed out (everything it started is killed), 2 stdout / 4 stderr cut at max_bytes (still drained).
int app_proc_run(std::string exe, std::vector<std::string> args, std::string cwd, std::vector<std::string> env, int timeout_ms,
                 int max_bytes, std::string& out, std::string& err, int& exit_code, int& flags);

// ---- dialogs: shown on the window's UI thread, the calling thread waits, so call from a worker --------------------------------
// mode 0 open a file, 1 save as, 2 pick a folder. `filters` = label, pattern, label, pattern ... ("*.txt;*.md"). 1 chose (path set),
// 0 cancelled, -1 could not be shown (no window, or not supported on this platform).
int app_dialog_pick(int handle, int mode, std::string title, std::string default_name, std::string default_ext, std::vector<std::string> filters,
                    std::string& path);
// kind 0 info 1 warning 2 error 3 question; buttons 0 ok, 1 ok+cancel, 2 yes+no. 1 ok, 2 cancel, 3 yes, 4 no; -1 not shown.
int app_dialog_message(int handle, std::string title, std::string message, int kind, int buttons);

// ---- watching a folder: a thread per watch queues records "watch" id kind path from (empty id) for the main loop ---------------
// kind: created, modified, removed, renamed (path is the new name), overflow (records were lost), stopped (path says why).
// Paths are relative to `root`, with / separators. `root` must be vetted by the caller. The thread has ended when stop returns.
int app_watch_start(int handle, std::string root, int recursive, int& watch_id);
int app_watch_stop(int handle, int watch_id);
int app_watch_stop_all(int handle);
