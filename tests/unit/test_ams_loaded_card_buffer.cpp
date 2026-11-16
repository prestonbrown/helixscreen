// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_loaded_card_buffer.cpp
 * @brief The loaded-spool card's buffer slider: shown with a reading, the
 *        slider only with a set point, the number always.
 */

#include "ui_panel_ams.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "ams_state.h"

#include <cstdlib>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
bool hidden(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    return lv_obj_is_hidden(obj);
}
std::string text(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    return lv_label_get_text(obj);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "loaded card draws the buffer beside the material",
                 "[ams][buffer][loaded_card]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ensure_ams_widgets_registered();
    auto* card = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_loaded_card", nullptr));
    REQUIRE(card != nullptr);

    SECTION("with a set point: slider and number") {
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.32f}), 0);
        CHECK_FALSE(hidden(card, "buffer_mini"));
        CHECK_FALSE(hidden(card, "buffer_mini_slider"));
        CHECK(text(card, "buffer_mini_value") == "32%");
        CHECK(text(card, "buffer_mini_label") == "FPS");
    }

    SECTION("without one: the number alone") {
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.32f}, -1.0f), 0);
        CHECK_FALSE(hidden(card, "buffer_mini"));
        CHECK(hidden(card, "buffer_mini_slider"));
        CHECK(text(card, "buffer_mini_value") == "32%");
    }

    SECTION("no reading: nothing") {
        AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
        CHECK(hidden(card, "buffer_mini"));
    }

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}

namespace {
lv_area_t coords_of(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return a;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "loaded strip: one line each for position and name, meters below while idle",
                 "[ams][buffer][loaded_card]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    // A repeat init publishes every name again: drop one the card binds and
    // let the second init put it back.
    lv_xml_unregister_subject(nullptr, "clog_meter_safe");
    ams.init_subjects(true);
    ensure_ams_widgets_registered();
    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(bp != nullptr);
    const int original = lv_subject_get_int(bp);

    auto* card = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_loaded_card", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_set_width(card, 137);
    AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.32f}), 0);
    lv_subject_set_string(ams.get_current_material_text_subject(),
                          "Polymaker Jet Black PLA Galaxy Sparkle Edition");

    for (int breakpoint = 0; breakpoint <= 4; ++breakpoint) {
        CAPTURE(breakpoint);
        lv_subject_set_int(bp, breakpoint);
        process_lvgl(20);
        lv_obj_update_layout(card);
        REQUIRE_FALSE(hidden(card, "buffer_mini"));
        const lv_area_t position = coords_of(card, "loaded_position");
        const lv_area_t name = coords_of(card, "loaded_material");
        const lv_area_t details = coords_of(card, "loaded_details");
        const lv_area_t body = coords_of(card, "loaded_body");
        const lv_area_t buffer = coords_of(card, "buffer_mini");
        if (breakpoint < 3) {
            // A colour bar leads; both lines take the body's width.
            CHECK_FALSE(hidden(card, "loaded_swatch"));
            CHECK(hidden(card, "loaded_spool"));
            CHECK(lv_area_get_width(&details) == lv_area_get_width(&body));
        } else {
            // The spool leads, and the lines take the rest of its track.
            CHECK(hidden(card, "loaded_swatch"));
            REQUIRE_FALSE(hidden(card, "loaded_spool"));
            const lv_area_t spool = coords_of(card, "loaded_spool");
            CHECK(details.x1 > spool.x2);
            CHECK(details.x2 == body.x2);
        }
        // One line each, ellipsized beyond.
        CHECK(name.y1 > position.y1);
        const lv_font_t* font =
            lv_obj_get_style_text_font(lv_obj_find_by_name(card, "loaded_material"), LV_PART_MAIN);
        CHECK(lv_area_get_height(&name) <= lv_font_get_line_height(font));
        // The meters sit below the lines.
        CHECK(buffer.y1 > details.y2);
    }

    // From medium up in landscape the meters are two equal tiles on one row,
    // under a divider that goes with them.
    lv_subject_t* clog_mode = lv_xml_get_subject(nullptr, "clog_meter_mode");
    REQUIRE(clog_mode != nullptr);
    const int clog_mode_before = lv_subject_get_int(clog_mode);
    lv_subject_set_int(clog_mode, 1);
    lv_subject_set_int(bp, 3);
    process_lvgl(20);
    lv_obj_update_layout(card);
    {
        REQUIRE_FALSE(hidden(card, "loaded_meter_divider"));
        const lv_area_t divider = coords_of(card, "loaded_meter_divider");
        const lv_area_t details = coords_of(card, "loaded_details");
        const lv_area_t clog = coords_of(card, "clog_meter");
        const lv_area_t buffer = coords_of(card, "buffer_mini");
        CHECK(divider.y1 > details.y2);
        CHECK(clog.y1 > divider.y2);
        CHECK(clog.y1 == buffer.y1);
        CHECK(lv_area_get_height(&clog) == lv_area_get_height(&buffer));
        CHECK(std::abs(lv_area_get_width(&clog) - lv_area_get_width(&buffer)) <= 1);
        CHECK(text(card, "clog_caption") == "Clog");
    }
    // No meter to show: no divider either.
    lv_subject_set_int(clog_mode, 0);
    lv_subject_set_int(ams.get_buffer_present_subject(), 0);
    process_lvgl(20);
    CHECK(hidden(card, "loaded_meter_divider"));
    lv_subject_set_int(clog_mode, 1);
    lv_subject_set_int(ams.get_buffer_present_subject(), 1);

    // During any operation the meters give their room to the step progress.
    process_lvgl(20);
    REQUIRE_FALSE(hidden(card, "clog_meter"));
    lv_subject_set_int(ams.get_ams_action_subject(), static_cast<int>(AmsAction::LOADING));
    process_lvgl(20);
    CHECK(hidden(card, "buffer_mini"));
    CHECK(hidden(card, "clog_meter"));
    lv_subject_set_int(ams.get_ams_action_subject(), static_cast<int>(AmsAction::IDLE));
    process_lvgl(20);
    CHECK_FALSE(hidden(card, "buffer_mini"));
    CHECK_FALSE(hidden(card, "clog_meter"));

    // Nothing to report: the check stands in for the arc and the reading.
    lv_subject_t* safe = ams.clog_meter_subjects().safe;
    for (int is_safe : {1, 0}) {
        lv_subject_set_int(safe, is_safe);
        CHECK(hidden(card, "clog_arc") == (is_safe == 1));
        CHECK(hidden(card, "clog_value_text") == (is_safe == 1));
        CHECK(hidden(card, "clog_safe_icon") == (is_safe == 0));
    }
    lv_subject_set_int(clog_mode, clog_mode_before);

    lv_subject_set_int(bp, original);
    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}
