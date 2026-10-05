// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_single_flight_walk.cpp
 * @brief One USB walk at a time, and only the newest request's result shows.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/usb_scan_wait.h"
#include "single_flight_walk.h"

#include <atomic>
#include <future>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::SingleFlightWalk;

namespace {

/// A job that reports @p id once it may finish, recording what cancelled() said.
SingleFlightWalk::Job job(int id, std::vector<int>& delivered, std::shared_future<void> gate,
                          std::atomic<bool>* saw_cancel = nullptr) {
    return [id, &delivered, gate, saw_cancel](const SingleFlightWalk::Cancelled& cancelled) {
        gate.wait();
        if (saw_cancel) {
            saw_cancel->store(cancelled());
        }
        return std::function<void()>([id, &delivered]() { delivered.push_back(id); });
    };
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "SingleFlightWalk runs one job at a time, newest wins",
                 "[usb][usb_async]") {
    SingleFlightWalk walk;
    std::vector<int> delivered;
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::promise<void> open_now;
    open_now.set_value();
    std::shared_future<void> open = open_now.get_future().share();
    auto wait = [&] { helix::test::wait_for_usb_scan([&] { return walk.in_flight(); }); };

    SECTION("the result arrives through the UI queue, not inside run()") {
        walk.run(job(1, delivered, open));
        CHECK(delivered.empty());
        wait();
        CHECK(delivered == std::vector<int>{1});
    }

    SECTION("requests during a job cancel it and collapse into one more") {
        std::atomic<bool> first_cancelled{false};
        walk.run(job(1, delivered, gate, &first_cancelled));
        walk.run(job(2, delivered, open));
        walk.run(job(3, delivered, open));
        release.set_value();
        wait();
        CHECK(first_cancelled.load());
        CHECK(delivered == std::vector<int>{3});
    }

    SECTION("cancel drops the running job and the queued one") {
        walk.run(job(1, delivered, gate));
        walk.run(job(2, delivered, open));
        walk.cancel();
        release.set_value();
        wait();
        CHECK(delivered.empty());
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "SingleFlightWalk runs inline when its lane is not running",
                 "[usb][usb_async]") {
    helix::http::HttpExecutor stopped("stopped-lane", 1);
    SingleFlightWalk walk(stopped);
    std::vector<int> delivered;
    std::promise<void> open_now;
    open_now.set_value();
    std::shared_future<void> open = open_now.get_future().share();

    walk.run(job(1, delivered, open));
    CHECK_FALSE(walk.in_flight());
    CHECK(delivered == std::vector<int>{1});

    // A walk that never wedged takes the next request too.
    walk.run(job(2, delivered, open));
    CHECK(delivered == std::vector<int>{1, 2});
}
