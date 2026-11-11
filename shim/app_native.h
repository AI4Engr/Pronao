// pronao -- everything CeeLing calls in the native layer. Trusted C++ with no policy and no formats of its own: it opens, reads,
// runs, shows and waits; deciding what is allowed, parsing, validating and bookkeeping are in src/. Everything crossing is an int or a
// copied string (or a vector of them); every function is safe to call from any thread. Status codes: 0 ok, 1 forbidden, 2 not found, 3 too large, 4 io error, 5 invalid, 6 exists, 7 not empty, 8 is a folder,
// 9 not a folder, 10 permission denied, 11 too many.
#pragma once
#include <string>
#include <vector>

// ---- the window (app_shim.cpp) --------------------------------------------------------------------------------------
// A window runs `html` (or, if `url` is not empty, loads that address: see the site calls below), with `init_js` executed before the page's own scripts, on its own UI thread. Returns a handle (> 0) or 0.
int app_open(std::string title, int width, int height, std::string html, std::string url, std::string init_js, int debug);
// The next call from the page, or a record the shim queued itself (id empty). 1: got one (`request` is a JSON array text), 0: the
// window closed and nothing is left, 2: timed out (timeout_ms < 0 waits forever).
int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request);
// Waits for ANY of these windows to have a call or record: 1 and `handle` = that window is ready (then app_next_call(handle, 0, ...) takes it; it returns 0 once the
// window has closed and been reported), 2 timed out, 0 none of them has anything more to say (gone, or closed and already reported). The loop that serves several windows.
int app_wait_any(std::vector<int> handles, int timeout_ms, int& handle);
int app_return(int handle, std::string id, int status, std::string json);   // status 0 resolves the page's promise, else rejects it. A page call
                                                                            // is answered ONCE: 1 if this answer was delivered, 0 if the call was
                                                                            // already answered (a timeout got there first), is unknown, or the window is gone
int app_eval(int handle, std::string js);                                   // runs a script in the page
int app_push(int handle, std::string id, std::string request, int max_queued);  // queues a record for app_next_call (id empty = the
                                                                                 // shim's own, never a page's); 1, 0 if max_queued wait, -1 no window
