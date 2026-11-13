// Everything CeeLing calls in the native layer: trusted C++ with no policy and no formats of its own (what is allowed, parsing and validation are in src/).
// Arguments and results are ints or copied strings (or vectors of them); any thread may call any function. "Windows only" calls return -1 on POSIX.
// File status codes: 0 ok, 1 forbidden, 2 not found, 3 too large, 4 io error, 5 invalid, 6 exists, 7 not empty, 8 is a folder, 9 not a folder, 10 permission denied, 11 too many.
#pragma once
#include <string>
#include <vector>

// ---- the window (app_shim.cpp) --------------------------------------------------------------------------------------
// Opens a window on its own UI thread showing `html`, or loading `url` if not empty; `init_js` runs before the page's scripts. Handle > 0, or 0.
int app_open(std::string title, int width, int height, std::string html, std::string url, std::string init_js, int debug);
// Next page call, or a record the shim queued itself (id empty). 1 got one (`request` is a JSON array), 0 closed and drained, 2 timed out (timeout_ms < 0: forever).
int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request);
// Waits for any of `handles` to have something: 1 and `handle` set (take it with app_next_call(handle, 0, ...)), 2 timed out, 0 none has anything more to say.
int app_wait_any(std::vector<int> handles, int timeout_ms, int& handle);
// Status 0 resolves the page's promise, else rejects it. A call is answered once: 1 delivered, 0 already answered (a timeout got there first), unknown or window gone.
int app_return(int handle, std::string id, int status, std::string json);
int app_eval(int handle, std::string js);                                   // runs a script in the page; 1 done, 0 no window
// Queues a record for app_next_call (empty id = the shim's own, never a page's). 1, 0 if max_queued are waiting, -1 no window.
int app_push(int handle, std::string id, std::string request, int max_queued);
int app_terminate(int handle);
int app_close(int handle);                                                  // terminates if needed, stops its watches, joins the UI thread, frees the handle
// Window control (Windows only). state: 0 restore, 1 minimize, 2 maximize, 3 bring to front, 4 close as the X would (the guard is asked), 5 hide (no taskbar button),
// 6 show and bring to front. guard 1: the X / Alt+F4 queues the record "window" "close_requested" instead of closing.
int app_window_title(int handle, std::string title);
int app_window_size(int handle, int width, int height);
int app_window_state(int handle, int state);
int app_window_guard(int handle, int on);
// Work area (screen minus taskbar) of every monitor as left, top, right, bottom, each number NUL terminated, primary first; 4 if none found. Windows only.
int app_monitors(std::string& out);
// Windows only. app_window_rect: "x y width height state" in screen pixels, frame included; state sums 1 maximized, 2 minimized, 4 full screen, 8 always on top,
// 16 no title bar, 32 not resizable, 64 hidden. 0 ok, 4 no such window. app_window_style and app_window_drag: 1 done, else no such window (app_window_modal: 0 ok, 4).
int app_window_rect(int handle, std::string& out);
// what: 1 full screen (off restores), 2 always on top, 3 no title bar (the resize border stays if resizable), 4 the user may resize and maximize; on 1 / 0.
int app_window_style(int handle, int what, int on);
// Starts the system move (edge 0) or resize (10 left, 11 right, 12 top, 13 top left, 14 top right, 15 bottom, 16 bottom left, 17 bottom right), only while the left mouse button is held.
int app_window_drag(int handle, int edge);
int app_window_modal(int child, int owner, int on);                         // while `child` exists `owner` is disabled (enabled again when it closes, or on 0)
// Tray icon (Windows only). app_tray_open: the program's icon, a tooltip (at most 127 bytes: the caller checks) and a right-click menu spec. `handle` has only a queue, served by
// app_wait_any / app_next_call like a window; app_close removes the icon. Records: "tray" click | double, "menu" id. 0 ok, 4 could not, 5 malformed spec.
// app_tray_update: new tooltip and / or menu spec ("" = leave it). 0 ok, 4 no such tray, 5 malformed spec.
// app_tray_notify: a balloon / toast from the icon; kind 0 info, 1 warning, 2 error; title at most 63, text at most 255 characters (the caller checks); not shown during
// Focus Assist. 0 the shell took it (not that it was shown), 4 it did not, 5 too long. A click on it queues the record "notify" id.
// app_notifier_open: a tray icon with no menu, in the notification area only while a notification is up (15 s at most).
int app_tray_open(std::string tooltip, std::string menu, int& handle);
int app_tray_update(int handle, std::string tooltip, std::string menu);
int app_tray_notify(int handle, std::string title, std::string text, int kind, std::string id);
int app_notifier_open(std::string tooltip, int& handle);
// Menus (Windows only). A click, or a shortcut while the page has the keyboard (the page does not get the key), queues the record "menu" id; ids are always the spec's own.
// app_menu_item / app_tray_item: tick (checked) and enable (enabled) an item, 1 / 0, -1 leaves it; 0 ok, 5 no such id (4: no such tray).
// app_menu_popup: a pop-up at the mouse, on the window's UI thread; blocks until closed, so call it from a worker. `chosen` is the id picked or "" (a pick is also queued
// as "menu" id). 0 ok, 5 malformed spec, 4 no such window. app_menu_set: the menu bar from the flat spec of src/menu.clg ("" removes it); 0 ok, 5 malformed spec, 4 no such window.
int app_tray_item(int handle, std::string id, int enabled, int checked);
int app_menu_popup(int handle, std::string spec, std::string& chosen);
int app_menu_set(int handle, std::string spec);
int app_menu_item(int handle, std::string id, int enabled, int checked);
// Colour scheme (Windows only). app_theme_system: 0 light, 1 dark. app_window_theme: mode 0 follow the system, 1 light, 2 dark; sets the title bar and the page's
// prefers-color-scheme (per browser profile, shared by all windows: the last call wins). After the first call a change of the system scheme queues "window" "theme" 0|1.
// 0 ok, 4 no such window or the runtime cannot set the page's scheme (the title bar is set), 5 not a mode.
int app_theme_system();
int app_window_theme(int handle, int mode);
// Dropped files (Windows only). on 1: the page is told drops matter (window.__pronao_drop, read by src/bridge.js) and the paths of dropped files are queued as the record
// "drop" "files" x y path... (x y in CSS pixels, at most 512 paths, the application vets them). 0 ok, 4 no such window or the runtime lacks the interface, 5 not 0 / 1.
int app_drop_accept(int handle, int on);
// Places the RESTORED rectangle (screen coordinates), maximizing if asked (Windows only). app_window_track(on): after the user moves, resizes, maximizes or restores the
// window, queues "window" "placement" x y width height maximized (only Windows reports).
int app_window_placement(int handle, int x, int y, int width, int height, int maximized);
int app_window_track(int handle, int on);
int app_release_build();                                                          // 1 if built with RELEASE=1 (no development server, no developer tools)
// A call over `max_request` bytes, or arriving while `max_queued` wait, is answered at once with {code: too_large} / {code: busy} and never reaches CeeLing. Defaults 16 MiB, 1000.
int app_limits(int handle, int max_request, int max_queued);
// After the application's own page only addresses starting with one of these may load, and never a new window. Default: none.
int app_navigation_allow(int handle, std::vector<std::string> prefixes);

