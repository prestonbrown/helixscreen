// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: the subject is JoinOnExit itself, which is test infrastructure by
// definition, so there is no shipped code for this file to reach into

#include "../test_helpers/join_on_exit.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "../catch_amalgamated.hpp"

TEST_CASE("JoinOnExit joins a worker when the scope leaves early", "[threading][test-helpers]") {
    std::atomic<bool> finished{false};
    std::thread worker;

    {
        worker = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            finished.store(true);
        });
        try {
            helix::test::JoinOnExit worker_join(worker);
            REQUIRE(worker.joinable());
            // Stands in for the failing REQUIRE this guard exists for.
            throw std::runtime_error("early exit");
        } catch (const std::runtime_error&) {
        }
    }

    // Joined during the unwind, so the thread object died non-joinable and the
    // worker ran to completion instead of being abandoned.
    REQUIRE_FALSE(worker.joinable());
    REQUIRE(finished.load());
}

TEST_CASE("JoinOnExit leaves a worker the test already joined alone", "[threading][test-helpers]") {
    std::atomic<int> ticks{0};
    std::thread worker([&] { ticks.fetch_add(1); });
    helix::test::JoinOnExit worker_join(worker);

    worker.join();

    // A second join would throw std::system_error out of the destructor.
    REQUIRE_FALSE(worker.joinable());
    REQUIRE(ticks.load() == 1);
}
