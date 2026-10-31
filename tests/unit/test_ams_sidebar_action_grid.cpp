// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_sidebar_action_grid.cpp
 * @brief The AMS sidebar's actions sit two to a row at every size, under the
 *        loaded strip and its status line.
 */

#include "ui_ams_sidebar.h"
#include "ui_button.h"

#include "../test_fixtures.h"
#include "../test_helpers/widget_in_tree.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "test_helpers/ams_sidebar_xml.h"

#include <cstdlib>
#include <lvgl.h>
#include <memory>

#include "../catch_amalgamated.hpp"

namespace {

struct SidebarGridFixture : public XMLTestFixture {
    SidebarGridFixture() {
        auto mock = std::make_unique<helix::AmsBackendMock>(4);
        REQUIRE(mock->start().success());
        helix::AmsState::instance().set_backend(std::move(mock));
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().sync_from_backend();

        helix::test::register_ams_sidebar_xml();
        column_ = lv_obj_create(test_screen());
        lv_obj_set_size(column_, 137, 264);
        lv_obj_set_style_pad_all(column_, 0, LV_PART_MAIN);
        root_ = static_cast<lv_obj_t*>(lv_xml_create(column_, "ams_sidebar", nullptr));
        REQUIRE(root_ != nullptr);
        // Check slots shows on backends that probe their slots; show it here so
        // all four actions are in play.
        lv_obj_remove_flag(named("btn_check_gates"), LV_OBJ_FLAG_HIDDEN);
        bp_ = lv_xml_get_subject(nullptr, "ui_breakpoint");
        REQUIRE(bp_ != nullptr);
        original_bp_ = lv_subject_get_int(bp_);
    }

    ~SidebarGridFixture() override {
        lv_subject_set_int(bp_, original_bp_);
        if (column_ && helix::test::widget_in_tree(column_))
            lv_obj_delete(column_);
        helix::AmsState::instance().set_backend(nullptr);
    }

    [[nodiscard]] lv_obj_t* named(const char* name) const {
        lv_obj_t* obj = lv_obj_find_by_name(root_, name);
        REQUIRE(obj != nullptr);
        return obj;
    }

    lv_area_t at(const char* name) {
        lv_obj_t* obj = named(name);
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        return a;
    }

    void use_breakpoint(int breakpoint) {
        lv_subject_set_int(bp_, breakpoint);
        process_lvgl(20);
        lv_obj_update_layout(test_screen());
    }

    lv_obj_t* column_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_subject_t* bp_ = nullptr;
    int original_bp_ = 0;
};

constexpr const char* kActions[] = {"btn_load", "btn_unload", "btn_reset", "btn_check_gates",
                                    "btn_settings"};

} // namespace

TEST_CASE_METHOD(SidebarGridFixture, "AMS sidebar: the actions sit two to a row at every size",
                 "[ams][sidebar][action_grid]") {
    for (int breakpoint = 0; breakpoint <= 4; ++breakpoint) {
        CAPTURE(breakpoint);
        use_breakpoint(breakpoint);
        const lv_area_t load = at("btn_load");
        const lv_area_t unload = at("btn_unload");
        const lv_area_t reset = at("btn_reset");
        const lv_area_t check = at("btn_check_gates");
        const lv_area_t settings = at("btn_settings");
        // Load | Unload over Home | Check slots, then Settings.
        CHECK(load.y1 == unload.y1);
        CHECK(reset.y1 == check.y1);
        CHECK(reset.y1 > unload.y2);
        CHECK(settings.y1 > check.y2);
        CHECK(unload.x1 > load.x2);
        CHECK(check.x1 > reset.x2);
        for (const char* name : kActions) {
            CAPTURE(name);
            const lv_area_t a = at(name);
            CHECK(lv_area_get_height(&a) >= 48);
            CHECK(lv_area_get_width(&a) * 2 <= lv_obj_get_width(root_));
        }
        // The status keeps its own line under the strip.
        CHECK_FALSE(lv_obj_has_flag(named("status_section"), LV_OBJ_FLAG_HIDDEN));
        CHECK(at("status_section").y1 > at("current_loaded_card").y2);
    }
}

TEST_CASE_METHOD(SidebarGridFixture, "AMS sidebar: a row the actions do not fill is centred",
                 "[ams][sidebar][action_grid]") {
    // Three actions: Unload | Home, then Settings alone on the second row.
    lv_obj_add_flag(named("btn_load"), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(named("btn_check_gates"), LV_OBJ_FLAG_HIDDEN);
    for (int breakpoint = 0; breakpoint <= 4; ++breakpoint) {
        CAPTURE(breakpoint);
        use_breakpoint(breakpoint);
        const lv_area_t grid = at("action_buttons_container");
        const lv_area_t unload = at("btn_unload");
        const lv_area_t reset = at("btn_reset");
        const lv_area_t settings = at("btn_settings");
        REQUIRE(settings.y1 > unload.y2);
        CAPTURE(grid.x1, grid.x2, unload.x1, reset.x2, settings.x1, settings.x2);
        // The lone button keeps its cell width and stands in the middle.
        CHECK(lv_area_get_width(&settings) == lv_area_get_width(&unload));
        CHECK(std::abs((settings.x1 - grid.x1) - (grid.x2 - settings.x2)) <= 1);
        // A full row leaves its slack evenly on both sides.
        CHECK(std::abs((unload.x1 - grid.x1) - (grid.x2 - reset.x2)) <= 1);
    }
}

// The breakpoint flips the cells between icon over label, icon beside it and
// label alone on a sidebar already built; every flip leaves each label one
// line of its font, within its button's width. The display stays 800x480, so
// tokens keep medium's sizes and a stacked cell's height is not judged here
// (test_ams_loaded_spool_sidebar.cpp measures that at 480x320).
TEST_CASE_METHOD(SidebarGridFixture, "AMS sidebar: labels fit across a live restack",
                 "[ams][sidebar][action_grid]") {
    for (const int breakpoint : {3, 1, 4, 0, 2, 3, 1}) {
        CAPTURE(breakpoint);
        use_breakpoint(breakpoint);
        for (const char* name : kActions) {
            CAPTURE(name);
            lv_obj_t* btn = named(name);
            lv_obj_t* label = nullptr;
            for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
                lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
                if (lv_obj_check_type(child, &lv_label_class) && child != ui_button_get_icon(btn)) {
                    label = child;
                }
            }
            REQUIRE(label != nullptr);
            const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
            CHECK(lv_obj_get_height(label) ==
                  lv_font_get_line_height(font) + lv_obj_get_style_space_top(label, LV_PART_MAIN) +
                      lv_obj_get_style_space_bottom(label, LV_PART_MAIN));
            lv_area_t inner;
            lv_obj_get_content_coords(btn, &inner);
            lv_area_t text;
            lv_obj_get_coords(label, &text);
            CHECK(text.x1 >= inner.x1);
            CHECK(text.x2 <= inner.x2);
        }
    }
}
