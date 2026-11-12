// Drives the commands of examples/cpp (plain C++) through the framework's real dispatcher and file gate, with the headless window double.
// The per-user folders point at a scratch folder first, so nothing outside it is touched.
#include <cstdio>
#include <memory>
#include <string>

#include "../../examples/cpp/commands.h"
#include "../doubles/test_support.h"

static int passed = 0;
static int failed = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    (ok ? passed : failed)++;
}

// Sends one request through the whole pipeline and returns "<status> <json>".
static std::string roundTrip(const std::shared_ptr<App>& app, const std::string& id, const std::string& request) {
    app_test_inject(app->handle, id, request);
    int turns = 0;
    while (app_test_response_ready(app->handle) == 0 && turns < 3000) {
        app->serveOne(20);
        ++turns;
    }
    std::string gotId;
    std::string json;
    int status = 0;
    if (app_test_take_response(app->handle, gotId, status, json) != 1) return "NO RESPONSE";
    if (gotId != id) return "WRONG ID " + gotId;
    return std::to_string(status) + " " + json;
}

static bool failedWith(const std::string& reply, const std::string& code) {
    return reply.rfind("1 {\"code\":\"" + code + "\"", 0) == 0;
}

int main() {
    std::string base;
    app_test_make_temp_dir(base);
    app_test_mkdir_raw(base + "/local");
    app_test_mkdir_raw(base + "/xdg");
    app_test_mkdir_raw(base + "/home");
    app_test_setenv("LOCALAPPDATA", base + "/local");
    app_test_setenv("XDG_DATA_HOME", base + "/xdg");
    app_test_setenv("HOME", base + "/home");
    app_test_write_raw(base + "/local/outside.txt", "secret");
    app_test_write_raw(base + "/xdg/outside.txt", "secret");

    auto app = std::make_shared<App>();
    app->open("t", 100, 100, "<html></html>", "", false);
    check("before installCpp nothing is registered", failedWith(roundTrip(app, "z", "[\"add\",[1,2]]"), "unknown_command"));
    installCpp(app);

    check("add", roundTrip(app, "a1", "[\"add\",[2,3]]") == "0 5");
    check("add with a missing argument is refused", failedWith(roundTrip(app, "a2", "[\"add\",[2]]"), "command_failed"));
    check("mean", roundTrip(app, "m1", "[\"mean\",[4,8,12]]") == "0 8");
    check("mean of nothing is refused with its own code", failedWith(roundTrip(app, "m2", "[\"mean\",[]]"), "invalid_argument"));
    check("greet", roundTrip(app, "g1", "[\"greet\",{\"name\":\"Ada\"}]") == "0 \"hello, Ada\"");
    check("greet without a name is refused", failedWith(roundTrip(app, "g2", "[\"greet\",{}]"), "invalid_argument"));
    check("a std::runtime_error becomes command_failed, not a crash",
          roundTrip(app, "f1", "[\"fail\",[]]") == "1 {\"code\":\"command_failed\",\"message\":\"this command always fails\"}");
    check("the program is still serving after the exception", roundTrip(app, "f2", "[\"add\",[1,1]]") == "0 2");
    check("an unknown command is unknown_command", failedWith(roundTrip(app, "u1", "[\"nope\",[]]"), "unknown_command"));

    // the framework's checked file policy, called from C++
    check("the page writes inside the data folder",
          roundTrip(app, "w1", "[\"fs.write\",{\"path\":\"note.txt\",\"text\":\"hi\",\"mode\":\"replace\"}]") == "0 null");
    check("and reads it back", roundTrip(app, "r1", "[\"fs.read\",{\"path\":\"note.txt\"}]") == "0 {\"text\":\"hi\"}");
    check("a path that leaves the folder is forbidden", failedWith(roundTrip(app, "r2", "[\"fs.read\",{\"path\":\"../outside.txt\"}]"), "forbidden"));
    std::string elsewhere = base + "/local/outside.txt";
    for (std::size_t i = 0; (i = elsewhere.find('\\', i)) != std::string::npos; i += 2) elsewhere.insert(i, "\\");      // JSON: a backslash is written twice
    check("an absolute path elsewhere is forbidden", failedWith(roundTrip(app, "r3", "[\"fs.read\",{\"path\":\"" + elsewhere + "\"}]"), "forbidden"));

    app->close();
    app_test_remove_tree(base);
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
