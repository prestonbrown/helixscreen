// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_wake_input_gate.cpp
 * @brief #1245 — the wake gate must CANCEL the in-flight press, not just pause it.
 *
 * On Android the sleep overlay is not clickable and the sleep-aware input
 * wrapper is compiled out (HELIX_DISPLAY_SDL), so the touch that wakes the
 * screen hit-tests straight through to whatever is under it on the home panel.
 * wake_display() calls disable_input_briefly(), which used to only flip
 * lv_indev_enable(indev, false) — a pure flag write
 * (lib/lvgl/src/indev/lv_indev.c). pr_timestamp, long_pr_sent and
 * pointer.act_obj all survive the 200ms blackout, so a finger still on the
 * glass when input came back produced LV_EVENT_LONG_PRESSED measured from the
 * ORIGINAL touch-down — home-grid edit mode, opened behind the lock screen and
 * discovered once the PIN cleared.
 *
 * The gate has to issue lv_indev_reset() too. That is honoured even while the
 * indev is disabled: lv_indev_read() runs indev_proc_reset_query_handler()
 * BEFORE the `if(indev->enabled == 0) return;` gate.
 *
 * Both halves are asserted here — the control case (the same held press DOES
 * long-press when the gate is never engaged) exists so the regression assertion
 * cannot pass vacuously on a harness that never drove LVGL at all.
 *
 * Harness note: reads and the clock are driven by hand rather than through
 * process_lvgl(). The fixture's pump (lv_timer_handler_safe in
 * tests/ui_test_utils.cpp) deliberately runs ONLY one-shot timers, and both the
 * indev read timer and the gate's own re-enable timer are periodic — under
 * process_lvgl() neither ever fires, and the whole test would be vacuous. The
 * gate's timer is therefore located and invoked directly, so the production
 * reenable_input_cb still runs.
 */

#include "app_constants.h"
#include "display_manager.h"
#include "lvgl_test_fixture.h"
#include "misc/lv_timer_private.h" // timer_cb/period — to fire the gate's own re-enable timer
#include "test_helpers/display_manager_test_access.h"

#include <algorithm>
#include <vector>

#include "../../catch_amalgamated.hpp"

namespace {

// Where the synthetic finger lands, and the target it lands on. Well inside the
// 800x480 fixture display.
constexpr int TOUCH_X = 120;
constexpr int TOUCH_Y = 120;
constexpr int TARGET_POS = 60;
constexpr int TARGET_SIZE = 160;

// Virtual-clock step between synthetic reads. Finer than the 33ms production
// read period so the long-press threshold is crossed with little overshoot.
constexpr int READ_STEP_MS = 10;

// The blackout disable_input_briefly() schedules.
constexpr int BLACKOUT_MS = 200;

/// State the synthetic indev's read callback reports. File-scope because LVGL
/// retains the callback for as long as the indev lives; ScopedWakeIndev bounds
/// that to one test.
struct WakeIndevState {
    lv_point_t point{0, 0};
    lv_indev_state_t state{LV_INDEV_STATE_RELEASED};
};

WakeIndevState g_wake_indev_state;

void wake_indev_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = g_wake_indev_state.point;
    data->state = g_wake_indev_state.state;
}

int g_pressed_count = 0;
int g_long_pressed_count = 0;

void count_pressed_cb(lv_event_t* /*e*/) {
    ++g_pressed_count;
}

void count_long_pressed_cb(lv_event_t* /*e*/) {
    ++g_long_pressed_count;
}

/// Owns a pointer indev for one test. lv_indev_create() also arms a periodic
/// read timer; leaving the indev behind would keep that timer (and this
/// callback) alive for the rest of the binary's run.
class ScopedWakeIndev {
  public:
    ScopedWakeIndev() {
        indev_ = lv_indev_create();
        lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev_, wake_indev_read_cb);
        // Production value, applied in DisplayManager::init()
        // (src/application/display_manager.cpp) — LVGL's own default is 400ms.
        lv_indev_set_long_press_time(indev_, AppConstants::Input::LONG_PRESS_MS);
    }
    ~ScopedWakeIndev() {
        g_wake_indev_state.state = LV_INDEV_STATE_RELEASED;
        lv_indev_delete(indev_);
    }
    ScopedWakeIndev(const ScopedWakeIndev&) = delete;
    ScopedWakeIndev& operator=(const ScopedWakeIndev&) = delete;

    /// Hold the current touch state for @p ms of virtual time, reading the
    /// device the way LVGL's own read timer would.
    void hold(int ms) const {
        for (int elapsed = 0; elapsed < ms; elapsed += READ_STEP_MS) {
            lv_tick_inc(READ_STEP_MS);
            lv_indev_read(indev_);
        }
    }

    lv_indev_t* get() const {
        return indev_;
    }

  private:
    lv_indev_t* indev_ = nullptr;
};

