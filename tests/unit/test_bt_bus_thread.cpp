// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for helix::bluetooth::BusThread — the single-threaded owner of an
// sd_bus* connection. Tests exercise lifecycle, serialization, and shutdown
// semantics without requiring a real BlueZ/D-Bus stack.
//
// Bus acquisition: tests construct BusThread with nullptr. A null bus is a
// legal configuration meaning "no bus, idle worker" — the loop runs and
// processes queued work items but skips all sd_bus_* calls. This lets the
// tests exercise the work-queue, lifecycle, and serialization logic — which
// is what the plan calls for. Full D-Bus behaviour is covered by integration
// tests against real BlueZ on target hardware.
//
// Source inclusion: we compile bt_bus_thread.cpp directly into this translation
// unit (same pattern as test_ui_switch.cpp) because src/bluetooth/*.cpp is
// excluded from APP_SRCS (built into the libhelix-bluetooth.so plugin instead).
//
// Platform guard: the Bluetooth plugin only builds on Linux with libsystemd-dev
// installed (see mk/bluetooth.mk). On macOS and Linux hosts without libsystemd,
// this test compiles to an empty translation unit so `make test` still succeeds.

#include "../catch_amalgamated.hpp"

#if defined(__has_include) && __has_include(<systemd/sd-bus.h>)

#include <atomic>
#include <chrono>
#include <future>
#include <systemd/sd-bus.h>
#include <thread>
#include <vector>

// Include implementation directly — src/bluetooth/ is excluded from APP_SRCS
// (it lives in the Bluetooth plugin .so), so the object isn't available to
// link against for tests.
#include "../../src/bluetooth/bt_bus_thread.cpp"

using helix::bluetooth::BusThread;
using namespace std::chrono_literals;

TEST_CASE("BusThread starts and stops cleanly", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();
    std::this_thread::sleep_for(50ms);
    bt.stop();
    // stop() is idempotent
    bt.stop();
    SUCCEED("start/stop completed without hang or crash");
}

TEST_CASE("BusThread submit runs the work item on the thread", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();

    std::thread::id caller_id = std::this_thread::get_id();
    std::thread::id work_id{};

    bt.submit([&](sd_bus*) { work_id = std::this_thread::get_id(); }).get();

    REQUIRE(work_id != std::thread::id{});
    REQUIRE(work_id != caller_id);

    bt.stop();
}

TEST_CASE("BusThread run_sync blocks until work completes", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();

    std::atomic<int> counter{0};
    bt.run_sync([&](sd_bus*) {
        std::this_thread::sleep_for(20ms);
        counter.store(42);
    });
    // run_sync must not return before the work completes.
    REQUIRE(counter.load() == 42);

    bt.stop();
}

