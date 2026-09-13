// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// HThreadPool's worker cap must hold under concurrent commits.
//
// createThread() checks cur_thread_num against max_thread_num before
// spawning, and that check has to be atomic with the increment: two threads
// committing at the same moment can both pass an unlocked check and both
// spawn, running live workers above max_thread_num. ThumbnailProcessor sizes
// its pool to leave headroom for the UI thread on 1-2 core devices, and the
// shutdown-race tests commit from four threads at once, so the growth path
// really is contended (prestonbrown/helixscreen#1585).
//
// TEST_MIRROR_OK: pins HThreadPool itself — the vendored, patched pool in
// lib/libhv. No helixscreen header wraps the cap reservation under test;
// include/thumbnail_processor.h only owns an instance of this pool.

#include <hv/hthreadpool.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

// Race the growth path the way the print-select metadata refresh does: one
// worker already busy, several threads committing at once. Rounds repeat
// across idle-out retirements because the window only opens while
// cur_thread_num sits below max.
TEST_CASE("HThreadPool never exceeds max threads under concurrent commits", "[threading][1585]") {
    constexpr int kMax = 2;
    HThreadPool pool(1, kMax, 100);
    pool.start(1);

    for (int round = 0; round < 12; ++round) {
        // Hold the one worker busy so every storm commit sees idle == 0 and
        // takes the growth path into createThread().
        std::promise<void> gate;
        auto busy = pool.commit([&gate] { gate.get_future().wait(); });

        std::atomic<int> started{0};
        std::vector<std::thread> stormers;
        for (int t = 0; t < 4; ++t) {
            stormers.emplace_back([&] {
                ++started;
                while (started < 4) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < 25; ++i) {
                    pool.commit([] {});
                }
            });
        }
        for (auto& t : stormers) {
            t.join();
        }
        gate.set_value();
        busy.wait();

        // Quiescent read: workers are busy or freshly idle, the 100ms idle-out
        // cannot have retired anyone yet, so the cap verdict is stable here.
        REQUIRE(pool.currentThreadNum() <= kMax);

        // Let the pool idle back down to min so the next round races at
        // cur == min again, re-opening the growth window.
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    pool.stop();
}

} // namespace
