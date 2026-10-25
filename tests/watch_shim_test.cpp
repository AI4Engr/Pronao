// Tests for shim/app_watch.cpp against the real file system: the watcher puts records in the window's call queue (with an
// empty id) and this reads them back through app_next_call the way the CeeLing main loop does.
#include "app_fs.h"
#include "app_shim.h"
#include "app_watch.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define PATHSEP "\\"
#else
#include <sys/stat.h>
#define PATHSEP "/"
#endif

static int g_failed = 0;
static int g_passed = 0;
static void check(const char* name, bool ok) {
    if (ok) { ++g_passed; std::printf("PASS %s\n", name); }
    else { ++g_failed; std::printf("FAIL %s\n", name); }
}

struct Rec {
    int watch = 0;
    std::string kind, path, from;
    bool internal = false;
};

static std::vector<std::string> fields(const std::string& rec) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : rec) { if (c == '\0') { f.push_back(cur); cur.clear(); } else cur += c; }
    return f;
}

// every record that arrives until `quietMs` pass with nothing new (or `maxMs` in all)
static std::vector<Rec> gather(int h, int quietMs, int maxMs = 6000) {
    std::vector<Rec> out;
    auto start = std::chrono::steady_clock::now();
    for (;;) {
        std::string id, req;
        int r = app_next_call(h, quietMs, id, req);
        if (r != 1) break;
        auto f = fields(req);
        Rec rec;
        rec.internal = id.empty();
        if (f.size() >= 5 && f[0] == "watch") { rec.watch = std::atoi(f[1].c_str()); rec.kind = f[2]; rec.path = f[3]; rec.from = f[4]; }
        out.push_back(rec);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > maxMs) break;
    }
    return out;
}

static bool has(const std::vector<Rec>& v, const std::string& kind, const std::string& path) {
    for (const auto& r : v) if (r.kind == kind && r.path == path) return true;
    return false;
}
static int count(const std::vector<Rec>& v, const std::string& kind, const std::string& path) {
    int n = 0;
    for (const auto& r : v) if (r.kind == kind && r.path == path) ++n;
    return n;
}

