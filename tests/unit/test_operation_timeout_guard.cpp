// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_operation_timeout_guard.cpp
 * @brief Unit tests for OperationTimeoutGuard utility
 */

#include "../lvgl_test_fixture.h"
#include "operation_timeout_guard.h"
#include "subject_managed_panel.h"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Number of timers currently linked into LVGL's timer list.
int count_lvgl_timers() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        n++;
    }
    return n;
}

/// How many live LVGL timers carry @p p as their user_data. Compares pointers
/// only - never dereferences, so it is safe to ask about an object that has
/// already been destroyed.
///
/// Counted rather than tested as a boolean on purpose. lv_timer_cancel_safe()
/// (used widely elsewhere) neuters a timer without unlinking it or clearing its
/// user_data, and the linked-but-dead entry survives until the next
/// lv_timer_handler pass. Earlier tests therefore leave timers pointing at their
/// own dead stack frames, and those addresses get reused. Only the *delta*
/// across a destructor is meaningful.
int lvgl_timers_pointing_at(const void* p) {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (lv_timer_get_user_data(t) == p) {
            n++;
        }
    }
    return n;
}

} // namespace

// ============================================================================
// Basic State Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: default state is inactive",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;
    REQUIRE_FALSE(guard.is_active());
    REQUIRE(guard.subject() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: begin sets active",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;

    guard.begin(30000, [] {});
    REQUIRE(guard.is_active());

    guard.end();
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: end clears active",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;

    guard.begin(30000, [] {});
    REQUIRE(guard.is_active());

    guard.end();
    REQUIRE_FALSE(guard.is_active());
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: double end is harmless",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;

    guard.begin(30000, [] {});
    guard.end();
    guard.end(); // Should not crash

    REQUIRE_FALSE(guard.is_active());
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: begin replaces active state",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;

    guard.begin(30000, [] {});
    REQUIRE(guard.is_active());

    // Second begin should still be active (replaces first timer)
    guard.begin(30000, [] {});
    REQUIRE(guard.is_active());

    guard.end();
    REQUIRE_FALSE(guard.is_active());
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: end without begin is safe",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;
    guard.end(); // Should not crash
    REQUIRE_FALSE(guard.is_active());
}

// ============================================================================
// Subject Integration Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: init_subject registers subject",
                 "[operation_timeout_guard]") {
    SubjectManager subjects;
    OperationTimeoutGuard guard;
    guard.init_subject("test_guard_subject", subjects);

    REQUIRE(guard.subject() != nullptr);
    REQUIRE(lv_subject_get_int(guard.subject()) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: begin sets subject to 1",
                 "[operation_timeout_guard]") {
    SubjectManager subjects;
    OperationTimeoutGuard guard;
    guard.init_subject("test_guard_begin_subject", subjects);

    guard.begin(30000, [] {});
    REQUIRE(lv_subject_get_int(guard.subject()) == 1);

    guard.end();
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: end sets subject to 0",
                 "[operation_timeout_guard]") {
    SubjectManager subjects;
    OperationTimeoutGuard guard;
    guard.init_subject("test_guard_end_subject", subjects);

    guard.begin(30000, [] {});
    REQUIRE(lv_subject_get_int(guard.subject()) == 1);

    guard.end();
    REQUIRE(lv_subject_get_int(guard.subject()) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: without subject begin/end work",
                 "[operation_timeout_guard]") {
    OperationTimeoutGuard guard;
    REQUIRE(guard.subject() == nullptr);

    guard.begin(30000, [] {});
    REQUIRE(guard.is_active());

    guard.end();
    REQUIRE_FALSE(guard.is_active());
    REQUIRE(guard.subject() == nullptr);
}

// A destructor that leaks its lv_timer is a live main-thread crash source: the
// timer stays linked, comes due, and timer_callback() dereferences the freed
// guard through lv_timer_get_user_data(). Nothing in a plain build notices unless
// a test pumps LVGL past the deadline afterwards, which is what these two do.
//
// The dangling-user_data check runs first and deliberately before any pumping: it
// is a pointer comparison, so it fails cleanly on a leak instead of failing by
// running the leaked callback into freed memory.

TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: destructor cancels the pending timer",
                 "[operation_timeout_guard]") {
    auto fired = std::make_shared<bool>(false);
    const int timers_before = count_lvgl_timers();
    const void* guard_addr = nullptr;
    int pointing_before = 0;

    {
        OperationTimeoutGuard guard;
        guard_addr = &guard;
        guard.begin(20, [fired] { *fired = true; });

        REQUIRE(guard.is_active());
        REQUIRE(count_lvgl_timers() == timers_before + 1);
        pointing_before = lvgl_timers_pointing_at(guard_addr);
        REQUIRE(pointing_before >= 1);
        // guard destroyed here with the timer still armed
    }

    // cancel_timer() uses lv_timer_delete(), so both are exact immediately.
    REQUIRE(lvgl_timers_pointing_at(guard_addr) == pointing_before - 1);
    REQUIRE(count_lvgl_timers() == timers_before);

    // Well past the 20ms deadline.
    process_lvgl(200);
    REQUIRE_FALSE(*fired);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "OperationTimeoutGuard: destructor cancels the timer when a subject is bound",
                 "[operation_timeout_guard]") {
    auto fired = std::make_shared<bool>(false);
    const int timers_before = count_lvgl_timers();
    const void* guard_addr = nullptr;
    int pointing_before = 0;

    {
        // Declaration order matters and is the reverse of what reads naturally:
        // subject_ lives inside the guard, so SubjectManager must deinit while the
        // guard is still alive. Declaring guard first destroys subjects first.
        //
        // Nothing is torn down explicitly - no end(), no deinit_all(). The
        // destructor is the thing under test, so it has to be left something to
        // get wrong.
        OperationTimeoutGuard guard;
        SubjectManager subjects;
        guard.init_subject("test_guard_dtor_subject", subjects);
        guard.begin(20, [fired] { *fired = true; });
        guard_addr = &guard;

        REQUIRE(guard.is_active());
        REQUIRE(lv_subject_get_int(guard.subject()) == 1);
        REQUIRE(count_lvgl_timers() == timers_before + 1);
        pointing_before = lvgl_timers_pointing_at(guard_addr);
        REQUIRE(pointing_before >= 1);
    }

    REQUIRE(lvgl_timers_pointing_at(guard_addr) == pointing_before - 1);
    REQUIRE(count_lvgl_timers() == timers_before);

    process_lvgl(200);
    REQUIRE_FALSE(*fired);
}

// LVGL dispatch canaries. process_lvgl() runs due one-shots itself and never
// enters LVGL's dispatch loop, so these call lv_timer_handler() directly, with
// every timer they did not create paused so the pass runs theirs alone.

namespace {

bool timer_linked(const lv_timer_t* timer) {
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (t == timer) {
            return true;
        }
    }
    return false;
}

/// One lv_timer_handler() pass over @p ours alone.
void run_handler_over(std::initializer_list<lv_timer_t*> ours) {
    std::vector<lv_timer_t*> paused_here;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (std::find(ours.begin(), ours.end(), t) == ours.end() && !lv_timer_get_paused(t)) {
            lv_timer_pause(t);
            paused_here.push_back(t);
        }
    }

    lv_tick_inc(50);
    lv_timer_handler();

    for (lv_timer_t* t : paused_here) {
        lv_timer_resume(t);
    }
}

} // namespace

