// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_http_executor.cpp
 * @brief Unit tests for HttpExecutor — single-threaded executor for HTTP work.
 *
 * Tests the HttpExecutor class which serializes HTTP requests through a single
 * long-lived worker thread, replacing the previous per-request std::thread
 * spawning pattern that failed catastrophically under thread exhaustion
 * (EAGAIN from pthread_create, #811-adjacent).
 */

#include "http_executor.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::http::HttpExecutor;
using namespace std::chrono_literals;

TEST_CASE("HttpExecutor: submit runs work on worker thread", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    std::atomic<bool> ran{false};
    std::thread::id worker_id{};
    auto fut = ex.submit([&]() {
        ran.store(true);
        worker_id = std::this_thread::get_id();
    });

    fut.wait();
    REQUIRE(ran.load());
    REQUIRE(worker_id != std::this_thread::get_id());

    ex.stop();
}

TEST_CASE("HttpExecutor: run_sync blocks until work completes", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    std::atomic<int> counter{0};
    ex.run_sync([&]() {
        std::this_thread::sleep_for(20ms);
        counter.store(42);
    });
    REQUIRE(counter.load() == 42); // If run_sync returns before work, this fails.

    ex.stop();
}

TEST_CASE("HttpExecutor: concurrent submits run FIFO on single worker", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    std::mutex order_mu;
    std::vector<int> order;
    std::vector<std::future<void>> futs;

    for (int i = 0; i < 10; ++i) {
        futs.push_back(ex.submit([i, &order_mu, &order]() {
            std::this_thread::sleep_for(2ms);
            std::lock_guard<std::mutex> lk(order_mu);
            order.push_back(i);
        }));
    }
    for (auto& f : futs)
        f.wait();

    REQUIRE(order.size() == 10);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(order[i] == i);
    }

    ex.stop();
}

TEST_CASE("HttpExecutor: stop drains in-flight and breaks pending promises",
          "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    // One slow in-flight item that stop() must drain.
    std::atomic<bool> inflight_done{false};
    auto inflight = ex.submit([&]() {
        std::this_thread::sleep_for(50ms);
        inflight_done.store(true);
    });

    // Queue several more that should get their promises broken by stop().
    std::vector<std::future<void>> pending;
    for (int i = 0; i < 5; ++i) {
        pending.push_back(ex.submit([]() { std::this_thread::sleep_for(50ms); }));
    }

    std::this_thread::sleep_for(5ms); // Let the worker pick up the in-flight one.
    ex.stop();

    REQUIRE(inflight_done.load()); // In-flight item drained.

    // Pending items get a broken promise — future::wait() returns, but ::get()
    // throws std::future_error(broken_promise).
    int broken = 0;
    for (auto& f : pending) {
        try {
            f.get();
        } catch (const std::future_error&) {
            ++broken;
        }
    }
    REQUIRE(broken == 5);
}

TEST_CASE("HttpExecutor: on_thread detects worker vs caller", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    REQUIRE_FALSE(ex.on_thread()); // Called from test thread.

    std::atomic<bool> seen{false};
    ex.run_sync([&]() { seen.store(ex.on_thread()); });
    REQUIRE(seen.load());

    ex.stop();
}

TEST_CASE("HttpExecutor: destructor calls stop automatically", "[http_executor][slow]") {
    std::atomic<bool> ran{false};
    {
        HttpExecutor ex("test", 1);
        ex.start();
        ex.submit([&]() { ran.store(true); }).wait();
    } // Destructor must stop/join cleanly.
    REQUIRE(ran.load());
}

TEST_CASE("HttpExecutor: submit from inside work item does not deadlock", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    // Reentrant submit must not deadlock; order is outer-then-inner.
    std::vector<int> order;
    std::mutex order_mu;
    std::future<void> inner_fut;
    auto outer = ex.submit([&]() {
        {
            std::lock_guard<std::mutex> lk(order_mu);
            order.push_back(1);
        }
        // Must NOT call .get()/.wait() on the inner future here — that
        // would self-wait and deadlock. Fire-and-forget is the rule.
        inner_fut = ex.submit([&]() {
            std::lock_guard<std::mutex> lk(order_mu);
            order.push_back(2);
        });
    });

    outer.wait();
    inner_fut.wait();
    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == 1);
    REQUIRE(order[1] == 2);

    ex.stop();
}

TEST_CASE("HttpExecutor: multi-worker runs items concurrently", "[http_executor][slow]") {
    HttpExecutor ex("test", 4);
    ex.start();

    // 4 items each sleep 50ms. With parallelism they complete in ~50ms;
    // serialized on 1 worker they'd take ~200ms. Budget 120ms to tolerate
    // scheduler jitter while still detecting serialization.
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 4; ++i) {
        futs.push_back(ex.submit([]() { std::this_thread::sleep_for(50ms); }));
    }
    for (auto& f : futs)
        f.wait();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(elapsed < 120ms);

    ex.stop();
}

TEST_CASE("HttpExecutor: submit from multiple threads is safe", "[http_executor][slow]") {
    HttpExecutor ex("test", 2);
    ex.start();

    std::atomic<int> count{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&ex, &count]() {
            for (int i = 0; i < 50; ++i) {
                ex.submit([&count]() { count.fetch_add(1); });
            }
        });
    }
    for (auto& t : producers)
        t.join();

    // Drain with a final run_sync — guarantees all prior submits have run.
    ex.run_sync([]() {});
    REQUIRE(count.load() == 200);

    ex.stop();
}

