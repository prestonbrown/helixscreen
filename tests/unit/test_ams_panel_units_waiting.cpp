// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_panel_units_waiting.cpp
 * @brief Until the backend reports its slots, the AMS panel shows a waiting
 *        state in place of the unit and path diagram.
 */

#include "ui_panel_ams.h"

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(XMLTestFixture, "AmsPanel waits for the units before drawing the diagram",
                 "[ui_integration][ams]") {
    auto owned = std::make_unique<AmsBackendMock>(4);
    REQUIRE(owned->start().success());
    AmsState::instance().set_backend(std::move(owned));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();

    ensure_ams_widgets_registered();
    AmsPanel panel(state(), &api());
    panel.init_subjects();
    lv_obj_t* panel_obj =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_panel", nullptr));
    REQUIRE(panel_obj != nullptr);
    panel.setup(panel_obj, test_screen());
    panel.on_activate();
    process_lvgl(50);

    lv_obj_t* diagram = lv_obj_find_by_name(panel_obj, "left_column");
    lv_obj_t* waiting = lv_obj_find_by_name(panel_obj, "units_waiting");
    REQUIRE(diagram != nullptr);
    REQUIRE(waiting != nullptr);
    lv_subject_t* slots = AmsState::instance().get_slot_count_subject();

    CHECK_FALSE(lv_obj_is_hidden(diagram));
    CHECK(lv_obj_is_hidden(waiting));

    lv_subject_set_int(slots, 0);
    process_lvgl(50);
    CHECK(lv_obj_is_hidden(diagram));
    CHECK_FALSE(lv_obj_is_hidden(waiting));

    lv_subject_set_int(slots, 4);
    process_lvgl(50);
    CHECK_FALSE(lv_obj_is_hidden(diagram));
    CHECK(lv_obj_is_hidden(waiting));

    panel.on_deactivate(DeactivateReason::NavigateAway);
    panel.clear_panel_reference();
    lv_obj_delete(panel_obj);
    process_lvgl(10);
    AmsState::instance().set_backend(nullptr);
}
