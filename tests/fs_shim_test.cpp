// Tests for the shim's file access (shim/app_fs.cpp), on the real file system, with no CeeLing involved.
// The point of most checks is a path that LOOKS allowed and is not: `..`, a folder whose name merely starts with an
// allowed one, a link or junction that leads out, Windows device and stream names. Built headless (APP_HEADLESS).
#include "app_fs.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
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

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool exists(const std::string& p) { std::ifstream f(p, std::ios::binary); return static_cast<bool>(f); }
static void put(const std::string& p, const std::string& s) { std::ofstream f(p, std::ios::binary); f << s; }
static void mkd(const std::string& p) {
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}
static int grantFile(const std::string& p) { std::string c; return app_fs_grant_file(p, c); }
static int grantFolder(const std::string& p) { std::string c; return app_fs_grant_folder(p, c); }
static std::string join(const std::string& a, const std::string& b) { return a + PATHSEP + b; }

// records: kind NUL size NUL mtime NUL name NUL
static std::vector<std::string> fields(const std::string& rec) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : rec) { if (c == '\0') { f.push_back(cur); cur.clear(); } else cur += c; }
    return f;
}
static bool listHas(const std::string& rec, char kind, const std::string& name) {
    auto f = fields(rec);
    for (size_t i = 0; i + 3 < f.size(); i += 4) if (f[i][0] == kind && f[i + 3] == name) return true;
    return false;
}

