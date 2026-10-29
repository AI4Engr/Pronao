// Stress test for the shim's call queue (shim/queue.h, through the headless test double), no CeeLing. Several "UI threads" inject calls at once
// (what the webview's bind callback does), one consumer drains them with app_next_call, and several responder
// threads answer with app_return -- the shapes the real program has, plus more contention than it will see.
// Checks: every call is answered exactly once, with its own payload, and each producer's calls come out in the
// order that producer sent them. Built under ThreadSanitizer by tests/run_sanitizers.sh.
#include "../shim/app_native.h"
#include "doubles/test_support.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Several windows, several producers per window, ONE consumer that serves them all through app_wait_any. Every call must arrive exactly once
// (no lost wakeup: a lost one makes the consumer sleep out its whole timeout, which is a failure), every window must be served (no
// starvation), and after the windows close app_wait_any must report that nothing is left.
static bool waitAnyStress() {
    const int windows = 4, producersPer = 3, perProducer = 300;
    std::vector<int> handles;
    for (int w = 0; w < windows; ++w) {
        int h = app_open("any", 10, 10, "", "", "", 0);
        if (h == 0) { std::printf("FAIL: app_open\n"); return false; }
        app_limits(h, 1 << 20, 1 << 30);
        handles.push_back(h);
    }
    std::vector<std::thread> pool;
    for (int w = 0; w < windows; ++w) {
        for (int p = 0; p < producersPer; ++p) {
            pool.emplace_back([=] {
                for (int k = 0; k < perProducer; ++k) {
                    // an id is only unique within its window (as a page's are): the same ids go to every window
                    app_test_inject(handles[static_cast<size_t>(w)], std::to_string(p) + ":" + std::to_string(k), "[]");
                }
            });
        }
    }
    const int total = windows * producersPer * perProducer;
    std::map<int, std::set<std::string>> got;
    int received = 0;
    bool ok = true;
    while (received < total) {
        int h = 0;
        int r = app_wait_any(handles, 5000, h);
        if (r != 1) { std::printf("FAIL: app_wait_any returned %d after %d of %d calls (a lost wakeup?)\n", r, received, total); ok = false; break; }
        std::string id, req;
        int n = app_next_call(h, 0, id, req);
        if (n != 1) continue;
        if (!got[h].insert(id).second) { std::printf("FAIL: a call arrived twice\n"); ok = false; }
        ++received;
    }
    for (auto& t : pool) t.join();
    for (int h : handles) if (static_cast<int>(got[h].size()) != producersPer * perProducer) { std::printf("FAIL: window %d was served %zu calls\n", h, got[h].size()); ok = false; }
    int h = 0;
    if (ok && app_wait_any(handles, 0, h) != 2) { std::printf("FAIL: app_wait_any found something after every call was taken\n"); ok = false; }
    for (int x : handles) app_terminate(x);                     // closes the queues: each is reported once, then retired
    int closes = 0;
    for (int guard = 0; guard < 100 && ok; ++guard) {
        int r = app_wait_any(handles, 2000, h);
        if (r == 0) break;
        if (r != 1) { std::printf("FAIL: app_wait_any returned %d while windows were closing\n", r); ok = false; break; }
        std::string id, req;
        if (app_next_call(h, 0, id, req) == 0) ++closes;
    }
    if (ok && (closes != windows || app_wait_any(handles, 0, h) != 0)) { std::printf("FAIL: %d of %d windows reported closed, or wait_any did not end\n", closes, windows); ok = false; }
    for (int x : handles) app_close(x);
    if (ok) std::printf("wait-any stress ok (%d windows, %d calls)\n", windows, total);
    return ok;
}

// Ping-pong: ONE call at a time into a random window while the consumer is already asleep in app_wait_any. The only thing that can wake it
// is the notification the queue sends; without it app_wait_any sleeps out its timeout, which is a failure here.
static bool pingPong() {
    const int windows = 3, rounds = 200;
    std::vector<int> handles;
    for (int w = 0; w < windows; ++w) {
        int h = app_open("pp", 10, 10, "", "", "", 0);
        if (h == 0) return false;
        handles.push_back(h);
    }
    std::atomic<int> sent{0}, taken{0};
    std::atomic<bool> stop{false};          // the consumer gave up: the producer must not wait for it forever
    std::thread producer([&] {
        for (int k = 0; k < rounds; ++k) {
            while (taken.load() < k && !stop.load()) std::this_thread::yield();       // the consumer has taken the previous one
            if (stop.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));   // and is asleep again
            app_test_inject(handles[static_cast<size_t>(k % windows)], std::to_string(k), "[]");
            ++sent;
        }
    });
    bool ok = true;
    for (int k = 0; k < rounds && ok; ++k) {
        int h = 0;
        const auto began = std::chrono::steady_clock::now();
        int r = app_wait_any(handles, 1500, h);
        // a wakeup that arrives takes milliseconds; one that is lost is only found when the wait times out and the queues are looked at again
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        if (r != 1 || waited > 700) { std::printf("FAIL: wait-any ping-pong round %d returned %d after %lld ms (the wakeup was lost)\n", k, r, static_cast<long long>(waited)); ok = false; break; }
        std::string id, req;
        if (app_next_call(h, 0, id, req) != 1 || id != std::to_string(k)) { std::printf("FAIL: ping-pong round %d got the wrong call\n", k); ok = false; }
        ++taken;
    }
    stop = true;
    producer.join();
    for (int x : handles) app_close(x);
    if (ok) std::printf("wait-any ping-pong ok (%d rounds)\n", rounds);
    return ok;
}

int main() {
    const bool anyOk = waitAnyStress() && pingPong();
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
    bool ok = anyOk && orderOk && payloadOk && received == total && static_cast<int>(answered.size()) == total;
    std::printf(ok ? "queue stress ok (%d calls)\n" : "FAIL: queue stress (received %d, order %d, payload %d)\n",
                received, orderOk, payloadOk);
    return ok ? 0 : 1;
}
