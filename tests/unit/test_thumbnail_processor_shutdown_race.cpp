// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// ThumbnailProcessor::process_async() raced ThumbnailProcessor::shutdown().
//
// ThreadSanitizer caught it on a real run (#1202, found while chasing #1198):
// `thread_pool_` was null-checked while holding mutex_, the lock was released,
// and only THEN was the pool dereferenced. shutdown() resets that same member
// under that same mutex, so the gap is a use-after-free window on the pool.
//
// The consequence is worse than a plain dangling dereference, because
// HThreadPool::commit() opens with `if (status == STOP) start();`. A commit
// landing after shutdown() therefore RESURRECTS the pool and spawns worker
// threads after teardown — precisely the static-destruction hazard shutdown()
// exists to prevent.
//
// These tests use a private, non-singleton instance (via the TestAccess
// friend). Calling shutdown() on the process-wide ThumbnailProcessor would
// permanently disable thumbnail processing for every later test in the binary.

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "thumbnail_processor.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

/// Grants tests the private constructor. Declared a friend in
/// include/thumbnail_processor.h.
struct ThumbnailProcessorTestAccess {
    // Raw pointers, deliberately: both the ctor AND the dtor are private, so
    // unique_ptr's default deleter cannot be instantiated outside this friend.
    static helix::ThumbnailProcessor* make() {
        return new helix::ThumbnailProcessor();
    }
    static void destroy(helix::ThumbnailProcessor* p) {
        delete p;
    }
};