int main() {
    std::string base;
    if (app_fs_test_make_temp_dir(base) != APP_FS_OK) { std::printf("FAIL cannot make a temp dir\n"); return 2; }
    const std::string allowed = join(base, "allowed");
    const std::string evil = join(base, "allowed_evil");       // starts with the allowed folder's name
    const std::string outside = join(base, "outside");
    mkd(allowed); mkd(evil); mkd(outside);
    put(join(outside, "secret.txt"), "TOP SECRET");
    put(join(evil, "x.txt"), "evil");
    put(join(allowed, "hello.txt"), "hello world");
    mkd(join(allowed, "sub"));
    put(join(allowed, "sub" PATHSEP "inner.txt"), "inner");

    std::string out;

    // ---- nothing is allowed until a folder is added ----
    check("no folder allowed: read is forbidden", app_fs_read(join(allowed, "hello.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("allow a folder", app_fs_allow(allowed) == APP_FS_OK);
    check("allow a missing folder fails", app_fs_allow(join(base, "nope")) == APP_FS_NOT_FOUND);
    check("allow a file fails", app_fs_allow(join(allowed, "hello.txt")) == APP_FS_NOT_DIR);

    // ---- reading ----
    check("read inside", app_fs_read(join(allowed, "hello.txt"), 1000, out) == APP_FS_OK && out == "hello world");
    check("read in a subfolder", app_fs_read(join(allowed, "sub" PATHSEP "inner.txt"), 1000, out) == APP_FS_OK && out == "inner");
    check("relative path hangs off the first allowed folder", app_fs_read("hello.txt", 1000, out) == APP_FS_OK && out == "hello world");
    check("relative path with a subfolder", app_fs_read("sub/inner.txt", 1000, out) == APP_FS_OK && out == "inner");
    check("read a missing file inside", app_fs_read(join(allowed, "missing.txt"), 1000, out) == APP_FS_NOT_FOUND);
    check("read a folder", app_fs_read(join(allowed, "sub"), 1000, out) == APP_FS_IS_DIR);

    // ---- text that looks inside and is not ----
    check("dot-dot escape is forbidden", app_fs_read(join(allowed, ".." PATHSEP "outside" PATHSEP "secret.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("dot-dot after a subfolder", app_fs_read(join(allowed, "sub" PATHSEP ".." PATHSEP ".." PATHSEP "outside" PATHSEP "secret.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("relative dot-dot escape", app_fs_read("../outside/secret.txt", 1000, out) == APP_FS_FORBIDDEN);
    check("absolute path outside", app_fs_read(join(outside, "secret.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("a sibling whose name starts with the allowed one", app_fs_read(join(evil, "x.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("a missing file outside does not say 'not found'", app_fs_read(join(outside, "nope.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("the allowed folder itself is not a file", app_fs_read(allowed, 1000, out) == APP_FS_IS_DIR);

    // ---- a link or junction that leads out ----
    const std::string linkOut = join(allowed, "linkout");
    check("make a link to the outside", app_fs_test_make_dir_link(linkOut, outside) == APP_FS_OK);
    check("read through a link that leads out", app_fs_read(join(linkOut, "secret.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("list through a link that leads out", app_fs_list(linkOut, out) == APP_FS_FORBIDDEN);
    check("write through a link that leads out", app_fs_write(join(linkOut, "planted.txt"), "x", 0) == APP_FS_FORBIDDEN);
    check("nothing was planted outside", !exists(join(outside, "planted.txt")));
    check("mkdir through a link that leads out", app_fs_mkdir(join(linkOut, "newdir"), 0) == APP_FS_FORBIDDEN);
    const std::string linkIn = join(allowed, "linkin");
    check("make a link that stays inside", app_fs_test_make_dir_link(linkIn, join(allowed, "sub")) == APP_FS_OK);
    check("read through a link that stays inside", app_fs_read(join(linkIn, "inner.txt"), 1000, out) == APP_FS_OK && out == "inner");
    check("stat describes a link as a link", app_fs_stat(linkOut, out) == APP_FS_OK && fields(out)[0] == "l");

    // ---- sizes ----
    put(join(allowed, "big.txt"), std::string(100, 'z'));
    check("too large for the limit", app_fs_read(join(allowed, "big.txt"), 50, out) == APP_FS_TOO_LARGE);
    check("exactly the limit", app_fs_read(join(allowed, "big.txt"), 100, out) == APP_FS_OK && out.size() == 100);
    check("negative limit", app_fs_read(join(allowed, "big.txt"), -1, out) == APP_FS_TOO_LARGE);

    // ---- invalid input ----
    check("empty path", app_fs_read("", 10, out) == APP_FS_INVALID);
    check("NUL in path", app_fs_read(std::string("a\0b", 3), 10, out) == APP_FS_INVALID);
    check("invalid UTF-8 in path", app_fs_read("\xff\xfe", 10, out) == APP_FS_INVALID);
    check("utf8: ascii", app_fs_is_utf8("hello") == 1);
    check("utf8: two, three and four byte sequences", app_fs_is_utf8("\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80") == 1);
    check("utf8: overlong", app_fs_is_utf8("\xc0\xaf") == 0);
    check("utf8: surrogate", app_fs_is_utf8("\xed\xa0\x80") == 0);
    check("utf8: above U+10FFFF", app_fs_is_utf8("\xf4\x90\x80\x80") == 0);
    check("utf8: truncated", app_fs_is_utf8("\xe2\x82") == 0);
    check("utf8: lone continuation byte", app_fs_is_utf8("\x80") == 0);

    // ---- writing ----
    const std::string w = join(allowed, "w.txt");
    check("create-only creates", app_fs_write(w, "one", 2) == APP_FS_OK && slurp(w) == "one");
    check("create-only on an existing name", app_fs_write(w, "two", 2) == APP_FS_EXISTS && slurp(w) == "one");
    check("replace with something shorter truncates", app_fs_write(w, "x", 0) == APP_FS_OK && slurp(w) == "x");
    check("append", app_fs_write(w, "yz", 1) == APP_FS_OK && slurp(w) == "xyz");
    check("append creates a missing file", app_fs_write(join(allowed, "appended.txt"), "a", 1) == APP_FS_OK && slurp(join(allowed, "appended.txt")) == "a");
    check("write with a missing parent", app_fs_write(join(allowed, "nodir" PATHSEP "f.txt"), "x", 0) == APP_FS_NOT_FOUND);
    check("write over a folder", app_fs_write(join(allowed, "sub"), "x", 0) == APP_FS_IS_DIR);
    check("write outside", app_fs_write(join(outside, "w.txt"), "x", 0) == APP_FS_FORBIDDEN && !exists(join(outside, "w.txt")));
    check("write via dot-dot", app_fs_write(join(allowed, ".." PATHSEP "outside" PATHSEP "w2.txt"), "x", 0) == APP_FS_FORBIDDEN && !exists(join(outside, "w2.txt")));
    check("empty file", app_fs_write(join(allowed, "empty.txt"), "", 0) == APP_FS_OK && slurp(join(allowed, "empty.txt")).empty());
    check("binary data survives", app_fs_write(join(allowed, "bin.dat"), std::string("a\0b\xff", 4), 0) == APP_FS_OK &&
          app_fs_read(join(allowed, "bin.dat"), 100, out) == APP_FS_OK && out == std::string("a\0b\xff", 4));
    check("unicode file name round trip", app_fs_write(join(allowed, "caf\xc3\xa9-\xe2\x82\xac.txt"), "u", 0) == APP_FS_OK &&
          app_fs_read(join(allowed, "caf\xc3\xa9-\xe2\x82\xac.txt"), 100, out) == APP_FS_OK && out == "u");

    // ---- listing and stat ----
    check("list the allowed folder", app_fs_list(allowed, out) == APP_FS_OK && listHas(out, 'f', "hello.txt") && listHas(out, 'd', "sub") && listHas(out, 'l', "linkout"));
    check("list has no dot entries", out.find(std::string(".\0", 2)) == std::string::npos || !listHas(out, 'd', "."));
    check("list a file", app_fs_list(join(allowed, "hello.txt"), out) == APP_FS_NOT_DIR);
    check("list outside", app_fs_list(outside, out) == APP_FS_FORBIDDEN);
    check("list an empty folder", app_fs_mkdir(join(allowed, "emptydir"), 0) == APP_FS_OK && app_fs_list(join(allowed, "emptydir"), out) == APP_FS_OK && out.empty());
    check("stat a file", app_fs_stat(join(allowed, "hello.txt"), out) == APP_FS_OK && fields(out)[0] == "f" && fields(out)[1] == "11" && fields(out)[3] == "hello.txt");
    check("stat a folder", app_fs_stat(join(allowed, "sub"), out) == APP_FS_OK && fields(out)[0] == "d");
    check("stat the allowed folder itself", app_fs_stat(allowed, out) == APP_FS_OK && fields(out)[0] == "d");
    check("stat missing", app_fs_stat(join(allowed, "missing"), out) == APP_FS_NOT_FOUND);
    check("stat outside", app_fs_stat(join(outside, "secret.txt"), out) == APP_FS_FORBIDDEN);

    // ---- mkdir ----
    check("mkdir", app_fs_mkdir(join(allowed, "m1"), 0) == APP_FS_OK);
    check("mkdir twice", app_fs_mkdir(join(allowed, "m1"), 0) == APP_FS_EXISTS);
    check("mkdir with a missing parent", app_fs_mkdir(join(allowed, "p" PATHSEP "q" PATHSEP "r"), 0) == APP_FS_NOT_FOUND);
    check("mkdir with parents", app_fs_mkdir(join(allowed, "p" PATHSEP "q" PATHSEP "r"), 1) == APP_FS_OK &&
          app_fs_stat(join(allowed, "p" PATHSEP "q" PATHSEP "r"), out) == APP_FS_OK);
    check("mkdir outside", app_fs_mkdir(join(outside, "d"), 0) == APP_FS_FORBIDDEN);
    check("mkdir with parents outside", app_fs_mkdir(join(allowed, ".." PATHSEP "outside" PATHSEP "d" PATHSEP "e"), 1) == APP_FS_FORBIDDEN);
    check("nothing was created outside", !exists(join(outside, "d")));

    // ---- remove ----
    check("remove a file", app_fs_remove(join(allowed, "appended.txt")) == APP_FS_OK && !exists(join(allowed, "appended.txt")));
    check("remove a missing file", app_fs_remove(join(allowed, "appended.txt")) == APP_FS_NOT_FOUND);
    check("remove a folder that is not empty", app_fs_remove(join(allowed, "sub")) == APP_FS_NOT_EMPTY);
    check("remove an empty folder", app_fs_remove(join(allowed, "emptydir")) == APP_FS_OK);
    check("remove outside", app_fs_remove(join(outside, "secret.txt")) == APP_FS_FORBIDDEN && exists(join(outside, "secret.txt")));
    check("remove the allowed folder itself", app_fs_remove(allowed) == APP_FS_FORBIDDEN);
    check("removing a link removes the link, not its target", app_fs_remove(linkOut) == APP_FS_OK && exists(join(outside, "secret.txt")));
    check("the link is gone", app_fs_stat(linkOut, out) == APP_FS_NOT_FOUND);

#ifdef _WIN32
    // ---- Windows-only ways to name something else ----
    std::string upper = allowed;
    for (auto& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    check("upper-case spelling of the folder is the same folder", app_fs_read(join(upper, "hello.txt"), 1000, out) == APP_FS_OK);
    std::string fwd = allowed;
    for (auto& c : fwd) if (c == '\\') c = '/';
    check("forward slashes", app_fs_read(fwd + "/hello.txt", 1000, out) == APP_FS_OK);
    check("alternate data stream name", app_fs_read(join(allowed, "hello.txt:stream"), 1000, out) == APP_FS_FORBIDDEN);
    check("write an alternate data stream", app_fs_write(join(allowed, "hello.txt:s"), "x", 0) == APP_FS_FORBIDDEN);
    check("device name NUL", app_fs_write(join(allowed, "NUL"), "x", 0) == APP_FS_FORBIDDEN);
    check("device name with an extension", app_fs_write(join(allowed, "aux.txt"), "x", 0) == APP_FS_FORBIDDEN);
    check("device name COM1", app_fs_write(join(allowed, "com1"), "x", 0) == APP_FS_FORBIDDEN);
    check("reading a device name does not read a device", app_fs_read(join(allowed, "CON"), 10, out) != APP_FS_OK);
    check("trailing dot", app_fs_write(join(allowed, "dot."), "x", 0) == APP_FS_FORBIDDEN);
    check("trailing space", app_fs_write(join(allowed, "space "), "x", 0) == APP_FS_FORBIDDEN);
    check("reserved characters", app_fs_write(join(allowed, "a?b"), "x", 0) == APP_FS_FORBIDDEN);
    check("\\\\?\\ prefix", app_fs_read("\\\\?\\" + join(allowed, "hello.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("\\\\.\\ device prefix", app_fs_read("\\\\.\\" + join(allowed, "hello.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("drive-relative path", app_fs_read("C:hello.txt", 1000, out) == APP_FS_FORBIDDEN);
    check("root-relative path", app_fs_read("\\Windows\\win.ini", 1000, out) == APP_FS_FORBIDDEN);
    check("a UNC path outside", app_fs_read("\\\\localhost\\c$\\Windows\\win.ini", 1000, out) == APP_FS_FORBIDDEN);
#endif

    // ---- two allowed folders ----
    check("allow a second folder", app_fs_allow(outside) == APP_FS_OK);
    check("the second folder is now readable", app_fs_read(join(outside, "secret.txt"), 1000, out) == APP_FS_OK && out == "TOP SECRET");
    check("the sibling that only shares a prefix still is not", app_fs_read(join(evil, "x.txt"), 1000, out) == APP_FS_FORBIDDEN);
    check("clear removes every folder", app_fs_clear() == APP_FS_OK && app_fs_read(join(allowed, "hello.txt"), 1000, out) == APP_FS_FORBIDDEN);

    // ---- grants: what the user picked in a dialog is exactly that, no more ----
    const std::string g1 = join(base, "g1");
    mkd(g1);
    put(join(g1, "a.txt"), "picked");
    put(join(g1, "b.txt"), "neighbour");
    mkd(join(g1, "dir"));
    put(join(g1, "dir" PATHSEP "deep.txt"), "deep");
    {
        std::string canon;
        check("grant a file", app_fs_grant_file(join(g1, "a.txt"), canon) == APP_FS_OK);
        check("the canonical form is a path the file commands accept", app_fs_read(canon, 100, out) == APP_FS_OK && out == "picked");
    }
    check("the granted file is readable", app_fs_read(join(g1, "a.txt"), 100, out) == APP_FS_OK && out == "picked");
    check("its neighbour is not", app_fs_read(join(g1, "b.txt"), 100, out) == APP_FS_FORBIDDEN);
    check("its folder is not listable", app_fs_list(g1, out) == APP_FS_FORBIDDEN);
    check("a path that goes up and comes back to the neighbour", app_fs_read(join(g1, "dir" PATHSEP ".." PATHSEP "b.txt"), 100, out) == APP_FS_FORBIDDEN);
    check("a path that goes up and comes back to the granted file", app_fs_read(join(g1, "dir" PATHSEP ".." PATHSEP "a.txt"), 100, out) == APP_FS_OK);
    check("the granted file is writable", app_fs_write(join(g1, "a.txt"), "changed", 0) == APP_FS_OK && slurp(join(g1, "a.txt")) == "changed");
    check("a new file next to it is not", app_fs_write(join(g1, "c.txt"), "x", 2) == APP_FS_FORBIDDEN && !exists(join(g1, "c.txt")));
    check("a relative path has no base when only grants exist", app_fs_read("a.txt", 100, out) == APP_FS_FORBIDDEN);
    check("a relative path cannot be granted", grantFile("a.txt") == APP_FS_INVALID);
    check("a folder cannot be granted as a file", grantFile(g1) == APP_FS_IS_DIR);
    check("grant a file that does not exist yet (save as)", grantFile(join(g1, "new.txt")) == APP_FS_OK);
    check("the new name can be created", app_fs_write(join(g1, "new.txt"), "n", 2) == APP_FS_OK && slurp(join(g1, "new.txt")) == "n");
    check("and read back", app_fs_read(join(g1, "new.txt"), 100, out) == APP_FS_OK && out == "n");
    check("a save-as target in a missing folder", grantFile(join(g1, "nofolder" PATHSEP "x.txt")) == APP_FS_NOT_FOUND);
    check("a granted file may be removed", app_fs_remove(join(g1, "new.txt")) == APP_FS_OK && !exists(join(g1, "new.txt")));
    check("grant a folder", grantFolder(join(g1, "dir")) == APP_FS_OK);
    check("the granted folder's contents are readable", app_fs_read(join(g1, "dir" PATHSEP "deep.txt"), 100, out) == APP_FS_OK && out == "deep");
    check("the granted folder itself cannot be removed", app_fs_remove(join(g1, "dir")) == APP_FS_FORBIDDEN);
    check("granting a file as a folder", grantFolder(join(g1, "b.txt")) == APP_FS_NOT_DIR);
    check("grants do not widen to the parent", app_fs_read(join(g1, "b.txt"), 100, out) == APP_FS_FORBIDDEN);
    check("an application folder and a grant coexist", app_fs_allow(allowed) == APP_FS_OK && app_fs_read("hello.txt", 100, out) == APP_FS_OK);
    check("a relative path still means the application's folder", app_fs_read("a.txt", 100, out) == APP_FS_NOT_FOUND);
    check("revoking grants keeps the application's folders", app_fs_revoke_grants() == APP_FS_OK &&
          app_fs_read("hello.txt", 100, out) == APP_FS_OK && app_fs_read(join(g1, "a.txt"), 100, out) == APP_FS_FORBIDDEN &&
          app_fs_read(join(g1, "dir" PATHSEP "deep.txt"), 100, out) == APP_FS_FORBIDDEN);
    int lastGrant = APP_FS_OK;
    int granted = 0;
    for (int i = 0; i < 300 && lastGrant == APP_FS_OK; ++i) {
        lastGrant = grantFile(join(g1, "limit" + std::to_string(i) + ".txt"));
        if (lastGrant == APP_FS_OK) ++granted;
    }
    check("at most 256 grants at a time", granted == 256 && lastGrant == APP_FS_TOO_MANY);
    check("revoke frees them", app_fs_revoke_grants() == APP_FS_OK && grantFile(join(g1, "again.txt")) == APP_FS_OK);
    app_fs_clear();

    app_fs_test_remove_tree(base);
    std::printf("fs shim test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