int app_terminate(int handle);
int app_close(int handle);                                                  // terminates if needed, joins the UI thread, frees the handle
// Window control (Windows; -1 elsewhere). state: 0 restore, 1 minimize, 2 maximize, 3 bring to front, 5 hide (no taskbar button either), 6 show again and bring to front. guard 1: closing the window
// (X, Alt+F4) no longer closes it but queues the record "window" "close_requested" for the main loop.
int app_window_title(int handle, std::string title);
int app_window_size(int handle, int width, int height);
int app_window_state(int handle, int state);     // 3 also brings a minimized window back
int app_window_guard(int handle, int on);
// Where windows may be. app_monitors: the work area (the screen without the taskbar) of every monitor as left, top, right, bottom, each number followed by a NUL,
// the primary monitor first; 4 if none can be found, -1 not supported. app_window_placement: puts the window's RESTORED rectangle at x y width height and
// maximizes it if asked (Windows; -1 elsewhere). The caller has fitted it to the monitors. app_window_track(on): from now on, after the user moves or resizes
// the window (or maximizes / restores it) the record "window" "placement" x y width height maximized is queued for the main loop (the restored rectangle).
int app_monitors(std::string& out);
// Layout (Windows; -1 elsewhere). app_window_rect: "x y width height state" in screen pixels (the window as it is, frame included); state is a sum: 1 maximized, 2 minimized,
// 4 full screen, 8 always on top, 16 no title bar, 32 not resizable, 64 hidden. app_window_style: what 1 full screen (no frame, over the whole monitor the window is on;
// off restores what it was), 2 always on top, 3 no title bar (the resize border stays if the window may be resized), 4 the user may resize and maximize it;
// on 1 / 0. app_window_drag: starts the system's move (edge 0) or resize (edge 10 left, 11 right, 12 top, 13 top left, 14 top right, 15 bottom, 16 bottom left, 17 bottom right)
// loop for the window, and only while the left mouse button is held (otherwise nothing happens): for a window with no title bar the page calls this on mouse down.
// app_window_modal: while `child` exists the window `owner` cannot be used (it is disabled; it is enabled again when the child closes, or with on 0).
int app_window_rect(int handle, std::string& out);
int app_window_style(int handle, int what, int on);
int app_window_drag(int handle, int edge);
int app_window_modal(int child, int owner, int on);
// Menus (Windows; -1 elsewhere). app_menu_set: the window's menu bar from the flat spec src/menu.clg writes ("" removes it); 0 ok, 5 the spec is malformed, 4 no such window.
// Clicking an item, or pressing its shortcut while the page has the keyboard, queues the record "menu" id on the window; the id is always one the spec contained.
// app_menu_item: ticks / unticks (checked 1 / 0) and enables / greys (enabled 1 / 0) an item of the menu bar, -1 leaves a state alone; 5 if the id is not in it.
// The tray (notification area) icon, Windows only (-1 elsewhere). app_tray_open: an icon (the program's own, ICON= in build.sh, else the system's) with a tooltip (at
// most 127 bytes: the caller checked) and a right-click menu from the flat spec of src/menu.clg; `handle` is the tray's own handle in the same table as the windows,
// with only a queue: app_wait_any and app_next_call serve it like a window. Its records: "tray" click | double (a left click, a double click) and "menu" id (an item of
// the tray menu was chosen). app_close(handle) removes the icon. 0 ok, 4 could not, 5 the menu spec is malformed.
// app_tray_update: a new tooltip ("" = leave it) and / or a new menu spec ("" = leave it). app_tray_item: as app_menu_item, for the tray menu.
int app_tray_open(std::string tooltip, std::string menu, int& handle);
int app_tray_update(int handle, std::string tooltip, std::string menu);
// Notifications (Windows; -1 elsewhere): a balloon / toast from the tray icon, `kind` 0 info, 1 warning, 2 error; title at most 63 and text at most 255 characters (the caller
// checked), shown unless the user's quiet time (Focus Assist) is on. 0 the shell took it (not that it was shown), 4 it did not, 5 too long. `id` comes back with the record "notify" id
// when the user clicks THIS notification. app_notifier_open: a tray icon with no menu that is in the notification area only while a notification is up (and for a few seconds after);
// it has a handle like a tray. The icon of a tray opened with app_tray_open stays.
int app_tray_notify(int handle, std::string title, std::string text, int kind, std::string id);
int app_notifier_open(std::string tooltip, int& handle);
int app_tray_item(int handle, std::string id, int enabled, int checked);
// app_menu_popup: a pop-up menu from a flat spec at the mouse, on the window's UI thread; the calling thread waits until it is closed (call it from a worker, like a
// dialog). `chosen` is the id of the item chosen, "" if the menu was dismissed; a chosen item is also queued as the record "menu" id on the window. 0 ok, 5 the spec is
// malformed, 4 no such window. -1 elsewhere.
int app_menu_popup(int handle, std::string spec, std::string& chosen);
int app_menu_set(int handle, std::string spec);
int app_menu_item(int handle, std::string id, int enabled, int checked);
// The system's colour scheme (Windows; -1 elsewhere): app_theme_system is 0 for light and 1 for dark (what the user chose for applications in Settings > Colours).
// app_window_theme: how the window looks: mode 0 follow the system, 1 always light, 2 always dark. It sets the title bar and what the page's CSS media query
// prefers-color-scheme answers (the browser engine keeps that per profile, and all windows of a program share one: the last call wins for the page). From the first call
// on, a change of the system's scheme queues the record "window" "theme" 0|1 on the window (once per change). 0 ok, 4 no such window, or this browser runtime cannot
// set the page's scheme (the title bar was set), 5 not a mode, -1 not supported.
int app_theme_system();
int app_window_theme(int handle, int mode);
// Files dragged from the shell onto the window (Windows; -1 elsewhere). app_drop_accept(handle, 1): the window's page is told that dropped files matter (window.__pronao_drop, which
// src/bridge.js looks at), and the paths of the files it hands the browser engine are queued as the record "drop" "files" x y path... (x and y are the pointer in the page's CSS
// pixels; at most 512 paths, vetted by the application, not here). app_drop_accept(handle, 0) turns it off. 0 ok, 4 could not (no such window, or the runtime lacks the interface), 5 not 0 / 1.
int app_drop_accept(int handle, int on);
int app_window_placement(int handle, int x, int y, int width, int height, int maximized);
int app_window_track(int handle, int on);
int app_release_build();                                                          // 1 if built with RELEASE=1 (no development server, no developer tools)
// Limits on what a page may send: a call over `max_request` bytes or arriving while `max_queued` wait is answered at once with
// {code: too_large} / {code: busy} and never reaches CeeLing. Defaults 16 MiB and 1000.
int app_limits(int handle, int max_request, int max_queued);
// After the application's own page, only addresses starting with one of these may load (and never a new window). Default: none.
int app_navigation_allow(int handle, std::vector<std::string> prefixes);

// ---- the application's own web files, served to the window from memory (Windows). Set up BEFORE app_open ----------------------
// The window then loads its pages from `origin` (like "http://app.localhost"): that origin is answered from this table, never from the
// network, and is allowed to navigate in. `csp`, if not empty, is sent as Content-Security-Policy with every HTML file. An address that
// was not added is a 404. Paths start with "/". Files compiled into the program (scripts/build.sh ASSETS=dir) are added by index.
int app_asset_site(std::string origin, std::string csp);
int app_embedded_count();
int app_embedded_name(int index, std::string& name);                         // relative path, forward slashes
int app_asset_add_embedded(std::string path, std::string mime, int index);
int app_asset_add_text(std::string path, std::string mime, std::string text);
int app_asset_alias(std::string from, std::string to);                       // `from` answers with what `to` is

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
int app_fs_replace(std::string from, std::string to);                     // renames `from` to `to`, replacing a file already there in one step (readers see the old or the new one, never half)