// A general pin on LVGL's own contract: lv_timer_exec() re-checks
// state.timer_deleted after a callback, so a one-shot deleting itself is not
// deleted a second time.
TEST_CASE_METHOD(LVGLTestFixture,
                 "OperationTimeoutGuard: LVGL tolerates a timer deleting itself in its callback",
                 "[operation_timeout_guard]") {
    const int timers_before = count_lvgl_timers();
    int runs = 0;

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            ++*static_cast<int*>(lv_timer_get_user_data(t));
            lv_timer_delete(t);
        },
        20, &runs);
    lv_timer_set_repeat_count(timer, 1);
    REQUIRE(count_lvgl_timers() == timers_before + 1);

    run_handler_over({timer});

    if (runs == 0 && timer_linked(timer)) {
        lv_timer_delete(timer); // it still points at this frame's runs
    }
    REQUIRE(runs == 1);
    REQUIRE(count_lvgl_timers() == timers_before);
}

// Canary for OperationTimeoutGuard::cancel_timer(). lv_timer_create() inserts at
// the head, so a one-shot created after the guard's timer runs first with the
// guard's timer as the handler's saved next. Ending the guard from that callback
// deletes the saved next, which is safe only because lv_timer_handler() restarts
// its walk once state.timer_deleted is set. Losing the restart is a use-after-free
// that a plain build need not notice; AddressSanitizer does.
TEST_CASE_METHOD(LVGLTestFixture, "OperationTimeoutGuard: another timer's callback ends the guard",
                 "[operation_timeout_guard]") {
    const int timers_before = count_lvgl_timers();
    auto fired = std::make_shared<bool>(false);
    auto guard = std::make_unique<OperationTimeoutGuard>();
    guard->begin(60000, [fired] { *fired = true; });
    lv_timer_t* guard_timer = guard->pending_timer();
    REQUIRE(guard_timer != nullptr);

    lv_timer_t* ender = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<std::unique_ptr<OperationTimeoutGuard>*>(lv_timer_get_user_data(t))
                ->reset();
        },
        0, &guard);
    lv_timer_set_repeat_count(ender, 1);
    REQUIRE(lv_timer_get_next(ender) == guard_timer);

    run_handler_over({ender, guard_timer});

    // The restart is also the pass that retires the spent one-shot, so it must
    // be gone here; it points at this frame's guard, so it never outlives it.
    const bool ender_left = timer_linked(ender);
    if (ender_left) {
        lv_timer_delete(ender);
    }
    REQUIRE_FALSE(ender_left);
    REQUIRE(guard == nullptr);
    REQUIRE_FALSE(*fired);
    REQUIRE(count_lvgl_timers() == timers_before);
}
