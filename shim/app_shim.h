// app-cling -- the C++ shim between CeeLing and the system WebView. This file and app_shim.cpp are the
// whole trusted native layer (together with webview/webview and the browser engine it drives).
//
// The shim holds NO application logic. It owns three things:
//   * a UI thread that creates the window and runs the message loop (WebView2 requires the thread that
//     creates the window to be the one that pumps its messages);
//   * a mutex-protected queue of calls arriving from JavaScript;
//   * a handle table, so CeeLing sees small integers and never a pointer.
// CeeLing runs the loop: app_next_call() blocks until a call (or the window closing), CeeLing dispatches it
// and answers with app_return(). Callbacks therefore never cross into CeeLing, which cannot receive one.
//
// Everything crossing this boundary is an int or a string, copied. Every function is safe to call from any
// thread. Return conventions: 1 = ok / a call arrived, 0 = failure or closed, 2 = timed out.
#pragma once
#include <string>

// Opens a window running `html`, with `init_js` executed before every page's own scripts, and starts the
// UI thread. Returns a handle (> 0), or 0 if the window could not be created.
int app_open(std::string title, int width, int height, std::string html, std::string init_js, int debug);

// Waits up to `timeout_ms` (negative = forever) for the next call from JavaScript. On 1, `id` is the opaque
// reply token and `request` is the JSON array of the call's arguments, e.g. ["add",[2,3]].
// 0 = the window has closed and no call is left; 2 = timed out.
int app_next_call(int handle, int timeout_ms, std::string& id, std::string& request);

// Answers a call. status 0 resolves the JavaScript promise with `json` (a JSON text), anything else rejects it.
int app_return(int handle, std::string id, int status, std::string json);

// Runs `js` in the page (posted to the UI thread). Used to push events to the frontend.
int app_eval(int handle, std::string js);

// Asks the message loop to stop; the window closes. Safe from any thread.
int app_terminate(int handle);

// Stops the window if still open, joins the UI thread and frees the handle.
int app_close(int handle);

#ifdef APP_HEADLESS
// Test-only entry points (headless build): feed a call as if JavaScript had made it, and read back what
// CeeLing answered with. Not available in the window build.
int app_test_inject(int handle, std::string id, std::string request);
int app_test_take_response(int handle, std::string& id, int& status, std::string& json);
int app_test_close_window(int handle);
#endif

// Sleeps the calling thread. Exists so CeeLing code (which has no sleep of its own) can make a background
// job take a known time, e.g. in tests that check replies arriving out of order.
int app_sleep_ms(int ms);

// A concurrency gauge for tests: jobs call app_gauge_enter / app_gauge_leave around their work, and
// app_gauge_peak() is the highest number that were inside at the same time (so a worker-limit test can tell
// "at most N ran at once" from "N happened to be enough").
int app_gauge_enter();
int app_gauge_leave();
int app_gauge_peak();
