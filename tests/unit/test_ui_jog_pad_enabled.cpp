// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_jog_pad_enabled.cpp
 * @brief Tests for ui_jog_pad_set_enabled().
 *
 * The jog pad is a custom-drawn widget with no XML disabled binding. When the
 * printer is not ready, ui_jog_pad_set_enabled(pad, false) adds LV_STATE_DISABLED
 * so LVGL's input handling stops routing presses/clicks to the pad (verified in
 * lv_indev.c, which gates delivery on !lv_obj_has_state(obj, LV_STATE_DISABLED))
 * and the draw callback overlays a dimming scrim. This exercises that toggle.
 */

#include "../../include/ui_jog_pad.h"
#include "../../include/ui_panel_motion.h" // helix::JogMode
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct LabelTaskInfo {
    std::string text;
    uint32_t text_local;
    lv_color_t color;
};

/// Records every label draw task the pad enqueues during a refresh. The text
/// is copied here rather than after the refresh: a task that owns its string
/// frees it when the task is destroyed.
void capture_label_tasks(lv_event_t* e) {
    lv_draw_task_t* task = lv_event_get_draw_task(e);
    if (!task || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) {
        return;
    }
    auto* out = static_cast<std::vector<LabelTaskInfo>*>(lv_event_get_user_data(e));
    const lv_draw_label_dsc_t* dsc = lv_draw_task_get_label_dsc(task);
    out->push_back({dsc->text ? dsc->text : "", dsc->text_local, dsc->color});
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "ui_jog_pad_set_enabled toggles LV_STATE_DISABLED",
                 "[jog_pad][ui]") {
    lv_obj_t* pad = ui_jog_pad_create(lv_screen_active());
    REQUIRE(pad != nullptr);

    // Created enabled: input flows to the pad normally.
    CHECK_FALSE(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Disable: LV_STATE_DISABLED set -> indev skips press/click, scrim drawn.
    ui_jog_pad_set_enabled(pad, false);
    CHECK(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Re-enable: state cleared.
    ui_jog_pad_set_enabled(pad, true);
    CHECK_FALSE(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Idempotent: re-enabling an already-enabled pad is a no-op, not a crash.
    ui_jog_pad_set_enabled(pad, true);
    CHECK_FALSE(lv_obj_has_state(pad, LV_STATE_DISABLED));

    // Null-safe.
    ui_jog_pad_set_enabled(nullptr, false);

    lv_obj_delete(pad);
}

TEST_CASE_METHOD(LVGLTestFixture, "Jog pad label draw tasks own their text", "[jog_pad][ui]") {
    lv_obj_t* pad = ui_jog_pad_create(lv_screen_active());
    REQUIRE(pad != nullptr);
    lv_obj_set_size(pad, 200, 200);
    ui_jog_pad_set_mode(pad, helix::JogMode::Coarse);

    std::vector<LabelTaskInfo> tasks;
    // LV_EVENT_DRAW_TASK_ADDED is sent only for objects carrying this flag
    // (lv_draw.c gates on it), and no widget sets it by default.
    lv_obj_set_send_draw_task_events(pad, true);
    lv_obj_add_event_cb(pad, capture_label_tasks, LV_EVENT_DRAW_TASK_ADDED, &tasks);

    // Some displays in this suite are created without a flush callback, so a
    // refresh of one never completes. Give this one a callback that reports the
    // flush done, matching test_label_scroll_motion.cpp's helper.
    lv_display_t* display = lv_obj_get_display(pad);
    lv_display_set_flush_cb(
        display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    lv_obj_invalidate(pad);
    lv_refr_now(display);

    lv_obj_remove_event_cb_with_user_data(pad, capture_label_tasks, &tasks);

    // The draw callback must actually have run. Without this the rest of the
    // assertions pass vacuously on an empty vector.
    REQUIRE_FALSE(tasks.empty());

    // The pad's ring labels are formatted into a by-value struct that dies with
    // the draw callback's frame, while on threaded builds the render thread reads
    // the text later, so each of those tasks must own its own copy. The axis
    // labels are string literals with static storage and need no copy.
    const helix::JogModeDistances dist = helix::get_jog_mode_distances(helix::JogMode::Coarse);
    int ring_labels = 0;
    int axis_labels = 0;
    for (const auto& t : tasks) {
        if (t.text == dist.inner_label || t.text == dist.outer_label) {
            ++ring_labels;
            CHECK(t.text_local == 1);
        } else if (t.text == "Y+" || t.text == "X+" || t.text == "Y-" || t.text == "X-") {
            ++axis_labels;
            CHECK(t.text_local == 0);
        }
    }
    // Both kinds must actually have been drawn, or the loop above says nothing.
    CHECK(ring_labels > 0);
    CHECK(axis_labels > 0);

    lv_obj_delete(pad);
}

// Each distance label reads on the fill it sits on: the inner one on the
// primary circle, the outer one on the secondary ring.
TEST_CASE_METHOD(LVGLUITestFixture, "Jog pad distance labels read on their own ring",
                 "[jog_pad][ui]") {
    lv_obj_t* pad = ui_jog_pad_create(lv_screen_active());
    REQUIRE(pad != nullptr);
    lv_obj_set_size(pad, 200, 200);
    ui_jog_pad_set_mode(pad, helix::JogMode::Coarse);

    std::vector<LabelTaskInfo> tasks;
    lv_obj_set_send_draw_task_events(pad, true);
    lv_obj_add_event_cb(pad, capture_label_tasks, LV_EVENT_DRAW_TASK_ADDED, &tasks);
    lv_display_t* display = lv_obj_get_display(pad);
    lv_display_set_flush_cb(
        display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    lv_obj_invalidate(pad);
    lv_refr_now(display);
    lv_obj_remove_event_cb_with_user_data(pad, capture_label_tasks, &tasks);

    // The two distance labels can carry the same text, so tell them apart by
    // draw order: each pass draws the inner one, then the outer one.
    const helix::JogModeDistances dist = helix::get_jog_mode_distances(helix::JogMode::Coarse);
    const lv_color_t inner_fill = theme_manager_get_color("primary");
    const lv_color_t outer_fill = theme_manager_get_color("secondary");
    const lv_color_t text = theme_manager_get_color("text");
    const uint32_t want_inner =
        lv_color_to_u32(theme_manager_get_contrast_adjusted_text(text, inner_fill));
    const uint32_t want_outer =
        lv_color_to_u32(theme_manager_get_contrast_adjusted_text(text, outer_fill));
    // Precondition: the two fills want different colours, so a label painted
    // for the wrong ring shows up below.
    REQUIRE(want_inner != want_outer);
    int inner = 0;
    int outer = 0;
    bool next_is_inner = true;
    for (const auto& t : tasks) {
        if (t.text != dist.inner_label && t.text != dist.outer_label) {
            continue;
        }
        if (next_is_inner) {
            ++inner;
            CHECK(lv_color_to_u32(t.color) == want_inner);
        } else {
            ++outer;
            CHECK(lv_color_to_u32(t.color) == want_outer);
        }
        next_is_inner = !next_is_inner;
    }
    REQUIRE(inner > 0);
    REQUIRE(outer == inner);

    lv_obj_delete(pad);
}