// ---- programs and the environment -----------------------------------------------------------------------------------------
int app_sleep_ms(int ms);                                                 // pauses the calling thread (a job polling for a cancel, a retry loop)
double app_now_ms();                                                      // milliseconds since the process started (monotonic)
int app_os_env(std::string name, std::string& out);                       // 1 and the value, or 0 if unset
int app_os_path_kind(std::string path);                                   // 0 nothing, 1 file (+4 executable), 2 folder
// `exe` and `cwd` absolute, `args` already vetted, `env` the child's WHOLE environment as NAME=value (nothing is inherited). 0 ran, 2 no such program or folder, 4 could not start, 5 cannot be passed.
// flags: 1 timed out (everything it started is killed), 2 stdout / 4 stderr cut at max_bytes (still drained), 8 CANCELLED: app_cancel(handle, call)
// was called for `call` (or the window is closing), the child and everything it started were killed, the output so far is returned.
int app_proc_run(int handle, std::string call, std::string exe, std::vector<std::string> args, std::string cwd, std::vector<std::string> env,
                 int timeout_ms, int max_bytes, std::string& out, std::string& err, int& exit_code, int& flags);

// ---- cancelling: the registry only remembers; blocking primitives that are given a call id poll it ----------------------------------
int app_cancel(int handle, std::string call);          // asks for the call to stop (a page's AbortSignal, a timeout)
int app_cancel_all(int handle);                        // the window is closing: every call of it, now and later
int app_cancel_forget(int handle, std::string call);   // the call is over; its request is no longer needed
int app_cancelled(int handle, std::string call);       // 1 if `call` was cancelled or the window is closing: a long job checks this between steps
int app_pending(int handle, std::string call);         // 1 if the page is still waiting for an answer to `call`

// ---- dialogs: shown on the window's UI thread, the calling thread waits, so call from a worker --------------------------------
// mode 0 open a file, 1 save as, 2 pick a folder. `filters` = label, pattern, label, pattern ... ("*.txt;*.md"). 1 chose (path set),
// 0 cancelled, -1 could not be shown (no window, or not supported on this platform).
int app_dialog_pick(int handle, int mode, std::string title, std::string default_name, std::string default_ext, std::vector<std::string> filters,
                    std::string& path);
// kind 0 info 1 warning 2 error 3 question; buttons 0 ok, 1 ok+cancel, 2 yes+no. 1 ok, 2 cancel, 3 yes, 4 no; -1 not shown.
int app_dialog_message(int handle, std::string title, std::string message, int kind, int buttons);

// ---- one running copy per user (Windows; the POSIX answers are "not supported", -1) ------------------------------------------------------------
// app_instance_claim: `name` is a plain name. first = 1 if this process is now THE instance with that name (it keeps the claim until it ends); 0 if
// another one holds it, and then this process's arguments and working folder were handed to that one (and it was allowed to take the focus).
// Returns 0 ok, 4 another one holds it but could not be told (it may be starting up or hanging), -1 not supported here (first is 1: nothing is enforced).
int app_instance_claim(std::string name, int& first);
// From now on what later launches hand over is queued on window `handle` as the record "instance", then the second process's working folder and each of
// its arguments, every field followed by a NUL (what arrived earlier is queued now). At most 16 are kept before this is called; one message is at most 64 KiB.
int app_instance_listen(int handle);
// The arguments this process was started with, without the program name, each followed by a NUL.
int app_command_line(std::string& out);

// ---- the system the program runs on --------------------------------------------------------------------------------------------------------------
// app_runtime_version: Windows: 0 and the version of the WebView2 runtime if it is installed, 1 if it is not (then no window can be opened); elsewhere -1 (nothing is needed).
// app_message_box: a message box that needs no window of ours (a program that cannot open its window has nothing else to tell the user with); ask 0: an OK button, 1: Yes / No.
// 1 for OK or Yes, 0 for No, -1 if it could not be shown. Blocks until the user answers. The text was vetted by the caller.
int app_runtime_version(std::string& version);
int app_message_box(std::string title, std::string text, int ask);

// ---- the shell: an address in the default browser, a file in its default program, a file shown in the file manager -----------------------------
// mode 0 open `target` (an http, https or mailto address, or the absolute path of a file), 1 show the file (or folder) in the file manager. 0 asked the
// system to do it (not that it succeeded: a program starts on its own), 4 could not, 5 the target is not acceptable. The caller has vetted the target.
int app_shell_open(std::string target, int mode);

// ---- watching a folder: a thread per watch queues records "watch" id kind path from (empty id) for the main loop ---------------
// kind: created, modified, removed, renamed (path is the new name), overflow (records were lost), stopped (path says why).
// Paths are relative to `root`, with / separators. `root` must be vetted by the caller. The thread has ended when stop returns.
int app_watch_start(int handle, std::string root, int recursive, int& watch_id);
int app_watch_stop(int handle, int watch_id);
