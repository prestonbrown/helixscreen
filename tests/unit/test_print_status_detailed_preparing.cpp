// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_detailed_preparing.cpp
 * @brief The detailed active layout shows the pre-print phase block while a pre-print
 *        routine runs, in place of the arc and data columns.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

bool hidden(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return lv_obj_is_hidden(obj);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "detailed active layout swaps the arc for the pre-print block while preparing",
                 "[print_status][panel_widget][preparing]") {
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "detailed"}});
        auto* comp = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "panel_widget_print_status", nullptr));
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        widget.on_size_changed(3, 2, 600, 400);
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        process_lvgl(30);

        auto& ps = get_printer_state().print_state();
        REQUIRE(lv_obj_find_by_name(comp, "print_card_printing_detailed") != nullptr);

        ps.set_print_start_state(PrintStartPhase::IDLE, "", 0);
        process_lvgl(30);
        CHECK(hidden(comp, "detailed_preparing_info"));
        CHECK_FALSE(hidden(comp, "detailed_center_col"));
        CHECK_FALSE(hidden(comp, "detailed_data_col"));

        ps.set_print_start_state(PrintStartPhase::HEATING_BED, "Heating bed", 40);
        process_lvgl(30);
        CHECK_FALSE(hidden(comp, "detailed_preparing_info"));
        CHECK(hidden(comp, "detailed_center_col"));
        CHECK(hidden(comp, "detailed_data_col"));
    }
    PrintStatusWidget::destroy_formatter_for_test();
}
