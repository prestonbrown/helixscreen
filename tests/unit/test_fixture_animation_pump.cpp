// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: tests the LVGLTestFixture contract itself, which is test infrastructure by
// definition SPDX-License-Identifier: GPL-3.0-or-later
//
// process_lvgl() must advance LVGL animations.
//
// It did not, for a structural reason that was easy to miss: LVGL runs
// animations from a PERIODIC timer (repeat_count -1), lv_timer_handler_safe()
// pauses every timer to avoid the leaked-refresh-timer infinite loop, and its
// manual pump only executes one-shots (repeat_count > 0). The periodic anim
// timer therefore never ran, and lv_timer_handler() itself was called with
// everything paused.
//
// The consequence was not "animations are slow in tests" but "animations never
// finish". Anything whose completion hangs off an anim ready_cb never happened:
// a modal exit with animations enabled left its dialog parented to the screen
// permanently, so lv_obj_find_by_name() kept finding the OUTGOING dialog and
// tests silently asserted against the previous modal's content
// (test_afc_fault_path_modal.cpp did exactly that).
//
// Mutation check: drop the lv_anim_refr_now() call from lv_timer_handler_safe()
// and every test here fails.

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

namespace {

struct AnimProbe {
    int32_t value = -1;
    int ready_calls = 0;
};

void set_value_cb(void* var, int32_t v) {
    static_cast<AnimProbe*>(var)->value = v;
}

void ready_cb(lv_anim_t* a) {
    static_cast<AnimProbe*>(lv_anim_get_user_data(a))->ready_calls++;
}

/// Start a linear 0..100 animation over `duration_ms` on `probe`.
void start_anim(AnimProbe& probe, uint32_t duration_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &probe);
    lv_anim_set_user_data(&a, &probe);
    lv_anim_set_exec_cb(&a, set_value_cb);
    lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "process_lvgl advances a running animation",
                 "[core][fixture][animation]") {
    AnimProbe probe;
    start_anim(probe, 200);

    // Partway: the value must have moved off the start without reaching the end.
    process_lvgl(100);
    CHECK(probe.value > 0);
    CHECK(probe.value < 100);
    CHECK(probe.ready_calls == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "process_lvgl runs an animation to completion",
                 "[core][fixture][animation]") {
    AnimProbe probe;
    start_anim(probe, 100);

    process_lvgl(200);

    // The end value AND the ready callback — modal teardown, deferred deletes and
    // every other "finish" hook hang off ready_cb, not off the value.
    CHECK(probe.value == 100);
    CHECK(probe.ready_calls == 1);

    // A completed animation must also be off LVGL's list, or the next
    // lv_anim_delete(var) walks an animation whose var is already gone.
    CHECK(lv_anim_count_running() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "wait_until advances animations too",
                 "[core][fixture][animation]") {
    AnimProbe probe;
    start_anim(probe, 100);

    // wait_until is the helper for "a background thread will publish this"; it
    // pumps through the same path, so an animation-gated predicate must settle
    // rather than burn the whole budget.
    const bool done = wait_until([&] { return probe.ready_calls > 0; }, 2000);

    CHECK(done);
    CHECK(probe.value == 100);
}
