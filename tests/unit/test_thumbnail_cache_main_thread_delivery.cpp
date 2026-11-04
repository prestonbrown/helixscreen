// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_thumbnail_cache_main_thread_delivery.cpp
 * @brief ThumbnailCache callbacks always land on the LVGL main thread (#960, #1202).
 *
 * Callers hand ThumbnailCache callbacks that set LVGL image sources, but which
 * thread a result arrived on used to depend on which internal path produced it.
 * #1202 identified the download callbacks; it closed with the mutex half and
 * left the marshalling half open. Two of the four paths are non-obvious:
 *
 *   - `fetch_optimized()`'s download ERROR fires on an HttpExecutor worker,
 *     while its download SUCCESS is already safe (it routes through
 *     ThumbnailProcessor::deliver_result, which queue_update()s).
 *   - ThumbnailProcessor's shutdown branches report `on_error` unmarshalled on
 *     the calling thread, and `process_and_callback` converts that into a
 *     *success* via the PNG fallback — so even a success could be delivered on
 *     a worker.
 *
 * Both entry points therefore wrap the caller's callbacks once, at the
 * boundary. These tests pin both halves of that contract: a background caller
 * is marshalled, and a main-thread caller still gets the synchronous delivery
 * it has today (a pre-scaled cache hit must not slip a frame).
 *
 * The null-api error path is used deliberately as the probe: it reaches the
 * callback with no HTTP, no filesystem state and no worker pool, so the test is
 * deterministic and fast while still exercising the real wrapper.
 */

#include "../../include/core/update_queue.h"
#include "../../include/thumbnail_cache.h"
#include "../lvgl_test_fixture.h"

#include <atomic>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailCache marshals a bg-thread callback to the main thread",
                 "[assets][cache][threading][960][1202]") {
    ThumbnailCache cache;

    std::atomic<bool> fired{false};
    std::atomic<bool> fired_before_drain{false};
    std::thread::id cb_thread{};

    const std::thread::id main_id = std::this_thread::get_id();
    REQUIRE(helix::ui::is_main_thread());

    std::thread worker([&] {
        // Null api reaches the "No API available" error without touching HTTP,
        // the filesystem or the processor pool.
        cache.fetch(nullptr, "does/not/exist/thumb.png", nullptr,
                    [&](const std::string& /*error*/) {
                        cb_thread = std::this_thread::get_id();
                        fired.store(true, std::memory_order_release);
                    });
    });
    worker.join();

    // The whole point: the worker has finished and the callback must NOT have
    // run on it. Without the boundary wrapper this is already true here, and
    // cb_thread is the worker's id.
    fired_before_drain.store(fired.load(std::memory_order_acquire));
    CHECK_FALSE(fired_before_drain.load());

    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(fired.load(std::memory_order_acquire));
    CHECK(cb_thread == main_id);
}

TEST_CASE_METHOD(LVGLTestFixture, "ThumbnailCache still delivers synchronously on the main thread",
                 "[assets][cache][threading][960][1202]") {
    // Marshalling unconditionally would push every cache hit a tick later.
    // run_on_main() runs inline when already on the main thread precisely so
    // this stays true; if that regresses, thumbnails start applying a frame
    // late everywhere and nothing else would catch it.
    ThumbnailCache cache;

    bool fired = false;
    cache.fetch(nullptr, "does/not/exist/thumb.png", nullptr,
                [&](const std::string& /*error*/) { fired = true; });

    CHECK(fired); // before any drain
}