// ---- the application's own web files, served from memory (Windows). Set up BEFORE app_open ----------------------
// The window loads from `origin` ("http://app.localhost"), answered from this table and never the network; `csp`, if not empty, goes with every HTML file. An address not
// added is a 404. Paths start with "/". Files compiled in (scripts/build.sh ASSETS=dir) are added by index.
int app_asset_site(std::string origin, std::string csp);
int app_embedded_count();
int app_embedded_name(int index, std::string& name);                         // relative path, forward slashes
int app_asset_add_embedded(std::string path, std::string mime, int index);
int app_asset_add_text(std::string path, std::string mime, std::string text);
int app_asset_alias(std::string from, std::string to);                       // `from` answers with what `to` is

// ---- files: open, ask where the handle really points (`fin`, as the OS spells it), then act on that handle -----------
// mode: 0 read a file | 1 a folder, to list | 2 attributes only, following a link | 3 attributes only, NOT following a final link | 4 as 3, with the right to delete
// | 5 write (create if missing, no truncation; `created` says whether it did) | 6 create, failing with 6 if it exists. A handle that cannot say where it points: 1.
int app_os_windows();   // 1 on Windows, else 0 (path syntax is CeeLing's job)
int app_fs_open(std::string path, int mode, int& h, std::string& fin, int& created);
int app_fs_close(int h);
// kind is a character code: 'd' folder, 'f' regular file, 'l' link, 'o' anything else; decimal size; seconds since 1970.
int app_fs_info(int h, int& kind, std::string& size, std::string& mtime);
int app_fs_read(int h, int max_bytes, std::string& out);                  // 3 if the file is larger than max_bytes (not read)
int app_fs_write(int h, std::string data, int append);                    // replaces the contents unless `append`
int app_fs_list(int h, std::string& out);                                 // per entry, NUL terminated: kind letter, size, mtime, name
int app_fs_mkdir(std::string path);                                       // one folder; 6 if the name is taken
int app_fs_delete(int h);                                                 // a file, a link or an EMPTY folder (7 otherwise)
int app_fs_replace(std::string from, std::string to);                     // renames, replacing a file already there

