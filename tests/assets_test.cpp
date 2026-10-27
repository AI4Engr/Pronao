// The asset table (shim/assets.h) on its own: lookups, the origin boundary, decoding, and concurrent use. Run under the sanitizers too.
#include "../shim/assets.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_failed = 0, g_passed = 0;
static void check(const char* name, bool ok) { if (ok) { ++g_passed; std::printf("PASS %s\n", name); } else { ++g_failed; std::printf("FAIL %s\n", name); } }

struct Got { bool mine; int status = 0; std::string headers; std::string body; };
static Got ask(AssetStore& s, const std::string& method, const std::string& url) {
    Got g;
    std::shared_ptr<const std::string> body;
    g.mine = s.respond(method, url, g.status, g.headers, body);
    if (g.mine) g.body = *body;
    return g;
}

int main() {
    AssetStore s;
    check("nothing is answered before an origin is set", !ask(s, "GET", "http://a.localhost/x").mine);
    s.configure("http://a.localhost", "default-src 'self'");
    check("a path must start with /", !s.add("x", "text/plain", "x"));
    check("add", s.add("/x.txt", "text/plain", std::string("a\0b\xff", 4)) && s.add("/p/q.html", "text/html", "<p>"));
    check("an alias needs an existing target", !s.alias("/y", "/nope") && s.alias("/y", "/x.txt") && !s.alias("y", "/x.txt"));
    Got x = ask(s, "GET", "http://a.localhost/x.txt");
    check("binary bytes come back whole", x.mine && x.status == 200 && x.body == std::string("a\0b\xff", 4));
    check("through an alias too", ask(s, "GET", "http://a.localhost/y").body == x.body);
    check("only html gets the policy", ask(s, "GET", "http://a.localhost/p/q.html").headers.find("Content-Security-Policy") != std::string::npos && x.headers.find("Content-Security-Policy") == std::string::npos);
    check("query and fragment are dropped", ask(s, "GET", "http://a.localhost/x.txt?v=1#top").status == 200);
    check("percent escapes decode, a bad one is kept", s.add("/a b", "text/plain", "s") && ask(s, "GET", "http://a.localhost/a%20b").status == 200 && ask(s, "GET", "http://a.localhost/a%2").status == 404 && ask(s, "GET", "http://a.localhost/a%zz").status == 404);
    check("dot-dot never leaves the table", ask(s, "GET", "http://a.localhost/../x.txt").status == 404 && ask(s, "GET", "http://a.localhost/p/../x.txt").status == 404 && ask(s, "GET", "http://a.localhost/%2e%2e/x.txt").status == 404);
    check("a lookalike origin is the network's", !ask(s, "GET", "http://a.localhost.evil/x.txt").mine && !ask(s, "GET", "https://a.localhost/x.txt").mine && !ask(s, "GET", "http://a.localhos/x.txt").mine);
    check("the bare origin is /", ask(s, "GET", "http://a.localhost").mine);
    check("HEAD has no body but the length", ask(s, "HEAD", "http://a.localhost/x.txt").body.empty() && ask(s, "HEAD", "http://a.localhost/x.txt").headers.find("Content-Length: 4") != std::string::npos);
    check("other methods are 405", ask(s, "POST", "http://a.localhost/x.txt").status == 405 && ask(s, "DELETE", "http://a.localhost/x.txt").status == 405);
    check("reconfiguring changes the origin", (s.configure("http://b.localhost", ""), !ask(s, "GET", "http://a.localhost/x.txt").mine && ask(s, "GET", "http://b.localhost/x.txt").status == 200));
    // many threads asking and adding at once (the window's UI thread asks while CeeLing adds)
    std::vector<std::thread> pool;
    std::atomic<int> good{0};
    for (int t = 0; t < 4; ++t) {
        pool.emplace_back([&, t] {
            for (int i = 0; i < 500; ++i) {
                s.add("/t" + std::to_string(t) + "/" + std::to_string(i), "text/plain", std::string(100, 'x'));
                if (ask(s, "GET", "http://b.localhost/x.txt").status == 200) ++good;
            }
        });
    }
    for (auto& th : pool) th.join();
    check("concurrent adds and lookups", good == 2000 && ask(s, "GET", "http://b.localhost/t3/499").status == 200);
    std::printf("assets test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
