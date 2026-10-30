// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_coalesced_timer.h"

#include "../lvgl_test_fixture.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

/// How many live LVGL timers carry @p p as their user_data. Compares pointers
/// only - never dereferences, so it is safe to ask about an object that has
/// already been destroyed.
///
/// Counted rather than tested as a boolean on purpose. lv_timer_cancel_safe()
/// neuters a timer without unlinking it or clearing its user_data, and the
/// linked-but-dead entry survives until the next lv_timer_handler pass. Earlier
/// tests therefore leave timers pointing at their own dead stack frames, and
/// those addresses get reused. Only the *delta* across a destructor is
/// meaningful; an absolute "nothing points here" is a coin flip on stack reuse.
int lvgl_timers_pointing_at(const void* p) {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (lv_timer_get_user_data(t) == p) {
            n++;
        }
    }
    return n;
}

/// Live entries in LVGL's timer list. A coalesced request must cost one timer
/// however often it is asked for: surplus timers fire into an empty callback
/// slot, so a call count alone cannot see them.
int live_timer_count() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        n++;
    }
    return n;
}

} // namespace

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
    // A destructor that leaves the timer armed fires timer_cb() into a callback
    // whose storage is gone - a live main-thread crash source. The flag below is
    // held in a shared_ptr so it outlives the timer and can be read afterwards.
    auto fired = std::make_shared<bool>(false);
    const void* timer_addr = nullptr;
    int pointing_before = 0;

    {
        CoalescedTimer timer(20);
        timer.schedule([fired]() { *fired = true; });
        REQUIRE(timer.pending());

        timer_addr = &timer;
        pointing_before = lvgl_timers_pointing_at(timer_addr);
        REQUIRE(pointing_before >= 1); // our own armed timer, at least
    } // destructor must cancel()

    // Checked before pumping, and by pointer comparison only: cancel() nulls the
    // timer's user_data, so a destructor that skips it shows up here cleanly
    // instead of by running the leaked callback into freed memory.
    //
    // There is no timer-count check to pair with this. cancel() routes through
    // lv_timer_cancel_safe(), which neuters the timer and leaves lv_timer_handler
    // to reap it, so the timer is still linked at this point either way.
    REQUIRE(lvgl_timers_pointing_at(timer_addr) == pointing_before - 1);

    // Well past the 20ms period.
    process_lvgl(200);
    REQUIRE_FALSE(*fired);
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

// ============================================================================
// schedule_once() — leading-edge coalescing
//
// The premise case comes first: lv_async_call() is what the render-path callers
// used to reach for, on the belief that it collapses repeat (cb, user_data)
// pairs. It does not, and that is why schedule_once() exists.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "lv_async_call does not coalesce repeat (cb, user_data)",
                 "[coalesced_timer]") {
    // lv_async_call mallocs a fresh lv_async_info_t and creates a fresh one-shot
    // timer per call (lv_async.c) — no dedup anywhere in it. Five identical calls
    // are five allocations, five timers, and five callback invocations. Callers
    // that schedule from a draw or animation path pay this every frame.
    static int call_count = 0;
    call_count = 0;
    int subject = 0;

    for (int i = 0; i < 5; i++) {
        lv_async_call([](void*) { call_count++; }, &subject);
    }
    process_lvgl(50);

    REQUIRE(call_count == 5);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: schedule_once runs the FIRST callback once",
                 "[coalesced_timer]") {
    // The distinction from schedule() is not "fires once" — schedule() also
    // fires once. It is *which* callback survives: schedule_once() drops the
    // later requests instead of replacing the pending one.
    int call_count = 0;
    int value = 0;
    CoalescedTimer timer(10);
    const int timers_before = live_timer_count();

    timer.schedule_once([&]() {
        call_count++;
        value = 1;
    });
    timer.schedule_once([&]() {
        call_count++;
        value = 2;
    });
    timer.schedule_once([&]() {
        call_count++;
        value = 3;
    });

    REQUIRE(timer.pending());
    // Three requests, one timer.
    CHECK(live_timer_count() - timers_before == 1);
    process_lvgl(50);

    REQUIRE(call_count == 1);
    REQUIRE(value == 1); // first wins; schedule() would leave 3
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: schedule_once cannot be starved by re-requests",
                 "[coalesced_timer]") {
    // The reason the new method exists. A caller re-requesting every frame is
    // the normal case for a render path, and it must not push the deadline out.
    constexpr int kPeriod = 20;
    constexpr int kFrames = 12; // 60ms of virtual time at 5ms/frame — 3x the period

    int once_calls = 0;
    CoalescedTimer once_timer(kPeriod);
    for (int i = 0; i < kFrames; i++) {
        once_timer.schedule_once([&once_calls]() { once_calls++; });
        process_lvgl(5);
    }
    REQUIRE(once_calls >= 1);

    // Same loop, same period, schedule(): every call resets the timer, so it
    // never comes due while the requests keep arriving.
    int debounce_calls = 0;
    CoalescedTimer debounce_timer(kPeriod);
    for (int i = 0; i < kFrames; i++) {
        debounce_timer.schedule([&debounce_calls]() { debounce_calls++; });
        process_lvgl(5);
    }
    REQUIRE(debounce_calls == 0);

    // ...and it does fire once the burst stops, confirming the contrast above is
    // starvation and not a dead timer.
    process_lvgl(50);
    REQUIRE(debounce_calls == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: schedule_once re-arms after firing",
                 "[coalesced_timer]") {
    // The drop only applies while pending(). Once the callback has run the timer
    // is free again, otherwise a render path would coalesce to exactly one
    // recompute for the lifetime of the widget.
    int call_count = 0;
    CoalescedTimer timer(1);

    timer.schedule_once([&call_count]() { call_count++; });
    process_lvgl(50);
    REQUIRE(call_count == 1);

    timer.schedule_once([&call_count]() { call_count++; });
    REQUIRE(timer.pending());
    process_lvgl(50);
    REQUIRE(call_count == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: cancel releases a schedule_once claim",
                 "[coalesced_timer]") {
    int call_count = 0;
    CoalescedTimer timer(10);

    timer.schedule_once([&call_count]() { call_count++; });
    timer.cancel();
    REQUIRE_FALSE(timer.pending());

    // A cancelled claim must not lock out the next request.
    timer.schedule_once([&call_count]() { call_count++; });
    process_lvgl(50);
    REQUIRE(call_count == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "CoalescedTimer: destructor cancels a pending schedule_once",
                 "[coalesced_timer]") {
    // The lifetime argument for the two render-path callers: the timer is a
    // member of the object the callback reaches into, so the destructor closes
    // the window in which a pending callback could touch freed storage. Same
    // shape as the schedule() destructor case above.
    auto fired = std::make_shared<bool>(false);
    const void* timer_addr = nullptr;
    int pointing_before = 0;

    {
        CoalescedTimer timer(20);
        timer.schedule_once([fired]() { *fired = true; });
        timer_addr = &timer;
        pointing_before = lvgl_timers_pointing_at(timer_addr);
        REQUIRE(pointing_before >= 1);
    }

    REQUIRE(lvgl_timers_pointing_at(timer_addr) == pointing_before - 1);
    process_lvgl(200);
    REQUIRE_FALSE(*fired);
}
