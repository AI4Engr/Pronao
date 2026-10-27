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
