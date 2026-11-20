// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_backend_selector.cpp
 * @brief With two or more filament backends, the AMS panel shows one segment
 *        per backend; the active one is lit and tapping another switches to it.
 */

#include "ui_panel_ams.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "theme_manager.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string segment_text(lv_obj_t* segment) {
    lv_obj_t* label = lv_obj_get_child(segment, 0);
    return label ? lv_label_get_text(label) : "";
}

bool segment_lit(lv_obj_t* segment) {
    return lv_color_eq(lv_obj_get_style_bg_color(segment, LV_PART_MAIN),
                       theme_manager_get_color("primary"));
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "AmsPanel backend selector shows and switches backends",
                 "[ui_integration][ams][ams_backend_selector]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto primary = std::make_unique<AmsBackendMock>(4);
    REQUIRE(primary->start().success());
    ams.set_backend(std::move(primary));
    auto secondary = std::make_unique<AmsBackendMock>(2);
    REQUIRE(secondary->start().success());
    const int second = ams.add_backend(std::move(secondary));
    REQUIRE(second == 1);
    ams.sync_from_backend();

    ensure_ams_widgets_registered();
    AmsPanel panel(state(), &api());
    panel.init_subjects();
    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());
    panel.on_activate();
    process_lvgl(50);

    lv_obj_t* row = lv_obj_find_by_name(panel_obj, "backend_selector_row");
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(row));
    REQUIRE(lv_obj_get_child_count(row) == 2);

    lv_obj_t* first_seg = lv_obj_get_child(row, 0);
    lv_obj_t* second_seg = lv_obj_get_child(row, 1);
    const std::string expected = ams_type_to_string(ams.get_backend(0)->get_type());
    CHECK(segment_text(first_seg) == expected);
    CHECK(segment_text(second_seg) ==
          std::string(ams_type_to_string(ams.get_backend(1)->get_type())));
    CHECK(segment_lit(first_seg));
    CHECK_FALSE(segment_lit(second_seg));

    lv_obj_send_event(second_seg, LV_EVENT_CLICKED, nullptr);
    process_lvgl(50);

    CHECK(ams.active_backend_index() == 1);
    REQUIRE(lv_obj_get_child_count(row) == 2);
    CHECK_FALSE(segment_lit(lv_obj_get_child(row, 0)));
    CHECK(segment_lit(lv_obj_get_child(row, 1)));

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    ams.set_active_backend(0);
    ams.clear_backends();
}
