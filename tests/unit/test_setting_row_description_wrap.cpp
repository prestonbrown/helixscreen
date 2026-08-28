// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: instantiates the shipped ui_xml/setting_*_row.xml components
//                 through lv_xml_create() and asserts on real LVGL geometry.
//                 ../lvgl_ui_test_fixture.h pulls in include/moonraker_api.h.

/**
 * @file test_setting_row_description_wrap.cpp
 * @brief Pins the setting rows' description/label wrapping contract.
 *
 * A settings row's description is `text_small`, i.e. a plain lv_label. A label
 * left at its default LV_SIZE_CONTENT width lays out on ONE line no matter how
 * long the string is, and the overflow is clipped by the parent — invisibly, so
 * the row looks fine in English on a wide panel and silently truncates on a
 * narrow one or in a longer language.
 *
 * Wrapping needs BOTH halves and neither works alone:
 *   - the label needs width="100%" + long_mode="wrap", and
 *   - the flex_grow column above it needs width="0", because a `width="100%"`
 *     child is dropped from its parent's content-width calculation
 *     (`w_ignore_size`, lv_obj_pos.c). Leave the column at content width and it
 *     collapses to its widest non-percentage child instead.
 *
 * The strings below are the real longest descriptions shipped in ui_xml/ (98 ch
 * for the toggle row, 130 ch for the slider row); de/es/fr run 15-29% longer
 * again, so these are the floor, not the ceiling.
 */

#include "../lvgl_ui_test_fixture.h"
#include "lvgl.h"

#include "../catch_amalgamated.hpp"

namespace {

void no_op_cb(lv_event_t*) {}

/// Longest description actually shipped on a setting_toggle_row
/// (ui_xml/settings_safety_overlay.xml, row_allow_cold_extrude).
constexpr const char* LONG_TOGGLE_DESC =
    "Run filament load/unload even when the nozzle is cold (for macros that heat the nozzle "
    "themselves)";

/// Longest description shipped on a setting_slider_row
/// (ui_xml/settings_touch_overlay.xml, long-press duration).
constexpr const char* LONG_SLIDER_DESC =
    "How long to hold before a press counts as a long-press, in ms (raise if edit mode or other "
    "long-press actions trigger by accident)";

/// Width of the settings column the rows live in. Every consumer nests the row
/// under `<lv_obj width="100%" flex_flow="column" flex_grow="1" scrollable="true">`,
/// so a fixed-width column is a faithful stand-in. 380px is roughly what a
/// 480px-wide panel leaves after the setting_group margins.
constexpr int32_t COLUMN_W = 380;

class SettingRowWrapFixture : public LVGLUITestFixture {
  public:
    lv_subject_t val_subject;
    lv_subject_t dis_subject;

    SettingRowWrapFixture() {
        lv_xml_register_event_cb(nullptr, "test_row_wrap_noop", no_op_cb);
        lv_subject_init_int(&val_subject, 0);
        lv_xml_register_subject(nullptr, "test_row_wrap_val", &val_subject);
        lv_subject_init_int(&dis_subject, 0);
        lv_xml_register_subject(nullptr, "test_row_wrap_dis", &dis_subject);
    }

    ~SettingRowWrapFixture() override {
        lv_subject_deinit(&val_subject);
        lv_subject_deinit(&dis_subject);
    }

    /// A fixed-width, content-height column standing in for a settings list.
    lv_obj_t* make_column() {
        lv_obj_t* col = lv_obj_create(lv_screen_active());
        lv_obj_set_width(col, COLUMN_W);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        return col;
    }

    static int32_t line_height(lv_obj_t* label) {
        const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        REQUIRE(font != nullptr);
        return lv_font_get_line_height(font);
    }
};

/// Absolute screen coords. lv_obj_get_x2()/y2() are measured from the object's
/// OWN parent, so they cannot be compared between a label and the row above it.
lv_area_t coords_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}

/// Assert a label wrapped to more than one line and stayed inside `bounds`.
void check_wrapped_inside(lv_obj_t* label, lv_obj_t* bounds, int32_t lh, const char* what) {
    INFO(what);
    const lv_area_t l = coords_of(label);
    const lv_area_t b = coords_of(bounds);
    const int32_t h = lv_obj_get_height(label);
    INFO("label " << lv_obj_get_width(label) << "x" << h << ", line height " << lh);

    // Wrapped: taller than a single line.
    CHECK(h >= 2 * lh);
    // Contained: never wider than, nor spilling past, the row it sits in.
    CHECK(lv_obj_get_width(label) <= lv_obj_get_width(bounds));
    CHECK(l.x2 <= b.x2);
}

} // namespace