// ---- programs and the environment -----------------------------------------------------------------------------------------
int app_sleep_ms(int ms);
double app_now_ms();                                                      // milliseconds since the process started (monotonic)
int app_os_env(std::string name, std::string& out);                       // 1 and the value, or 0 if unset
int app_os_path_kind(std::string path);                                   // 0 nothing, 1 file (+4 executable), 2 folder
// `exe` and `cwd` absolute, `args` vetted, `env` the child's WHOLE environment as NAME=value (nothing inherited). 0 ran, 2 no such program or folder, 4 could not start.
// flags: 1 timed out, 2 stdout / 4 stderr cut at max_bytes (still drained), 8 cancelled (app_cancel for `call`, or the window closing). On 1 and 8 the child and everything
// it started are killed and the output so far is returned.
int app_proc_run(int handle, std::string call, std::string exe, std::vector<std::string> args, std::string cwd, std::vector<std::string> env,
                 int timeout_ms, int max_bytes, std::string& out, std::string& err, int& exit_code, int& flags);

// ---- HTTP: one request, sent from a worker; the caller waits ----------------------------------------------------------------
// `url` and `method` have been vetted by src/http.clg; `headers` are "Name: value" lines. No redirect is followed, no cookie kept, no credentials sent on their own.
// 0 answered (status_code, resp_headers as "Name: value" lines each ended by a newline, resp_body); 2 could not connect or resolve; 3 TLS failed; 4 the address or a header is
// not one the system accepts; 5 not implemented on this platform; 6 other. flags: 1 timed out (timeout_ms for the whole request), 2 the body was cut at max_bytes, 8 cancelled
// (app_cancel for `call`, or the window closing); the request is aborted on 1 and 8.
int app_http_request(int handle, std::string call, std::string method, std::string url, std::vector<std::string> headers, std::string body,
                     int timeout_ms, int max_bytes, int& status_code, std::string& resp_headers, std::string& resp_body, int& flags);

// ---- cancelling: the registry only remembers; blocking primitives given a call id poll it ----------------------------------
int app_cancel(int handle, std::string call);          // asks for the call to stop (a page's AbortSignal, a timeout)
int app_cancel_all(int handle);                        // the window is closing: every call of it, including ones that start afterwards
int app_cancel_forget(int handle, std::string call);   // the call is over
int app_cancelled(int handle, std::string call);       // 1 if `call` was cancelled or the window is closing: a long job checks this between steps
int app_pending(int handle, std::string call);         // 1 if the page is still waiting for an answer to `call`

// ---- dialogs: shown on the window's UI thread and the caller waits, so call from a worker --------------------------------
// mode 0 open a file, 1 save as, 2 pick a folder. `filters` = label, pattern, label, pattern ... ("*.txt;*.md"). 1 chose (path set), 0 cancelled, -1 could not be shown.
int app_dialog_pick(int handle, int mode, std::string title, std::string default_name, std::string default_ext, std::vector<std::string> filters,
                    std::string& path);
// kind 0 info 1 warning 2 error 3 question; buttons 0 ok, 1 ok+cancel, 2 yes+no. 1 ok, 2 cancel, 3 yes, 4 no; -1 not shown.
int app_dialog_message(int handle, std::string title, std::string message, int kind, int buttons);

// ---- one running copy per user (Windows only; POSIX: -1, nothing enforced) ------------------------------------------------------------
// `name`: 1 to 64 letters, digits, `.`, `_`, `-`. first = 1 if this process is now THE instance (until it ends); 0 if another holds it, and then this process's arguments and
// working folder were handed to that one. 0 ok, 4 another holds it but could not be told, 5 bad name or already claimed, -1 not supported (first is 1).
int app_instance_claim(std::string name, int& first);
// Later launches are queued on window `handle` as the record "instance", working folder, then each argument, every field NUL terminated; earlier ones (at most 16 kept) are
// queued by this call. A message is at most 64 KiB.
int app_instance_listen(int handle);
int app_command_line(std::string& out);                                   // the arguments without the program name, each NUL terminated (also on POSIX)

// ---- the system the program runs on --------------------------------------------------------------------------------------------------------------
// Windows: 0 and the WebView2 runtime's version, 1 if it is not installed (no window can then be opened); elsewhere -1 (nothing needed).
int app_runtime_version(std::string& version);
// A box that needs no window (for a program that cannot open one); ask 0 OK, 1 Yes / No. 1 OK or Yes, 0 No, -1 not shown. Blocks until answered; the caller vetted the text.
int app_message_box(std::string title, std::string text, int ask);

// ---- the shell ----------------------------------------------------------------------------------------------------------------------------------
// mode 0 open `target` (an http, https or mailto address, or an absolute file path) with the system, 1 show the file or folder in the file manager. 0 asked the system (not that
// it succeeded), 4 could not, 5 target not acceptable. The caller vetted the target.
int app_shell_open(std::string target, int mode);

// ---- watching a folder: a thread per watch queues the records "watch" id kind path from (empty id) ---------------
// kind: created, modified, removed, renamed (path is the new name), overflow (records were lost), stopped (path says why). Paths are relative to `root`, with / separators;
// the caller vets `root`. app_watch_start returns once the OS is watching; the thread has ended when app_watch_stop returns.
int app_watch_start(int handle, std::string root, int recursive, int& watch_id);
int app_watch_stop(int handle, int watch_id);