/// Target the synthetic finger presses, wired to the two event counters.
lv_obj_t* make_press_target(lv_obj_t* parent) {
    lv_obj_t* target = lv_obj_create(parent);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(target, TARGET_POS, TARGET_POS);
    lv_obj_set_size(target, TARGET_SIZE, TARGET_SIZE);
    lv_obj_add_event_cb(target, count_pressed_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(target, count_long_pressed_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_update_layout(target);
    return target;
}

std::vector<lv_timer_t*> snapshot_timers() {
    std::vector<lv_timer_t*> timers;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        timers.push_back(t);
    }
    return timers;
}

/// The single timer that appeared since @p before was taken.
lv_timer_t* timer_added_since(const std::vector<lv_timer_t*>& before) {
    lv_timer_t* found = nullptr;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (std::find(before.begin(), before.end(), t) == before.end()) {
            if (found != nullptr) {
                return nullptr; // ambiguous — caller asserts on non-null
            }
            found = t;
        }
    }
    return found;
}

void reset_counters() {
    g_pressed_count = 0;
    g_long_pressed_count = 0;
}

} // namespace

// ============================================================================
// Control case — proves the harness really drives LVGL's long-press machinery.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "held press fires LONG_PRESSED once with no wake gate engaged",
                 "[application][display][input][wake][1245]") {
    reset_counters();
    ScopedWakeIndev indev;
    make_press_target(test_screen());

    g_wake_indev_state.point = {TOUCH_X, TOUCH_Y};
    g_wake_indev_state.state = LV_INDEV_STATE_PRESSED;

    // Press registers on the target.
    indev.hold(100);
    REQUIRE(g_pressed_count == 1);
    REQUIRE(g_long_pressed_count == 0); // 100ms < the 500ms threshold

    // Same 700ms total hold the gated test below uses.
    indev.hold(600);

    // Exactly once — further holding produces LONG_PRESSED_REPEAT, not LONG_PRESSED.
    REQUIRE(g_long_pressed_count == 1);
}

// ============================================================================
// Regression — #1245: the gate must cancel the press, not merely pause input.
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "wake gate cancels the in-flight press so no LONG_PRESSED survives the blackout",
                 "[application][display][input][wake][1245]") {
    reset_counters();
    ScopedWakeIndev indev;
    make_press_target(test_screen());

    g_wake_indev_state.point = {TOUCH_X, TOUCH_Y};
    g_wake_indev_state.state = LV_INDEV_STATE_PRESSED;

    // The wake touch lands and LVGL starts counting toward the long press.
    indev.hold(100);
    REQUIRE(g_pressed_count == 1);
    REQUIRE(g_long_pressed_count == 0);

    // wake_display() engages the gate while the finger is still down.
    DisplayManager mgr;
    const std::vector<lv_timer_t*> before = snapshot_timers();
    DisplayManagerTestAccess::disable_input_briefly(mgr);

    lv_timer_t* gate_timer = timer_added_since(before);
    REQUIRE(gate_timer != nullptr);
    REQUIRE(gate_timer->period == BLACKOUT_MS);

    // Blackout: input is off, but reads still run — that is what carries the
    // reset through (the handler precedes the enabled check).
    indev.hold(BLACKOUT_MS);

    // Production's re-enable. Fired directly because the fixture's timer pump
    // never runs periodic timers (see the file header).
    gate_timer->timer_cb(gate_timer);

    // Finger stays down well past the 500ms threshold measured from the
    // ORIGINAL touch-down at t=10ms. Without the reset, LONG_PRESSED lands at
    // t=510ms — 210ms into this window. With it, the press restarts at t=310ms
    // and could not long-press before t=810ms, past the end of the hold.
    indev.hold(400);

    REQUIRE(g_long_pressed_count == 0);
}
