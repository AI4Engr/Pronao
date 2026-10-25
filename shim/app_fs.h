// app-cling -- file access for pages, with the folder allow-list enforced HERE, in trusted C++.
//
// Why the policy lives in the shim and not in CeeLing: whether a path is inside an allowed folder cannot be decided
// from the text of the path (`..`, symlinks, junctions, case-insensitive volumes, `\\?\` prefixes, 8.3 names). It has
// to be decided on what the operating system actually opened, and between a check and an open the answer can change.
// So every operation here opens first, asks the OS for the final path of the OPEN HANDLE, and only then reads or
// writes through that same handle. CeeLing code just calls these functions and maps the status codes.
//
// All paths are UTF-8. A path that is not absolute is taken relative to the FIRST allowed folder. Every function is
// safe to call from any thread. Nothing here is checked by CeeLing: it is part of the trusted boundary.
//
// What this does NOT protect against: a local attacker who can create links inside an allowed folder at the same
// moment a write is happening (a file may be created and then removed again if it turns out to lie outside), and
// anything the allowed folders themselves contain (a link to a sensitive place that the user placed there deliberately
// is followed, but only after the check that the target is inside an allowed folder).
#pragma once
#include <string>

enum AppFsStatus {
    APP_FS_OK = 0,
    APP_FS_FORBIDDEN = 1,    // outside every allowed folder (or a name Windows treats specially), or no folder allowed
    APP_FS_NOT_FOUND = 2,
    APP_FS_TOO_LARGE = 3,    // bigger than the limit the caller passed
    APP_FS_IO = 4,           // the operating system refused for another reason
    APP_FS_INVALID = 5,      // not valid UTF-8, empty, contains a NUL
    APP_FS_EXISTS = 6,       // create-only write, or mkdir without `parents`, and the name is taken
    APP_FS_NOT_EMPTY = 7,    // remove on a folder that has entries
    APP_FS_IS_DIR = 8,       // a file operation on a folder
    APP_FS_NOT_DIR = 9,      // a folder operation on a file
    APP_FS_DENIED = 10,      // inside an allowed folder, but the operating system's own permissions refuse
    APP_FS_TOO_MANY = 11     // a limit on how many of something (grants, watches) is reached
};

// 1 if `s` is well-formed UTF-8 (no overlongs, no surrogates, nothing above U+10FFFF), else 0. File contents go to the
// page inside JSON strings, so text read from disk must pass this first.
int app_fs_is_utf8(std::string s);

// Adds a folder to the allow-list. It must exist; it is stored in its canonical form (links resolved).
int app_fs_allow(std::string dir);
// Empties the allow-list (nothing is accessible afterwards), grants included.
int app_fs_clear();

// GRANTS: what the user picked in a dialog. Unlike the folders the application allows itself, a grant is as narrow as
// the choice: exactly one file (it may not exist yet: a "save as" target, whose folder must) or one folder. `path`
// must be absolute. At most 256 grants exist at a time (APP_FS_TOO_MANY beyond that). They are kept apart so they can be
// withdrawn without touching the application's own folders.
// On success `canonical` is the path in the form the file commands will accept (links resolved, case as stored): hand
// THAT to the page, not the text the dialog returned.
int app_fs_grant_file(std::string path, std::string& canonical);
int app_fs_grant_folder(std::string path, std::string& canonical);
int app_fs_revoke_grants();

// Resolves an EXISTING path that the file commands could reach to its canonical absolute form (links resolved), or
// fails exactly as they would. kind: 0 anything, 1 a file, 2 a folder (APP_FS_IS_DIR / APP_FS_NOT_DIR otherwise).
// This is how a page-supplied path or working folder is vetted before it is handed to a program.
int app_fs_resolve(std::string path, int kind, std::string& canonical);

// Reads a whole file. Fails with APP_FS_TOO_LARGE, without reading it, if it is larger than `max_bytes`.
int app_fs_read(std::string path, int max_bytes, std::string& out);

// mode 0: create or replace; 1: append (create if missing); 2: create only, APP_FS_EXISTS if the name is taken.
// The parent folder must already exist.
int app_fs_write(std::string path, std::string data, int mode);

// One record per entry, each field followed by a NUL: kind ("f" file, "d" folder, "l" link), size in bytes (decimal),
// modified time (decimal seconds since 1970), name. Order is unspecified.
int app_fs_list(std::string dir, std::string& out);

// The same four NUL-terminated fields for one path. A link is described as itself, not as its target.
int app_fs_stat(std::string path, std::string& out);

// Creates a folder. With `parents` non-zero, creates missing parents too (each one inside an allowed folder).
int app_fs_mkdir(std::string path, int parents);

// Removes a file, a link (the link, never its target), or an EMPTY folder. Never removes an allowed folder itself.
int app_fs_remove(std::string path);

#ifdef APP_HEADLESS
// Test-only: a fresh empty folder under the system temp directory (returned in `out`), and a recursive delete that
// is NOT confined to the allow-list. Not available in the window build.
int app_fs_test_make_temp_dir(std::string& out);
int app_fs_test_remove_tree(std::string path);
// Test-only: writes a file with NO check against the allow-list (to change something the page may no longer reach).
int app_fs_test_write_raw(std::string path, std::string data);
// Test-only: a directory link `link` -> `target` (a junction on Windows, a symlink elsewhere). Not allow-listed.
int app_fs_test_make_dir_link(std::string link, std::string target);
#endif
