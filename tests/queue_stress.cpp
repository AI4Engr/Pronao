// Stress test for the shim's call queue (shim/queue.h, through the headless test double), no CeeLing. Several "UI threads" inject calls at once
// (what the webview's bind callback does), one consumer drains them with app_next_call, and several responder
// threads answer with app_return -- the shapes the real program has, plus more contention than it will see.
// Checks: every call is answered exactly once, with its own payload, and each producer's calls come out in the
// order that producer sent them. Built under ThreadSanitizer by tests/run_sanitizers.sh.
#include "../shim/app_native.h"
#include "doubles/test_support.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

int main() {
    const int producers = 4, perProducer = 500, responders = 3;
    int h = app_open("stress", 10, 10, "", "", "", 0);
    if (h == 0) { std::printf("FAIL: app_open\n"); return 2; }

    app_limits(h, 1 << 20, 1 << 30);          // the stress test queues thousands of calls before the consumer starts
    std::vector<std::thread> pool;
    for (int p = 0; p < producers; ++p) {
        pool.emplace_back([=] {
            for (int k = 0; k < perProducer; ++k) {
                std::string id = std::to_string(p) + ":" + std::to_string(k);
                app_test_inject(h, id, "[\"echo\"," + std::to_string(p * 100000 + k) + "]");
            }
        });
    }

    // consumer (this thread) -> hand each call to a responder through a second guarded queue
    std::mutex qm;
    std::vector<std::pair<std::string, std::string>> work;
    std::atomic<bool> consumed{false};
    std::vector<std::thread> answerers;
    for (int r = 0; r < responders; ++r) {
        answerers.emplace_back([&] {
            for (;;) {
                std::pair<std::string, std::string> item;
                bool have = false;
                {
                    std::lock_guard<std::mutex> lock(qm);
                    if (!work.empty()) { item = work.back(); work.pop_back(); have = true; }
                }
                if (have) {
                    // the reply carries the request text back, so the checker can match payloads
                    app_return(h, item.first, 0, item.second);
                } else if (consumed.load()) {
                    return;
                }
            }
        });
    }

    int received = 0;
    std::map<int, int> lastSeen;          // producer -> last sequence number consumed
    bool orderOk = true;
    const int total = producers * perProducer;
    while (received < total) {
        std::string id, req;
        int r = app_next_call(h, 10000, id, req);
        if (r != 1) { std::printf("FAIL: app_next_call returned %d after %d calls\n", r, received); orderOk = false; break; }
        int p = std::stoi(id.substr(0, id.find(':')));
        int k = std::stoi(id.substr(id.find(':') + 1));
        if (lastSeen.count(p) && lastSeen[p] + 1 != k) orderOk = false;
        lastSeen[p] = k;
        { std::lock_guard<std::mutex> lock(qm); work.emplace_back(id, req); }
        ++received;
    }
    consumed = true;
    for (auto& t : pool) t.join();
    for (auto& t : answerers) t.join();

    std::set<std::string> answered;
    bool payloadOk = true;
    for (int i = 0; i < received; ++i) {
        std::string id, json; int status = -1;
        if (app_test_take_response(h, id, status, json) != 1) { payloadOk = false; break; }
        if (!answered.insert(id).second) payloadOk = false;       // answered twice
        int p = std::stoi(id.substr(0, id.find(':')));
        int k = std::stoi(id.substr(id.find(':') + 1));
        if (json != "[\"echo\"," + std::to_string(p * 100000 + k) + "]" || status != 0) payloadOk = false;
    }
    app_close(h);
    bool ok = orderOk && payloadOk && received == total && static_cast<int>(answered.size()) == total;
    std::printf(ok ? "queue stress ok (%d calls)\n" : "FAIL: queue stress (received %d, order %d, payload %d)\n",
                received, orderOk, payloadOk);
    return ok ? 0 : 1;
}
