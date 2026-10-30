// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_path_refresh_coalesce.cpp
 * @brief A burst of setters in one tick must schedule ONE canvas refresh.
 *
 * Every filament_path_canvas setter marks its layers dirty and asks for an
 * out-of-render-pass canvas refresh. A state update is never one setter — the
 * AMS panel pushes topology, slot count, active slot, segment and colour in the
 * same tick — so the scheduling path decides whether that costs one refresh or
 * one per setter.
 *
 * lv_async_call cannot make that decision: it mallocs an info struct and creates
 * a one-shot timer on every call, with no dedup on (cb, user_data). The
 * assertions below are on the timer count precisely because that is where the
 * per-call cost lands, and it is the thing an lv_async_call-based path cannot
 * satisfy.
 */

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"
#include "src/ui/ui_filament_path_internal.h"

#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

/// Length of LVGL's live timer list. Only deltas across a known-quiet window are
/// meaningful — the fixture and the widget both own unrelated timers, and
/// lv_timer_cancel_safe() leaves neutered entries linked until the next handler
/// pass reaps them.
int live_timer_count() {
    int n = 0;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        n++;
    }
    return n;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: a burst of setters schedules ONE refresh",
                 "[filament_path][coalesced_timer]") {
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);

    // Let widget construction, the SIZE_CHANGED mark and the initial refresh all
    // settle, so the measurement below sees only what the setters schedule.
    process_lvgl(60);

    const int before = live_timer_count();

    // Five distinct values through five distinct setters — every one of these
    // reaches layered_mark_dirty(), and none of them starts an animation.
    ui_filament_path_canvas_set_slot_width(path, 111);
    ui_filament_path_canvas_set_slot_count(path, 5);
    ui_filament_path_canvas_set_slot_overlap(path, 7);
    ui_filament_path_canvas_set_topology(path, 0);
    ui_filament_path_canvas_set_filament_color(path, 0x00FF00);

    // One pending refresh, not five. Against lv_async_call this reads 5.
    REQUIRE(live_timer_count() - before == 1);

    // ...and the refresh does run: coalescing must not swallow the work.
    process_lvgl(60);
    REQUIRE(live_timer_count() <= before);

    // A later burst is scheduled again — the claim is released when it fires,
    // not held for the life of the widget.
    const int after_drain = live_timer_count();
    ui_filament_path_canvas_set_slot_width(path, 222);
    ui_filament_path_canvas_set_slot_count(path, 6);
    REQUIRE(live_timer_count() - after_drain == 1);

    process_lvgl(60);
    lv_obj_delete(path);
    process_lvgl(30);
}

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: deleting the widget cancels a pending refresh",
                 "[filament_path][coalesced_timer]") {
    // The refresh callback reaches back through the widget pointer into
    // FilamentPathData, which the delete event frees. A refresh still pending at
    // that point must be retracted, not left to fire on the next tick.
    //
    // Asserted through the widget's own scheduling seam rather than a timer
    // count: cancel() routes through lv_timer_cancel_safe(), which neuters a
    // timer and leaves lv_timer_handler to reap it, so the entry is still linked
    // either way (same reason test_coalesced_timer.cpp compares pointers, not
    // counts). The flag is held in a shared_ptr so it outlives the widget.
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    process_lvgl(60);

    auto* data = helix::ui::fpath::get_data(path);
    REQUIRE(data != nullptr);

    auto fired = std::make_shared<bool>(false);
    data->layers.refresh_timer.schedule_once([fired]() { *fired = true; });
    REQUIRE(data->layers.refresh_timer.pending());

    lv_obj_delete(path);
    REQUIRE(helix::ui::fpath::get_data(path) == nullptr); // data freed

    process_lvgl(100);
    REQUIRE_FALSE(*fired);
}
