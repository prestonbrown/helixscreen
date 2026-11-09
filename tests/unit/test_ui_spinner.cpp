// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_spinner.cpp
 * @brief Unit tests for ui_spinner.cpp - Reactive spinner widget
 *
 * A spinner's arc reads the shared StyleRole::Spinner style rather than an
 * inline colour, so a theme change recolours every spinner in place.
 */

#include "ui_spinner.h"

#include "../lvgl_ui_test_fixture.h"
#include "lvgl_assert_handler.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// Helper: Create a dark mode test palette with configurable primary color
static ThemePalette make_dark_test_palette_with_primary(lv_color_t primary) {
    ThemePalette p = {};
    p.screen_bg = lv_color_hex(0x121212);
    p.overlay_bg = lv_color_hex(0x1A1A1A);
    p.card_bg = lv_color_hex(0x1E1E1E);
    p.elevated_bg = lv_color_hex(0x2D2D2D);
    p.border = lv_color_hex(0x424242);
    p.text = lv_color_hex(0xE0E0E0);
    p.text_muted = lv_color_hex(0xA0A0A0);
    p.text_subtle = lv_color_hex(0x808080);
    p.primary = primary;
    p.secondary = lv_color_hex(0x03DAC6);
    p.tertiary = lv_color_hex(0x6C757D);
    p.info = lv_color_hex(0x42A5F5);
    p.success = lv_color_hex(0x4CAF50);
    p.warning = lv_color_hex(0xFFA726);
    p.danger = lv_color_hex(0xEF5350);
    p.focus = lv_color_hex(0x4FC3F7);
    return p;
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_spinner: every spinner recolours with the shared style",
                 "[reactive-spinner][reactive]") {
    const char* attrs_sm[] = {"size", "sm", nullptr};
    const char* attrs_lg[] = {"size", "lg", nullptr};
    lv_obj_t* small = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs_sm));
    lv_obj_t* large = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs_lg));
    REQUIRE(small != nullptr);
    REQUIRE(large != nullptr);
    lv_style_t* shared_style = ThemeManager::instance().get_style(StyleRole::Spinner);
    REQUIRE(shared_style != nullptr);
    const lv_color_t before = lv_obj_get_style_arc_color(small, LV_PART_INDICATOR);

    ThemePalette palette = make_dark_test_palette_with_primary(lv_color_hex(0x9C27B0));
    auto& tm = ThemeManager::instance();
    tm.set_palettes(palette, palette);
    tm.set_dark_mode(true);
    lv_obj_report_style_change(nullptr);

    lv_style_value_t style_value;
    REQUIRE(lv_style_get_prop(shared_style, LV_STYLE_ARC_COLOR, &style_value) ==
            LV_STYLE_RES_FOUND);
    for (lv_obj_t* spinner : {small, large}) {
        CHECK(
            lv_color_eq(lv_obj_get_style_arc_color(spinner, LV_PART_INDICATOR), style_value.color));
    }
    CHECK_FALSE(lv_color_eq(before, style_value.color));

    lv_obj_delete(small);
    lv_obj_delete(large);
}

// ============================================================================
// Hidden spinners stop animating
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ui_spinner: a hidden spinner stops animating and resumes when shown",
                 "[ui_spinner]") {
    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 200, 200);
    const char* attrs[] = {"size", "md", nullptr};
    lv_obj_t* spinner = static_cast<lv_obj_t*>(lv_xml_create(box, "spinner", attrs));
    REQUIRE(spinner != nullptr);

    // The fixture pauses LVGL's refresh timer, so each draw is explicit.
    lv_refr_now(nullptr);
    process_lvgl(100);
    REQUIRE(lv_anim_get(spinner, nullptr) != nullptr);

    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    process_lvgl(100);
    CHECK(lv_anim_get(spinner, nullptr) == nullptr);

    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(spinner, nullptr) != nullptr);
    process_lvgl(100);
    CHECK(lv_anim_get(spinner, nullptr) != nullptr);

    lv_obj_delete(box);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_spinner: animated=false never starts animating when drawn",
                 "[ui_spinner]") {
    const char* attrs[] = {"size", "md", "animated", "false", nullptr};
    lv_obj_t* spinner = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs));
    REQUIRE(spinner != nullptr);

    lv_refr_now(nullptr);
    process_lvgl(100);
    CHECK(lv_anim_get(spinner, nullptr) == nullptr);

    lv_obj_delete(spinner);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ui_spinner: a frozen spinner stays still until unfrozen",
                 "[ui_spinner]") {
    struct FreezeGuard {
        ~FreezeGuard() {
            helix::ui::spinner_set_frozen(false);
        }
    } freeze_guard;

    const char* attrs[] = {"size", "md", nullptr};
    lv_obj_t* spinner = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "spinner", attrs));
    REQUIRE(spinner != nullptr);

    // What a remote-control freeze does: every animation gone, spinners held still.
    helix::ui::spinner_set_frozen(true);
    lv_anim_delete_all();
    lv_obj_invalidate(spinner);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(spinner, nullptr) == nullptr);

    helix::ui::spinner_set_frozen(false);
    lv_obj_invalidate(spinner);
    lv_refr_now(nullptr);
    CHECK(lv_anim_get(spinner, nullptr) != nullptr);

    lv_obj_delete(spinner);
}

namespace {
int g_lvgl_asserts = 0;

void count_lvgl_assert(const char* /*file*/, int /*line*/, const char* /*func*/) {
    ++g_lvgl_asserts;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ui_spinner: a parked spinner restarts on its draw without moving mid-render",
                 "[ui_spinner]") {
    // The restart runs inside the draw, where changing an angle invalidates the arc
    // during rendering. The arc keeps the angles it parked on until the next tick.
    struct AssertHook {
        helix_assert_callback_t prev = g_helix_assert_cpp_callback;
        AssertHook() {
            g_lvgl_asserts = 0;
            g_helix_assert_cpp_callback = count_lvgl_assert;
        }
        ~AssertHook() {
            g_helix_assert_cpp_callback = prev;
        }
    } hook;

    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 200, 200);
    const char* attrs[] = {"size", "md", nullptr};
    lv_obj_t* spinner = static_cast<lv_obj_t*>(lv_xml_create(box, "spinner", attrs));
    REQUIRE(spinner != nullptr);

    lv_refr_now(nullptr);
    process_lvgl(600); // mid-cycle: both edges have left their first-frame angles
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    process_lvgl(50);
    REQUIRE(lv_anim_get(spinner, nullptr) == nullptr);

    const lv_value_precise_t parked_start = lv_arc_get_angle_start(spinner);
    const lv_value_precise_t parked_end = lv_arc_get_angle_end(spinner);
    // The first frame starts the trailing edge at 0 and the leading edge at 45.
    REQUIRE(parked_start != 0);
    REQUIRE(parked_end != 45);

    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    REQUIRE(lv_anim_get(spinner, nullptr) != nullptr);
    CHECK(lv_arc_get_angle_start(spinner) == parked_start);
    CHECK(lv_arc_get_angle_end(spinner) == parked_end);
    CHECK(g_lvgl_asserts == 0);

    lv_obj_delete(box);
}
