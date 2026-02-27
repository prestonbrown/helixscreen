// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_coalesced_timer.h"

#include "../lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: single schedule fires callback once",
                 "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer(1);

    timer.schedule([&call_count]() { call_count++; });

    REQUIRE(timer.pending());
    process_lvgl(50);

    REQUIRE(call_count == 1);
    REQUIRE_FALSE(timer.pending());
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: multiple rapid schedules coalesce to one call",
                 "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer(10);

    // Schedule 5 times rapidly — should coalesce into a single callback
    for (int i = 0; i < 5; i++) {
        timer.schedule([&call_count]() { call_count++; });
    }

    REQUIRE(timer.pending());
    process_lvgl(50);

    REQUIRE(call_count == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: cancel prevents callback from firing",
                 "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer(10);

    timer.schedule([&call_count]() { call_count++; });
    REQUIRE(timer.pending());

    timer.cancel();
    REQUIRE_FALSE(timer.pending());

    process_lvgl(50);
    REQUIRE(call_count == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: destructor cancels pending timer",
                 "[coalesced_timer]") {
    // Verify destructor properly cleans up (doesn't crash or leak).
    // The cancel test above verifies callback suppression; this tests RAII cleanup.
    {
        CoalescedTimer timer(1000);
        timer.schedule([]() {});
        REQUIRE(timer.pending());
    } // destructor calls cancel() — timer deleted via lv_timer_delete
    SUCCEED("Destructor completed without crash");
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: re-schedule after fire works",
                 "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer(1);

    timer.schedule([&call_count]() { call_count++; });
    process_lvgl(50);
    REQUIRE(call_count == 1);

    // Schedule again after first fire
    timer.schedule([&call_count]() { call_count++; });
    REQUIRE(timer.pending());
    process_lvgl(50);
    REQUIRE(call_count == 2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "CoalescedTimer: last callback wins when schedule called multiple times",
                 "[coalesced_timer]") {
    int value = 0;
    CoalescedTimer timer(10);

    timer.schedule([&value]() { value = 1; });
    timer.schedule([&value]() { value = 2; });
    timer.schedule([&value]() { value = 3; });

    process_lvgl(50);
    REQUIRE(value == 3);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: move transfers pending timer",
                 "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer1(10);
    timer1.schedule([&call_count]() { call_count++; });
    REQUIRE(timer1.pending());

    CoalescedTimer timer2(std::move(timer1));
    REQUIRE_FALSE(timer1.pending());
    REQUIRE(timer2.pending());

    process_lvgl(50);
    REQUIRE(call_count == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: default period is 1ms", "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer; // default period

    timer.schedule([&call_count]() { call_count++; });
    process_lvgl(50);

    REQUIRE(call_count == 1);
}
