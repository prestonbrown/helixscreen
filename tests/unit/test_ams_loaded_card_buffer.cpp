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

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
bool hidden(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
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