namespace {

/// Drains on teardown. process_async()'s success/error callbacks are deferred
/// through the UpdateQueue, so without this every case here would hand its
/// queued work to whichever test drained next — the exact defect #1167/#1169
/// just retired, and the untagged ceiling now has zero headroom.
struct ThumbnailRaceFixture : public HelixTestFixture {
    ~ThumbnailRaceFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

/// RAII around the friend accessor, so a failing CHECK cannot leak an instance
/// (each one owns a live thread pool).
struct ProcHandle {
    helix::ThumbnailProcessor* p = ThumbnailProcessorTestAccess::make();
    ProcHandle() = default;
    ProcHandle(const ProcHandle&) = delete;
    ProcHandle& operator=(const ProcHandle&) = delete;
    ~ProcHandle() {
        ThumbnailProcessorTestAccess::destroy(p);
    }
    helix::ThumbnailProcessor* operator->() const {
        return p;
    }
    helix::ThumbnailProcessor* get() const {
        return p;
    }
};

/// A tiny but genuinely decodable payload is unnecessary here: every test
/// below is about the shutdown handshake, and do_process() failing on garbage
/// still exercises the commit path. Kept small so the copies are cheap.
std::vector<uint8_t> dummy_png() {
    return std::vector<uint8_t>(64, 0x7F);
}

helix::ThumbnailTarget target_120() {
    helix::ThumbnailTarget t; // color_format already defaults to ARGB8888
    t.width = 120;
    t.height = 120;
    return t;
}

} // namespace

TEST_CASE_METHOD(ThumbnailRaceFixture,
                 "process_async after shutdown reports an error and queues nothing",
                 "[thumbnail][threading][1202]") {
    ProcHandle proc;

    proc->shutdown();

    std::atomic<int> errors{0};
    std::atomic<int> successes{0};
    proc->process_async(
        dummy_png(), "after_shutdown.png", target_120(),
        [&successes](const std::string&) { ++successes; },
        [&errors](const std::string&) { ++errors; });

    // The error path must fire synchronously — it is reported directly, not
    // deferred through the UpdateQueue, precisely because the pool is gone.
    CHECK(errors.load() == 1);
    CHECK(successes.load() == 0);

    // And nothing may have been enqueued: if commit() had run it would have
    // restarted the stopped pool.
    CHECK(proc->pending_tasks() == 0);
}

// ---------------------------------------------------------------------------
// process_file_async: the read belongs to the worker, not the caller.
//
// ThumbnailCache::process_and_callback() used to slurp the whole PNG on the
// MAIN thread and hand the bytes to process_async() — once per file while a
// file listing populated, so scrolling a gcode folder put a synchronous
// whole-file read on the LVGL loop per card. process_file_async() takes the
// path instead and opens it inside the pool task.
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(ThumbnailRaceFixture, "process_file_async opens the file on the worker thread",
                 "[thumbnail][threading][slow]") {
    ProcHandle proc;

    std::atomic<int> errors{0};
    std::atomic<int> successes{0};

    // A path that cannot possibly open. If the read happened on THIS thread the
    // failure would be known by the time the call returns.
    proc->process_file_async(
        "/nonexistent/definitely/not/here.png", "missing.png", target_120(),
        [&successes](const std::string&) { ++successes; },
        [&errors](const std::string&) { ++errors; });

    // Nothing reported yet: the open() has not run on the calling thread. This
    // is the assertion that fails if the read is ever moved back inline.
    CHECK(errors.load() == 0);
    CHECK(successes.load() == 0);

    // The worker runs, fails the open, and reports through the UpdateQueue.
    proc->wait_for_completion();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    CHECK(errors.load() == 1);
    CHECK(successes.load() == 0);
}

TEST_CASE_METHOD(ThumbnailRaceFixture, "process_file_async after shutdown reports an error",
                 "[thumbnail][threading][slow]") {
    ProcHandle proc;

    proc->shutdown();

    std::atomic<int> errors{0};
    std::atomic<int> successes{0};
    proc->process_file_async(
        "/tmp/whatever.png", "after_shutdown.png", target_120(),
        [&successes](const std::string&) { ++successes; },
        [&errors](const std::string&) { ++errors; });

    // Same contract as process_async(): with no pool the error is synchronous,
    // and nothing may be enqueued (a commit would restart the stopped pool).
    CHECK(errors.load() == 1);
    CHECK(successes.load() == 0);
    CHECK(proc->pending_tasks() == 0);
}

TEST_CASE_METHOD(ThumbnailRaceFixture, "shutdown is idempotent and keeps refusing work",
                 "[thumbnail][threading][1202]") {
    ProcHandle proc;

    proc->shutdown();
    proc->shutdown(); // second call must be a no-op, not a double-reset

    std::atomic<int> errors{0};
    proc->process_async(
        dummy_png(), "twice.png", target_120(), [](const std::string&) {},
        [&errors](const std::string&) { ++errors; });

    CHECK(errors.load() == 1);
    CHECK(proc->pending_tasks() == 0);
}

TEST_CASE_METHOD(ThumbnailRaceFixture, "wait_for_completion is safe against a concurrent shutdown",
                 "[thumbnail][threading][1202]") {
    // wait_for_completion() blocks, so it cannot hold mutex_. It takes a strong
    // reference instead; without that the pool can be freed mid-wait.
    ProcHandle proc;

    for (int i = 0; i < 8; ++i) {
        proc->process_async(
            dummy_png(), "wait_" + std::to_string(i) + ".png", target_120(),
            [](const std::string&) {}, [](const std::string&) {});
    }

    helix::ThumbnailProcessor* raw = proc.get();
    std::thread waiter([raw] { raw->wait_for_completion(); });
    std::thread stopper([raw] { raw->shutdown(); });

    waiter.join();
    stopper.join();

    CHECK(proc->pending_tasks() == 0);
}

TEST_CASE_METHOD(ThumbnailRaceFixture, "process_async racing shutdown never uses a freed pool",
                 "[thumbnail][threading][1202][slow]") {
    // The actual regression. Under the old code the submitting threads
    // dereference thread_pool_ after the lock is dropped, so a shutdown()
    // landing in that window is a use-after-free — and because commit()
    // restarts a stopped pool, it also leaks worker threads past teardown.
    //
    // Repeated because it is a timing window: with the fix every iteration is
    // clean; without it, ASAN/TSAN trip and even an uninstrumented build
    // eventually faults.
    constexpr int ITERATIONS = 40;
    constexpr int SUBMITTERS = 4;

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        ProcHandle proc;

        helix::ThumbnailProcessor* raw = proc.get();
        // shared_ptr, not a stack local: these callbacks are deferred through
        // the UpdateQueue and can run after this iteration returns, so
        // capturing &resolved by reference would be a UAF in the test.
        auto resolved = std::make_shared<std::atomic<int>>(0);
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        threads.reserve(SUBMITTERS + 1);

        for (int t = 0; t < SUBMITTERS; ++t) {
            threads.emplace_back([raw, resolved, &go, t] {
                while (!go.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < 6; ++i) {
                    raw->process_async(
                        dummy_png(), "race_" + std::to_string(t) + "_" + std::to_string(i) + ".png",
                        target_120(), [resolved](const std::string&) { ++*resolved; },
                        [resolved](const std::string&) { ++*resolved; });
                }
            });
        }

        threads.emplace_back([raw, &go] {
            while (!go.load(std::memory_order_acquire)) {
            }
            raw->shutdown();
        });

        go.store(true, std::memory_order_release);
        for (auto& th : threads) {
            th.join();
        }

        // The invariant is "no crash, no torn state". Every submission either
        // got queued before shutdown or was refused after it; success
        // callbacks are deferred through the UpdateQueue and are NOT drained
        // here, so `resolved` is deliberately not asserted to a fixed number.
        CHECK(proc->pending_tasks() == 0);
        // Drain per iteration, not just at teardown: 40 rounds of deferred
        // callbacks would otherwise accumulate in the queue.
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    SUCCEED("Completed " << ITERATIONS << " shutdown races without faulting");
}
