// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_keyboard_field_avoidance.cpp
 * @brief The on-screen keyboard lifts the page so the focused field stays above
 *        it, including when focus moves between fields while the keyboard is up.
 */

#include "ui_keyboard_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "lvgl/lvgl.h"

#include "../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;

namespace {

/// Owns the process-wide KeyboardManager, an input group and the animation
/// setting for one test, and puts all of them back afterwards.
struct ScopedAnimatedKeyboard {
    lv_group_t* group = nullptr;
    lv_group_t* prior_default = nullptr;
    bool prior_animations = false;

    explicit ScopedAnimatedKeyboard(lv_obj_t* parent) {
        auto& settings = DisplaySettingsManager::instance();
        prior_animations = settings.get_animations_enabled();
        settings.set_animations_enabled(true);
        prior_default = lv_group_get_default();
        group = lv_group_create();
        lv_group_set_default(group);
        release_keyboard();
        KeyboardManager::instance().init(parent);
    }
    ~ScopedAnimatedKeyboard() {
        release_keyboard();
        DisplaySettingsManager::instance().set_animations_enabled(prior_animations);
        lv_group_set_default(prior_default);
        lv_group_delete(group);
    }

    static void release_keyboard() {
        KeyboardManager::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// A registered one-line field at (x, y) inside @p parent.
lv_obj_t* make_field(lv_obj_t* parent, int32_t x, int32_t y) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_size(ta, 76, 31);
    lv_obj_set_pos(ta, x, y);
    KeyboardManager::instance().register_textarea(ta);
    return ta;
}

int32_t bottom_of(lv_obj_t* obj) {
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a.y2;
}

int32_t keyboard_top() {
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    lv_obj_update_layout(kb);
    lv_area_t a;
    lv_obj_get_coords(kb, &a);
    return a.y1;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Moving focus between low fields keeps the new field above the keyboard",
                 "[keyboard][keyboard_avoidance]") {
    auto [w, h] =
        GENERATE(std::pair<int32_t, int32_t>{480, 320}, std::pair<int32_t, int32_t>{800, 480});
    CAPTURE(w, h);
    ScopedResolution res(lv_display_get_default(), w, h);
    lv_obj_set_size(test_screen(), w, h);
    ScopedAnimatedKeyboard kb(test_screen());

    // A full-screen overlay with two fields on one row near its bottom edge, the
    // way the slot editor lays out its weight fields.
    lv_obj_t* overlay = lv_obj_create(test_screen());
    lv_obj_set_size(overlay, w, h);
    lv_obj_set_pos(overlay, 0, 0);
    const int32_t row_y = h - 50;
    lv_obj_t* remaining = make_field(overlay, 10, row_y);
    lv_obj_t* spool_weight = make_field(overlay, 100, row_y);
    lv_obj_update_layout(test_screen());

    lv_group_focus_obj(remaining);
    process_lvgl(400);
    REQUIRE(KeyboardManager::instance().is_visible());
    REQUIRE(bottom_of(remaining) < keyboard_top());

    // A tap on the neighbouring field defocuses the first (hide) and focuses the
    // second (show) in one input event, with the page still lifted.
    lv_group_focus_obj(spool_weight);
    process_lvgl(400);
    REQUIRE(KeyboardManager::instance().is_visible());
    CHECK(bottom_of(spool_weight) < keyboard_top());

    // Closing the keyboard puts the page back where it was.
    KeyboardManager::instance().hide();
    process_lvgl(400);
    lv_obj_update_layout(overlay);
    CHECK(lv_obj_get_y(overlay) == 0);

    lv_obj_delete(overlay);
}

TEST_CASE_METHOD(LVGLUITestFixture, "A field already above the keyboard leaves the page alone",
                 "[keyboard][keyboard_avoidance]") {
    ScopedAnimatedKeyboard kb(test_screen());

    // A screen child resting below the top on purpose.
    lv_obj_t* placed = lv_obj_create(test_screen());
    lv_obj_set_y(placed, 37);
    lv_obj_t* high = make_field(test_screen(), 10, 10);
    lv_obj_update_layout(test_screen());
    REQUIRE(lv_obj_get_y(placed) == 37);

    lv_group_focus_obj(high);
    process_lvgl(400);
    REQUIRE(KeyboardManager::instance().is_visible());
    REQUIRE(bottom_of(high) < keyboard_top());
    lv_obj_update_layout(placed);
    CHECK(lv_obj_get_y(placed) == 37);

    lv_group_focus_obj(nullptr);
    lv_obj_delete(high);
    lv_obj_delete(placed);
}

TEST_CASE_METHOD(LVGLUITestFixture, "A focused textarea deleted hides the keyboard",
                 "[keyboard][1983]") {
    ScopedAnimatedKeyboard scoped(test_screen());
    lv_obj_t* textarea = lv_textarea_create(test_screen());
    auto& keyboard = KeyboardManager::instance();
    // Outside any group, so no DEFOCUSED arrives: only the delete hook can hide it.
    lv_group_t* group = lv_group_get_default();
    lv_group_set_default(nullptr);
    keyboard.register_textarea(textarea);
    lv_group_set_default(group);
    keyboard.show(textarea);
    REQUIRE(keyboard.is_visible());

    lv_obj_delete(textarea);
    process_lvgl(400);

    CHECK_FALSE(keyboard.is_visible());
}