TEST_CASE_METHOD(SettingRowWrapFixture, "setting_toggle_row wraps a long description",
                 "[xml][settings][layout][i18n]") {
    lv_obj_t* col = make_column();

    const char* attrs[] = {
        "name",        "row_under_test",     "label",    "Allow cold load/unload",
        "subject",     "test_row_wrap_val",  "disabled", "test_row_wrap_dis",
        "callback",    "test_row_wrap_noop", "icon",     "flash",
        "description", LONG_TOGGLE_DESC,     nullptr};
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(col, "setting_toggle_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_update_layout(col);

    lv_obj_t* desc = lv_obj_find_by_name(row, "description");
    REQUIRE(desc != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(desc, LV_OBJ_FLAG_HIDDEN)); // 800x480 -> Medium, description on

    check_wrapped_inside(desc, row, line_height(desc), "toggle row description");

    SECTION("the row grew tall enough to actually show the wrapped text") {
        // The row is height="content"; if it did not grow, the extra lines are
        // clipped and wrapping bought nothing.
        CHECK(lv_obj_get_height(row) >= lv_obj_get_height(desc));
        CHECK(coords_of(desc).y2 <= coords_of(row).y2);
    }

    SECTION("the switch is still present and reachable inside the taller row") {
        lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
        REQUIRE(toggle != nullptr);
        CHECK(coords_of(toggle).x2 <= coords_of(row).x2);
        CHECK(lv_obj_get_width(toggle) > 0);
    }
}

TEST_CASE_METHOD(SettingRowWrapFixture, "setting_toggle_row leaves a short description on one line",
                 "[xml][settings][layout]") {
    lv_obj_t* col = make_column();

    const char* attrs[] = {"name",     "row_short",          "label",       "Auto Lock",
                           "subject",  "test_row_wrap_val",  "disabled",    "test_row_wrap_dis",
                           "callback", "test_row_wrap_noop", "description", "Lock now",
                           nullptr};
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(col, "setting_toggle_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_update_layout(col);

    lv_obj_t* desc = lv_obj_find_by_name(row, "description");
    REQUIRE(desc != nullptr);

    // Guards the other direction: width="100%" must not pad a short string out
    // to extra lines, or every compact row grows for nothing.
    CHECK(lv_obj_get_height(desc) < 2 * line_height(desc));
}

TEST_CASE_METHOD(SettingRowWrapFixture, "setting_action_row wraps a long description",
                 "[xml][settings][layout][i18n]") {
    lv_obj_t* col = make_column();

    const char* attrs[] = {"name",        "row_action",         "label", "Automatic Heater Control",
                           "callback",    "test_row_wrap_noop", "icon",  "flash",
                           "description", LONG_TOGGLE_DESC,     nullptr};
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(col, "setting_action_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_update_layout(col);

    lv_obj_t* desc = lv_obj_find_by_name(row, "description");
    REQUIRE(desc != nullptr);
    check_wrapped_inside(desc, row, line_height(desc), "action row description");
    CHECK(coords_of(desc).y2 <= coords_of(row).y2);
}

TEST_CASE_METHOD(SettingRowWrapFixture, "setting_slider_row wraps its longest description",
                 "[xml][settings][layout][i18n]") {
    lv_obj_t* col = make_column();

    const char* attrs[] = {"name",     "row_slider",         "label",       "Long-Press Duration",
                           "callback", "test_row_wrap_noop", "description", LONG_SLIDER_DESC,
                           nullptr};
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(col, "setting_slider_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_update_layout(col);

    lv_obj_t* desc = lv_obj_find_by_name(row, "description");
    REQUIRE(desc != nullptr);
    // 130 ch in a 380px column is three lines or more.
    check_wrapped_inside(desc, row, line_height(desc), "slider row description");
    CHECK(lv_obj_get_height(desc) >= 3 * line_height(desc));

    SECTION("the slider still sits below the wrapped description, not behind it") {
        lv_obj_t* slider = lv_obj_find_by_name(row, "slider");
        REQUIRE(slider != nullptr);
        CHECK(coords_of(slider).y1 >= coords_of(desc).y2);
    }
}

TEST_CASE_METHOD(SettingRowWrapFixture, "setting_toggle_row wraps a long label",
                 "[xml][settings][layout][i18n]") {
    lv_obj_t* col = make_column();

    // "Keep Spool Info on Eject" is 52 ch once translated to French — past one
    // line of font_body in this column.
    const char* attrs[] = {"name",     "row_long_label",
                           "label",    "Conserver les informations de bobine a l'ejection",
                           "subject",  "test_row_wrap_val",
                           "disabled", "test_row_wrap_dis",
                           "callback", "test_row_wrap_noop",
                           nullptr};
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(col, "setting_toggle_row", attrs));
    REQUIRE(row != nullptr);
    lv_obj_update_layout(col);

    lv_obj_t* label = lv_obj_find_by_name(row, "label");
    REQUIRE(label != nullptr);
    check_wrapped_inside(label, row, line_height(label), "toggle row label");
}
