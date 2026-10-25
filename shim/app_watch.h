// app-cling -- watching a folder for changes. Trusted C++, like the rest of the shim.
//
// A watcher is a thread owned by the shim. When files change it puts a record in the same queue the page's calls arrive
// in, with an EMPTY id (a page's own calls always have one), and the CeeLing main loop turns the record into an event for
// the page; no JSON is built here. A record is NUL-terminated fields:  "watch" id kind path from
//   kind  "created" | "modified" | "removed" | "renamed" | "overflow" (events were lost: rescan) | "stopped" (the watch is over;
//         `path` says why: "closed", "access_revoked", "folder_gone", "failed")
//   path  relative to the watched folder, with / separators; never an absolute path, so an event cannot reveal where the
//         watched folder is. For "renamed" it is the new name and `from` the old one; otherwise `from` is empty.
// Rapid repeats of the same change are merged (a program saving a file touches it many times in a few milliseconds).
//
// The folder must be one the file commands can reach (see app_fs.h). The permission is re-checked before every delivery,
// so withdrawing it (app_fs_revoke_grants, app_fs_clear) ends the watch with "access_revoked" and nothing more is sent.
// Changes made through a link that leads out of the folder are not reported.
#pragma once
#include <string>

// Starts a watch on `path` (an existing folder); `watch_id` is set on success. At most 32 watches per window
// (APP_FS_DENIED beyond). Returns an AppFsStatus.
int app_watch_start(int handle, std::string path, int recursive, int& watch_id);

// Ends one watch / every watch of the window. The thread has stopped when these return; nothing is delivered afterwards.
int app_watch_stop(int handle, int watch_id);
int app_watch_stop_all(int handle);
