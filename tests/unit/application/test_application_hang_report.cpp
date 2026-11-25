// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The main-loop hang report against the platform's background state: a loop
// that is parked because the app is in the background is not a hang.

#include "application.h"
#include "system/telemetry_manager.h"
#include "test_helpers/application_test_access.h"
#include "test_helpers/telemetry_test_fixture.h"

#include "../../catch_amalgamated.hpp"

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