TEST_CASE("BusThread serializes interleaved work", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();

    std::atomic<int> in_flight{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> completed{0};

    constexpr int THREADS = 3;
    constexpr int PER_THREAD = 20;

    std::vector<std::thread> submitters;
    submitters.reserve(THREADS);

    // Collect futures on the main thread to avoid racing on a shared container.
    std::mutex futures_mu;
    std::vector<std::future<void>> futures;
    futures.reserve(THREADS * PER_THREAD);

    for (int t = 0; t < THREADS; ++t) {
        submitters.emplace_back([&] {
            for (int i = 0; i < PER_THREAD; ++i) {
                auto fut = bt.submit([&](sd_bus*) {
                    int now = in_flight.fetch_add(1) + 1;
                    int prev_max = max_concurrent.load();
                    while (now > prev_max && !max_concurrent.compare_exchange_weak(prev_max, now)) {
                        // retry
                    }
                    // Small amount of work to widen the window for any racing work items.
                    std::this_thread::sleep_for(100us);
                    in_flight.fetch_sub(1);
                    completed.fetch_add(1);
                });
                std::lock_guard<std::mutex> lk(futures_mu);
                futures.push_back(std::move(fut));
            }
        });
    }

    for (auto& t : submitters)
        t.join();

    // Drain: run_sync returns only after all previously-queued items run
    // (FIFO order, single worker).
    bt.run_sync([](sd_bus*) {});

    for (auto& f : futures) {
        REQUIRE_NOTHROW(f.get());
    }

    REQUIRE(completed.load() == THREADS * PER_THREAD);
    REQUIRE(max_concurrent.load() == 1);
    REQUIRE(in_flight.load() == 0);

    bt.stop();
}

TEST_CASE("BusThread stop breaks pending futures", "[bt][slow]") {
    BusThread t(nullptr);
    t.start();

    auto slow = t.submit([](sd_bus*) { std::this_thread::sleep_for(100ms); });
    auto pending = t.submit([](sd_bus*) {});
    // stop() runs while the worker is still inside the slow item; pending stays queued
    // and gets its promise broken in stop()'s post-join drain.
    std::this_thread::sleep_for(10ms); // ensure worker has picked up slow
    t.stop();

    REQUIRE_NOTHROW(slow.get());
    REQUIRE_THROWS(pending.get());
}

TEST_CASE("BusThread start is idempotent", "[bt][slow]") {
    BusThread t(nullptr);
    t.start();
    t.start(); // must be a no-op, no second thread spawned
    t.start();
    t.stop();
    SUCCEED(); // didn't crash or deadlock
}

TEST_CASE("BusThread concurrent start+submit: on_thread never races thread_id_",
          "[bt][bus_thread][slow]") {
    // Regression: thread_id_ used to be written by the parent after std::thread
    // construction, so the worker's first on_thread() check could race with a
    // submit() from another thread calling on_thread() via run_sync(). Here we
    // start the thread and immediately submit work items whose body reads
    // on_thread() — with the fix (worker publishes its own id before loop()),
    // every work item sees on_thread() == true.
    for (int iter = 0; iter < 20; ++iter) {
        BusThread bt(nullptr);
        std::atomic<bool> go{false};
        constexpr int SUBMITTERS = 4;
        std::vector<std::thread> threads;
        std::atomic<int> on_thread_true{0};
        std::atomic<int> on_thread_total{0};
        std::mutex fm;
        std::vector<std::future<void>> futs;
        for (int i = 0; i < SUBMITTERS; ++i) {
            threads.emplace_back([&] {
                while (!go.load()) { /* spin */
                }
                auto f = bt.submit([&](sd_bus*) {
                    on_thread_total.fetch_add(1);
                    if (bt.on_thread())
                        on_thread_true.fetch_add(1);
                });
                std::lock_guard<std::mutex> lk(fm);
                futs.push_back(std::move(f));
            });
        }
        bt.start();
        go.store(true);
        for (auto& t : threads)
            t.join();
        for (auto& f : futs)
            REQUIRE_NOTHROW(f.get());
        bt.stop();
        REQUIRE(on_thread_total.load() == SUBMITTERS);
        REQUIRE(on_thread_true.load() == SUBMITTERS);
    }
}

TEST_CASE("BusThread submit racing with stop: no orphan tasks", "[bt][bus_thread][slow]") {
    // Regression: submit()'s pre-check of running_/stopping_ was outside mu_,
    // so a concurrent stop() could drain the queue between the check and the
    // emplace — leaving the newly-pushed item as an orphan whose promise never
    // resolved until ~BusThread. With the in-lock re-check, every future
    // either completes successfully or throws; none hang.
    for (int iter = 0; iter < 50; ++iter) {
        BusThread bt(nullptr);
        bt.start();
        std::vector<std::future<void>> futs;
        std::mutex fm;
        std::atomic<bool> go{false};
        constexpr int SUBMITTERS = 3;
        std::vector<std::thread> threads;
        for (int i = 0; i < SUBMITTERS; ++i) {
            threads.emplace_back([&] {
                while (!go.load()) { /* spin */
                }
                for (int j = 0; j < 50; ++j) {
                    try {
                        auto f = bt.submit([](sd_bus*) {});
                        std::lock_guard<std::mutex> lk(fm);
                        futs.push_back(std::move(f));
                    } catch (...) {
                        // submit() doesn't throw, but defensively ignore.
                    }
                }
            });
        }
        go.store(true);
        // Let a few submits land before stopping.
        std::this_thread::sleep_for(1ms);
        bt.stop();
        for (auto& t : threads)
            t.join();
        // Every future must be ready (either fulfilled or exceptional) — no orphans.
        for (auto& f : futs) {
            REQUIRE(f.wait_for(1s) == std::future_status::ready);
            try {
                f.get();
            } catch (const std::runtime_error&) { /* expected post-stop */
            }
        }
    }
}

TEST_CASE("BusThread post-loop drain breaks promises before destructor", "[bt][bus_thread][slow]") {
    // Regression for the sd_bus_process error-exit orphan-task leak: when
    // loop() exits early (error path), queued items used to sit in the queue
    // until ~BusThread, hanging callers blocked on .get(). With the post-loop
    // drain in loop(), every future resolves promptly when the loop exits —
    // we don't need the destructor to run.
    //
    // Simulating an sd_bus_process failure isn't possible with a null bus
    // (the sd_bus_* branch is skipped entirely), so we exercise the normal
    // exit path and confirm the drain happens INSIDE loop() — by checking
    // futures are ready BEFORE the BusThread is destroyed. Without the
    // post-loop drain, this still passes because stop() drains too; but the
    // test documents the contract and guards against regressions in either
    // drain site.
    auto bt = std::make_unique<BusThread>(nullptr);
    bt->start();

    // Block the worker on a slow item so the next submits sit in the queue.
    auto slow = bt->submit([](sd_bus*) { std::this_thread::sleep_for(50ms); });
    std::this_thread::sleep_for(5ms); // ensure worker picked up `slow`

    std::vector<std::future<void>> pending;
    for (int i = 0; i < 5; ++i) {
        pending.push_back(bt->submit([](sd_bus*) {}));
    }

    // Begin shutdown.
    bt->stop();
    REQUIRE_NOTHROW(slow.get());

    // All pending futures must be ready (and exceptional) without us
    // destroying bt — proving the drain ran inside stop()/loop(), not in the
    // destructor.
    for (auto& f : pending) {
        REQUIRE(f.wait_for(100ms) == std::future_status::ready);
        REQUIRE_THROWS_AS(f.get(), std::runtime_error);
    }
    bt.reset(); // destructor runs last, finds nothing to clean up
}

TEST_CASE("BusThread submit().get() inside a work item runs inline (no deadlock)",
          "[bt][bus_thread][slow]") {
    // Regression: sd_bus dispatch callbacks (match handlers, timeouts) run on
    // the bus thread. If any such callback does bt.submit(...).get() — or its
    // moral equivalent .wait() — the work would be enqueued behind the
    // currently-executing item, and .get() would block forever waiting on a
    // promise that the same thread is supposed to fulfill. submit() must
    // detect on_thread() and run inline instead of queueing.
    BusThread t(nullptr);
    t.start();

    std::atomic<int> outer_ran{0};
    std::atomic<int> inner_ran{0};

    auto outer_fut = t.submit([&](sd_bus*) {
        std::thread::id outer_tid = std::this_thread::get_id();
        outer_ran.store(1);
        // Recursive submit + blocking get — must not deadlock.
        auto inner = t.submit([&](sd_bus*) {
            REQUIRE(std::this_thread::get_id() == outer_tid);
            inner_ran.store(1);
        });
        // Bound the wait so a regression fails fast instead of hanging the test run.
        REQUIRE(inner.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        REQUIRE_NOTHROW(inner.get());
    });
    REQUIRE(outer_fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE_NOTHROW(outer_fut.get());
    REQUIRE(outer_ran.load() == 1);
    REQUIRE(inner_ran.load() == 1);

    t.stop();
}

TEST_CASE("BusThread run_sync inside a work item runs inline (no deadlock)", "[bt][slow]") {
    BusThread t(nullptr);
    t.start();

    std::atomic<int> outer_tid_seen{0};
    std::atomic<int> inner_tid_seen{0};
    t.run_sync([&](sd_bus*) {
        // We are on the bus thread.
        std::thread::id outer = std::this_thread::get_id();
        outer_tid_seen.store(1);
        t.run_sync([&](sd_bus*) {
            // Should run inline on the same thread (no queue, no deadlock).
            REQUIRE(std::this_thread::get_id() == outer);
            inner_tid_seen.store(1);
        });
    });
    REQUIRE(outer_tid_seen.load() == 1);
    REQUIRE(inner_tid_seen.load() == 1);
    t.stop();
}

#endif // __has_include(<systemd/sd-bus.h>)
