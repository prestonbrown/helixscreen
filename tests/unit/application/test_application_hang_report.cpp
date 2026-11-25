// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The main-loop hang report against the platform's background state: a loop
// that is parked because the app is in the background is not a hang. And what
// a report written by another thread says the main thread was waiting on.

#include "application.h"
#include "memory_utils.h"
#include "system/telemetry_manager.h"
#include "test_helpers/application_test_access.h"
#include "test_helpers/telemetry_test_fixture.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "../../catch_amalgamated.hpp"
#include "hv/json.hpp"

// The SDL event handler's bridges into Application.
extern "C" void helix_notify_app_backgrounded();
extern "C" void helix_notify_app_foregrounded();

TEST_CASE_METHOD(TelemetryTestFixture,
                 "A main-loop stall is reported as a hang only while the app is in the foreground",
                 "[application][2163]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    helix_notify_app_backgrounded();
    REQUIRE(ApplicationTestAccess::app_backgrounded());
    ApplicationTestAccess::report_main_loop_stall(60000);
    CHECK(tm.get_queue_snapshot().empty());

    helix_notify_app_foregrounded();
    REQUIRE_FALSE(ApplicationTestAccess::app_backgrounded());
    ApplicationTestAccess::report_main_loop_stall(60000);
    const auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].at("event") == "error_encountered");
    CHECK(snapshot[0].at("code") == "main_loop_hang");
}

#ifdef __linux__
TEST_CASE_METHOD(TelemetryTestFixture,
                 "A hang reported from another thread names what the main thread waits on",
                 "[application][2163]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);
    helix_notify_app_foregrounded();
    REQUIRE_FALSE(ApplicationTestAccess::app_backgrounded());

    // The main thread blocks in join() below, on a futex, as a UI thread waiting
    // for another thread's lock does. The reporter reports once the kernel says
    // the main thread is asleep, giving up after about a second.
    bool main_waiting = false;
    std::thread reporter([&] {
        for (int i = 0; i < 500 && !main_waiting; ++i) {
            main_waiting = helix::read_main_thread_state() == std::optional<std::string>("S");
            if (!main_waiting) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        ApplicationTestAccess::report_main_loop_stall(60000);
    });
    reporter.join();

    REQUIRE(main_waiting);
    const auto snapshot = tm.get_queue_snapshot();
    REQUIRE(snapshot.size() == 1);
    const auto& e = snapshot[0];
    REQUIRE(e.at("code") == "main_loop_hang");
    REQUIRE(e.contains("main_state"));
    CHECK(e["main_state"] == "S");
    REQUIRE(e.contains("main_wchan"));
    CHECK(e["main_wchan"].get<std::string>().rfind("futex", 0) == 0);
}
#endif
