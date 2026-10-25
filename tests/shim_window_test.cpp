// The shim on its own, with a real window and no CeeLing: the page calls two commands, the main thread (a
// stand-in for CeeLing's loop) answers from a thread that is NOT the UI thread, the page checks the answers and
// reports back through a third call, and the main thread then closes the window. Proves the threading model:
// bind callback on the UI thread -> queue -> any thread answers via app_return.
#include "app_shim.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static std::string slurp(const char* path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main(int, char** argv) {
    std::string bridge = slurp(argv[1]);
    std::string html = R"(<html><body><script>
      (async () => {
        const sum = await cling.invoke('add', [2, 3]);
        let rejected = null;
        try { await cling.invoke('fail', null); } catch (e) { rejected = e; }
        await cling.invoke('report', { sum: sum, rejected: rejected });
      })();
    </script></body></html>)";
    int h = app_open("shim test", 400, 300, html, bridge, 0);
    if (h == 0) { std::printf("FAIL: window not created\n"); return 2; }
    int calls = 0, ok = 0;
    for (;;) {
        std::string id, req;
        int r = app_next_call(h, 20000, id, req);
        if (r == 2) { std::printf("FAIL: timed out waiting for the page (%d calls so far)\n", calls); app_close(h); return 3; }
        if (r == 0) break;
        ++calls;
        std::printf("call %d: %s\n", calls, req.c_str());
        if (req.rfind("[\"add\"", 0) == 0) { app_return(h, id, 0, "5"); }
        else if (req.rfind("[\"fail\"", 0) == 0) { app_return(h, id, 1, "{\"code\":\"nope\",\"message\":\"refused\"}"); }
        else if (req.rfind("[\"report\"", 0) == 0) {
            ok = req.find("\"sum\":5") != std::string::npos && req.find("\"code\":\"nope\"") != std::string::npos;
            app_return(h, id, 0, "null");
            app_terminate(h);
        }
    }
    app_close(h);
    std::printf(ok ? "shim window test ok (%d calls)\n" : "FAIL: page reported wrong values (%d calls)\n", calls);
    return ok ? 0 : 1;
}
