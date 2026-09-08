// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_progress_arc_axis.cpp
 * @brief The progress arc and the linear bar are two views of one number.
 *
 * PrinterPrintState publishes progress twice: print_progress is the raw sample
 * logic reasons about, print_progress_display is what gets drawn, and the two
 * deliberately part company while freeze_progress_display() holds — a finished
 * print pins 100 while the raw sample keeps moving and Moonraker eventually
 * zeroes it. Every render surface has to fill from the display subject, or the
 * two progress widgets on the same card show different numbers
 * (prestonbrown/helixscreen#1510).
 */

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

// Feed a Moonraker status payload through the real parser.
void push_status(PrinterState& ps, const char* state, double progress) {
    nlohmann::json status;
    status["print_stats"]["state"] = state;
    status["virtual_sdcard"]["progress"] = progress;
    ps.update_from_status(status);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "progress arc and bar agree while the display is frozen",
                 "[print_status][progress][1510]") {
    int arc_pct = -1;
    int bar_pct = -1;
    int mid_arc = -1;
    int mid_bar = -1;
    {
        // Construct the widget before parsing the component: helix-xml skips
        // bindings whose subject is absent at parse time.
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "detailed"}});

        lv_obj_t* comp = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "panel_widget_print_status", nullptr));
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        process_lvgl(10);

        lv_obj_t* arc = lv_obj_find_by_name(comp, "detailed_progress_arc");
        lv_obj_t* bar = lv_obj_find_by_name(comp, "print_progress_bar");
        REQUIRE(arc != nullptr);
        REQUIRE(bar != nullptr);

        push_status(state(), "printing", 0.47);
        process_lvgl(10);
        mid_arc = static_cast<int>(lv_arc_get_value(arc));
        mid_bar = static_cast<int>(lv_bar_get_value(bar));

        // Completion freezes the display pair at 100, then Moonraker zeroes
        // virtual_sdcard.progress in the same batch as STANDBY. The raw subject
        // follows it down; neither widget may.
        push_status(state(), "complete", 0.47);
        push_status(state(), "standby", 0.0);
        process_lvgl(10);

        REQUIRE(lv_subject_get_int(state().get_print_progress_subject()) == 0);
        arc_pct = static_cast<int>(lv_arc_get_value(arc));
        bar_pct = static_cast<int>(lv_bar_get_value(bar));
    }
    // Destroy the shared formatter while this fixture's subjects are still
    // alive (see test_print_status_widget_recycle.cpp), then assert — a
    // regression fails cleanly instead of taking teardown down with it.
    PrintStatusWidget::destroy_formatter_for_test();

    REQUIRE(mid_arc == 47);
    REQUIRE(mid_bar == 47);
    REQUIRE(arc_pct == 100);
    REQUIRE(bar_pct == 100);
}
