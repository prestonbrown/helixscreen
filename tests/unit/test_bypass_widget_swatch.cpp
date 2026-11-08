// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bypass_widget_swatch.cpp
 * @brief The bypass tile's external-spool dot paints through the swatch painter,
 *        with a multi-colour spool's split and its finish.
 */

#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "ams_state.h"
#include "ams_types.h"
#include "src/ui/panel_widgets/bypass_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture,
                 "bypass: the external spool dot paints the spool's colours and finish",
                 "[bypass][swatch][panel_widget]") {
    AmsState::instance().init_subjects(true);
    SlotInfo ext;
    ext.color_rgb = 0xFF0000;
    ext.material = "PLA Silk";
    helix::AmsStateTestAccess::seed_external_spool(ext);

    PanelWidgetHarness<BypassWidget> h(test_screen());
    process_lvgl(30);

    lv_obj_t* dot = h.child("bypass_color_dot");
    REQUIRE(dot != nullptr);
    lv_obj_t* disc = lv_obj_find_by_name(dot, "color_swatch_disc");
    REQUIRE(disc != nullptr);
    CHECK(ui::has_swatch_paint(disc));
    CHECK(ui::swatch_paint_finish(disc) == ui::SwatchFinish::Silk);

    // A single-colour spool replaces it: one colour, plain.
    ext.color_rgb = 0x2266AA;
    ext.material = "PETG";
    helix::AmsStateTestAccess::seed_external_spool(ext);
    process_lvgl(30);
    const auto colors = ui::swatch_paint_colors(disc);
    REQUIRE(colors.size() == 1);
    CHECK((lv_color_to_u32(colors[0]) & 0xFFFFFF) == 0x2266AAu);
    CHECK(ui::swatch_paint_finish(disc) == ui::SwatchFinish::Plain);

    AmsState::instance().clear_external_spool_info();
}