static void put(const std::string& p, const std::string& s, bool append = false) {
    std::ofstream f(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    f << s;
}
static void mkd(const std::string& p) {
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}
static std::string join(const std::string& a, const std::string& b) { return a + PATHSEP + b; }

int main() {
    int h = app_open("watch test", 10, 10, "", "", 0);
    if (h == 0) { std::printf("FAIL cannot open a headless window\n"); return 2; }
    std::string base;
    if (app_fs_test_make_temp_dir(base) != APP_FS_OK) { std::printf("FAIL no temp dir\n"); return 2; }
    const std::string dir = join(base, "watched");
    mkd(dir);
    app_fs_allow(dir);
    int w1 = 0;

    // ---- starting ----
    check("start a watch", app_watch_start(h, dir, 0, w1) == APP_FS_OK && w1 > 0);
    int junk = 0;
    check("a folder outside the allowed ones", app_watch_start(h, base, 0, junk) == APP_FS_FORBIDDEN && junk == 0);
    check("a folder that does not exist", app_watch_start(h, join(dir, "nope"), 0, junk) == APP_FS_NOT_FOUND);
    put(join(dir, "plain.txt"), "x");
    check("a file is not a folder", app_watch_start(h, join(dir, "plain.txt"), 0, junk) == APP_FS_NOT_DIR);
    check("a dot-dot path", app_watch_start(h, join(dir, ".." PATHSEP "watched" PATHSEP ".." PATHSEP "outside"), 0, junk) == APP_FS_FORBIDDEN);
    check("no such window", app_watch_start(h + 50, dir, 0, junk) == APP_FS_NOT_FOUND);
    gather(h, 300);                                           // plain.txt was created before... or just after the watch began

    // ---- the basic changes ----
    put(join(dir, "a.txt"), "one");
    auto r = gather(h, 400);
    check("a created file is reported", has(r, "created", "a.txt"));
    check("with an empty id and the watch's id", !r.empty() && r[0].internal && r[0].watch == w1);

    put(join(dir, "a.txt"), "two!", true);
    r = gather(h, 400);
    check("a modified file is reported", has(r, "modified", "a.txt"));

    std::remove(join(dir, "renamed.txt").c_str());
    std::rename(join(dir, "a.txt").c_str(), join(dir, "renamed.txt").c_str());
    r = gather(h, 400);
    bool renamed = false;
    for (const auto& x : r) if (x.kind == "renamed" && x.path == "renamed.txt" && x.from == "a.txt") renamed = true;
    check("a rename carries the old and the new name", renamed);

    std::remove(join(dir, "renamed.txt").c_str());
    r = gather(h, 400);
    check("a removed file is reported", has(r, "removed", "renamed.txt"));

    // created through the shim, which speaks UTF-8 (a narrow std::ofstream would use the ANSI code page on Windows)
    app_fs_write(join(dir, "caf\xc3\xa9-\xe2\x82\xac.txt"), "u", 0);
    r = gather(h, 400);
    check("a Unicode name arrives as UTF-8", has(r, "created", "caf\xc3\xa9-\xe2\x82\xac.txt"));

    // ---- recursion ----
    mkd(join(dir, "sub"));
    r = gather(h, 400);
    check("a new folder is reported", has(r, "created", "sub"));
    put(join(dir, "sub" PATHSEP "inner.txt"), "i");
    r = gather(h, 400);
    check("a non-recursive watch does not see inside a subfolder", !has(r, "created", "sub/inner.txt") && !has(r, "modified", "sub/inner.txt"));

    int w2 = 0;
    check("start a recursive watch", app_watch_start(h, dir, 1, w2) == APP_FS_OK && w2 != w1);
    gather(h, 300);
    put(join(dir, "sub" PATHSEP "inner.txt"), "more", true);
    r = gather(h, 400);
    bool sawRecursive = false;
    for (const auto& x : r) if (x.watch == w2 && x.kind == "modified" && x.path == "sub/inner.txt") sawRecursive = true;
    check("a recursive watch sees inside a subfolder, with a relative path", sawRecursive);
    mkd(join(dir, "newdir"));
    gather(h, 300);
    put(join(dir, "newdir" PATHSEP "later.txt"), "l");
    r = gather(h, 500);
    bool sawNew = false;
    for (const auto& x : r) if (x.watch == w2 && x.path == "newdir/later.txt") sawNew = true;
    check("a folder created after the watch began is watched too", sawNew);
    check("no event carries an absolute path", [&] {
        for (const auto& x : r) { if (x.path.find(':') != std::string::npos || (!x.path.empty() && x.path[0] == '/')) return false; }
        return true;
    }());
    check("stop the recursive watch", app_watch_stop(h, w2) == APP_FS_OK);
    check("stopping twice", app_watch_stop(h, w2) == APP_FS_NOT_FOUND);
    gather(h, 200);

    // ---- bursts ----
    put(join(dir, "burst.txt"), "");
    gather(h, 300);
    for (int i = 0; i < 60; ++i) put(join(dir, "burst.txt"), "line\n", true);
    r = gather(h, 500);
    check("sixty quick writes are merged into a few events", has(r, "modified", "burst.txt") && count(r, "modified", "burst.txt") <= 6);
    for (int i = 0; i < 200; ++i) put(join(dir, "many-" + std::to_string(i) + ".txt"), "x");
    r = gather(h, 600, 10000);
    int seen = 0;
    for (int i = 0; i < 200; ++i) if (has(r, "created", "many-" + std::to_string(i) + ".txt")) ++seen;
    check("two hundred new files are all reported", seen == 200);

    // ---- the watch ends when its permission does ----
    const std::string granted = join(base, "granted");
    mkd(granted);
    std::string canon;
    app_fs_grant_folder(granted, canon);
    int w3 = 0;
    check("watch a folder the user granted", app_watch_start(h, granted, 0, w3) == APP_FS_OK);
    gather(h, 300);
    put(join(granted, "before.txt"), "x");
    r = gather(h, 400);
    check("it reports while the grant stands", has(r, "created", "before.txt"));
    app_fs_revoke_grants();
    put(join(granted, "after.txt"), "x");
    r = gather(h, 500);
    bool stopped = false, leaked = false;
    for (const auto& x : r) {
        if (x.watch == w3 && x.kind == "stopped" && x.path == "access_revoked") stopped = true;
        if (x.watch == w3 && x.path == "after.txt") leaked = true;
    }
    check("revoking the grant ends the watch", stopped);
    check("and the change that followed is not reported", !leaked);
    put(join(granted, "later.txt"), "x");
    r = gather(h, 300);
    check("nothing more arrives from it", r.empty());

    // ---- the watched folder goes away ----
    const std::string doomed = join(dir, "doomed");
    mkd(doomed);
    int w4 = 0;
    check("watch a folder that will be deleted", app_watch_start(h, doomed, 0, w4) == APP_FS_OK);
    gather(h, 300);
    app_fs_test_remove_tree(doomed);
    r = gather(h, 800);
    bool gone = false;
    for (const auto& x : r) if (x.watch == w4 && x.kind == "stopped") gone = true;
    check("deleting the watched folder ends the watch", gone);

    // ---- limits ----
    int ids[40] = {};
    int ok = 0, last = APP_FS_OK;
    for (int i = 0; i < 40 && last == APP_FS_OK; ++i) {
        last = app_watch_start(h, dir, 0, ids[i]);
        if (last == APP_FS_OK) ++ok;
    }
    check("at most 32 watches in one window", ok >= 28 && ok <= 32 && last == APP_FS_TOO_MANY);
    for (int i = 0; i < ok; ++i) app_watch_stop(h, ids[i]);
    int again = 0;
    check("stopping frees the slots", app_watch_start(h, dir, 0, again) == APP_FS_OK);

    // ---- stopping ----
    app_watch_stop(h, w1);
    gather(h, 200);
    put(join(dir, "after-stop.txt"), "x");
    r = gather(h, 400);
    bool fromStopped = false;
    for (const auto& x : r) if (x.watch == w1) fromStopped = true;
    check("a stopped watch sends nothing", !fromStopped);

    // closing the window stops every watch and does not hang
    auto t0 = std::chrono::steady_clock::now();
    app_close(h);
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check("closing the window stops its watches promptly", ms < 3000);
    check("and the folder can be deleted afterwards", app_fs_test_remove_tree(base) == APP_FS_OK);

    std::printf("watch shim test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