TEST_CASE("HttpExecutor: on_thread distinguishes separate executor instances",
          "[http_executor][slow]") {
    HttpExecutor a("a", 1);
    HttpExecutor b("b", 1);
    a.start();
    b.start();

    std::atomic<bool> a_sees_a{false}, a_sees_b{false};
    std::atomic<bool> b_sees_a{false}, b_sees_b{false};
    a.run_sync([&]() {
        a_sees_a.store(a.on_thread());
        a_sees_b.store(b.on_thread());
    });
    b.run_sync([&]() {
        b_sees_a.store(a.on_thread());
        b_sees_b.store(b.on_thread());
    });

    REQUIRE(a_sees_a.load());
    REQUIRE_FALSE(a_sees_b.load());
    REQUIRE_FALSE(b_sees_a.load());
    REQUIRE(b_sees_b.load());

    a.stop();
    b.stop();
}

TEST_CASE("HttpExecutor: stop with timeout detaches stuck worker", "[http_executor][slow]") {
    // Controllable block instead of a raw 30s sleep: the worker waits on a
    // shared condition_variable we release at the end of the test. A prior
    // version used `sleep_for(30s)`, which left a detached worker bomb that
    // fired during a later test's fixture (UAF via shared_ptr<SharedState>
    // now; previously an EINVAL mutex-lock crash).
    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        bool go = false;
    };
    auto gate = std::make_shared<Gate>();

    HttpExecutor ex("test", 1);
    ex.start();

    (void)ex.submit([gate]() {
        std::unique_lock<std::mutex> lk(gate->mu);
        gate->cv.wait(lk, [&]() { return gate->go; });
    });
    std::this_thread::sleep_for(10ms); // Let worker pick it up.

    // stop() should return quickly via the detach path, not block until the
    // worker finishes.
    auto t0 = std::chrono::steady_clock::now();
    ex.stop(50ms);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(elapsed < 500ms); // Generous: 50ms timeout + poll + detach overhead.

    // Release the detached worker so it exits cleanly instead of lingering
    // until process teardown. SharedState keeps the mutex/cv alive via the
    // worker's shared_ptr, so re-entering the loop body after the lambda
    // returns is safe even though `ex` may have been destroyed by then.
    {
        std::lock_guard<std::mutex> lk(gate->mu);
        gate->go = true;
    }
    gate->cv.notify_all();
}

TEST_CASE("HttpExecutor: singletons are usable and idempotent", "[http_executor][slow]") {
    HttpExecutor::start_all();
    HttpExecutor::start_all(); // Idempotent.

    std::atomic<int> fast_runs{0};
    std::atomic<int> slow_runs{0};
    HttpExecutor::fast().run_sync([&]() { fast_runs.fetch_add(1); });
    HttpExecutor::slow().run_sync([&]() { slow_runs.fetch_add(1); });
    REQUIRE(fast_runs.load() == 1);
    REQUIRE(slow_runs.load() == 1);

    HttpExecutor::stop_all();
    HttpExecutor::stop_all(); // Idempotent.

    // fast() and slow() are process-wide and nothing else in a test run starts
    // them, so a stopped lane stays stopped for every test ordered after this
    // one. Their submit() then rejects silently - broken promise, work never
    // run, inflight() reading 0 - and a test waiting on async work sees it
    // complete instantly having done nothing. Restart the lanes, and run work
    // on both to prove the restart took.
    HttpExecutor::start_all();
    HttpExecutor::fast().run_sync([&]() { fast_runs.fetch_add(1); });
    HttpExecutor::slow().run_sync([&]() { slow_runs.fetch_add(1); });
    CHECK(fast_runs.load() == 2);
    CHECK(slow_runs.load() == 2);
}

TEST_CASE("HttpExecutor: submit after stop breaks promise immediately", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();
    ex.stop();

    auto fut = ex.submit([]() { FAIL("should not run after stop"); });
    bool threw = false;
    try {
        fut.get();
    } catch (const std::future_error&) {
        threw = true;
    }
    REQUIRE(threw);
}

TEST_CASE("HttpExecutor: stop is idempotent", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();
    ex.stop();
    ex.stop(); // Must not crash or hang.
    SUCCEED();
}

TEST_CASE("HttpExecutor: start is idempotent", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();
    ex.start(); // Must not spawn a second worker.

    std::atomic<int> calls{0};
    ex.run_sync([&]() { calls.fetch_add(1); });
    REQUIRE(calls.load() == 1);

    ex.stop();
}

TEST_CASE("HttpExecutor: exception in work item does not kill worker", "[http_executor][slow]") {
    HttpExecutor ex("test", 1);
    ex.start();

    // First item throws.
    auto bad = ex.submit([]() { throw std::runtime_error("boom"); });

    // The future should surface the exception.
    bool got_exc = false;
    try {
        bad.get();
    } catch (const std::runtime_error&) {
        got_exc = true;
    }
    REQUIRE(got_exc);

    // Worker must still be alive and accepting work.
    std::atomic<bool> ran{false};
    ex.run_sync([&]() { ran.store(true); });
    REQUIRE(ran.load());

    ex.stop();
}
