// What the tests need that the product does not: declared here for C++ tests; headless_main.clg declares the ones it calls as ext_fn.
#pragma once
#include <string>

#include <condition_variable>
#include <deque>
#include <mutex>

int app_test_inject(int handle, std::string id, std::string request);
int app_test_take_response(int handle, std::string& id, int& status, std::string& json);
int app_test_response_ready(int handle);                           // 1 if an answer is waiting (does not take it)
int app_test_push_internal(int handle, std::string request);       // a record from the native layer (empty id), as a watcher or a dialog worker sends
int app_test_take_eval(int handle, std::string& js);              // pops the oldest script passed to app_eval; 0 if none
int app_test_take_window_log(int handle, std::string& line);     // pops the oldest window-control call
int app_test_close_window(int handle);

// files, outside any policy
int app_test_make_temp_dir(std::string& out);
int app_test_remove_tree(std::string path);
int app_test_write_raw(std::string path, std::string data);
int app_test_mkdir_raw(std::string path);
int app_test_read_raw(std::string path, std::string& out);        // 1 if it could be read
int app_test_exists(std::string path);
int app_test_make_dir_link(std::string link, std::string target);  // a junction on Windows, a symlink elsewhere

// dialogs: the next answer is scripted ("cancel", "path:<text>", "button:<1-4>"); app_dialog_* then return it
int app_test_dialog_push(std::string answer);
int app_test_dialog_take_request(std::string& request);
// what app_instance_claim answers (first, status) and the command line app_command_line gives (arguments each followed by a NUL); the claim and the
// listen calls are logged as "instance|claim|NAME" and "instance|listen|HANDLE" for app_test_dialog_take_request
int app_test_instance(int first, int status, std::string commandLine);
// what app_monitors answers: left, top, right, bottom of each monitor's work area, every number followed by a NUL ("" = no monitor can be found)
int app_test_monitors(std::string fields);
// a click on a menu item: queues the record "menu" id on the window, as the native layer does (the window log shows menu|set| and menu|item| calls)
int app_test_menu_click(int handle, std::string id);
// what app_window_rect answers ("x y width height state"); the style, drag and modal calls show up in the window log (style|WHAT|ON, drag|EDGE, modal|OWNER|ON)
int app_test_window_rect(std::string rect);
// what app_runtime_version answers (0 installed with this version, 1 not installed, -1 nothing needed) and what a Yes / No message box answers (1 / 0); the boxes are
// logged as "box|ASK|TITLE|TEXT" for app_test_dialog_take_request
int app_test_runtime(int status, std::string version, int boxAnswer);
// the item the next pop-up menu "chooses" ("" = it is dismissed); the pop-up itself shows up in the window log as menu|popup|SPEC
int app_test_popup_answer(std::string id);
// a click on the tray icon (kind "click" or "double") as the native layer queues it; the tray's own calls show up in its window log (tray|open|, tray|update|, tray|item|)
int app_test_tray_event(int handle, std::string kind);
// the user clicked the notification that was shown with this id: queues the record "notify" id on the tray (or notifier) handle
int app_test_notification_click(int handle, std::string id);

// the colour scheme: what app_theme_system answers (1 dark); app_test_theme_changed also queues the record "window" "theme" 0|1 on the window. The window log shows theme|MODE|SYSTEM
// (app_window_theme) and drop|ON (app_drop_accept). app_test_drop queues what the native layer queues for a drag: kind enter | over | leave | files, the pointer, the paths each followed by a NUL
int app_test_system_theme(int dark);
int app_test_theme_changed(int handle, int dark);
int app_test_drop(int handle, std::string kind, int x, int y, std::string paths);

// a program for the process tests to run (the environment variable APP_PROC_ECHO); 0 if unset
int app_test_helper(std::string& path);

// the environment of this (test) process
int app_test_setenv(std::string name, std::string value);

// what the window would be given for an address (shim/assets.h): 1 if the address is the site's, with the status, the header text and the body
int app_test_fetch(std::string method, std::string url, int& status, std::string& headers, std::string& body);

// threads
int app_test_sleep_ms(int ms);
int app_test_gauge_enter();
int app_test_gauge_leave();
int app_test_gauge_peak();
