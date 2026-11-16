// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_clock.cpp
 * @brief clock picks its 4-way mode from physical pixels on two independent
 * axes (width, height), not colspan/rowspan.
 *
 * `on_size_changed` (clock_widget.cpp) writes/restyles four things, all
 * asserted here:
 *   - `clock_size_mode` subject (0-3) — written before the `!widget_obj_`
 *     guard, so it is readable even if the widget lookups below it fail.
 *   - `clock_time`: font_body (mode 0), font_heading (modes 1-2), font_xl
 *     (mode 3)
 *   - `clock_date`: font_body (modes 0-2), font_heading (mode 3)
 *   - `clock_uptime`: hidden unless mode >= 2
 *
 * Four cases isolate the 4-way, 2-axis predicate. Each pairs the pixels for
 * one mode with a span that would produce a *different* mode under the old
 * colspan/rowspan predicate, so an implementation that still reads spans
 * fails here instead of passing by coincidence. The width>=w_wide(),
 * height<h_tall() case is the one that actually separates mode 1 from mode 3:
 * a predicate that checks width before height would wrongly land "large" on
 * a wide-but-short cell.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/clock_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {
/// Reads clock's size-mode subject directly — it is written before the
/// widget's `!widget_obj_` guard, so it is observable independent of
/// whether the object lookups below it succeed.
int clock_size_mode() {
    auto* subject = lv_xml_get_subject(nullptr, "clock_size_mode");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "clock mode follows pixels on both axes, not spans",
                 "[widget_size][clock]") {
    // Widget-owned subjects (clock_size_mode, clock_time_text, ...) are
    // registered lazily; the harness alone does not trigger it.
    PanelWidgetManager::instance().init_widget_subjects();
    require_font_tokens_distinct();

    PanelWidgetHarness<ClockWidget> h(test_screen());

    lv_obj_t* time_label = h.child("clock_time");
    REQUIRE(time_label != nullptr);
    lv_obj_t* date_label = h.child("clock_date");
    REQUIRE(date_label != nullptr);
    lv_obj_t* uptime_label = h.child("clock_uptime");
    REQUIRE(uptime_label != nullptr);

    // Mode 0 (compact, time only): a short box too narrow for the date.
    // Contradicting span: 4x4 (old predicate: colspan>=3 && rowspan>=2 -> mode 3).
    h.resize(4, 4, 40, h_tall() - 1);
    CHECK(clock_size_mode() == 0);
    CHECK(lv_obj_get_style_text_font(time_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_get_style_text_font(date_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_is_hidden(uptime_label));

    // Mode 1 below the normal width band: the date shows wherever it fits,
    // not only from w_normal() up.
    h.resize(3, 2, w_normal() - 1, h_tall() - 1);
    CHECK(clock_size_mode() == 1);
    CHECK_FALSE(lv_obj_is_hidden(date_label));

    // Mode 1 (normal, time+date): height below threshold, width AT/OVER
    // w_wide(). Isolates "height below threshold" from the width axis --
    // proves width alone cannot promote a short cell to mode 3.
    // Contradicting span: 1x1 (old predicate: colspan<=1 && rowspan<=1 ->
    // mode 0).
    h.resize(1, 1, w_wide(), h_tall() - 1);
    CHECK(clock_size_mode() == 1);
    CHECK(lv_obj_get_style_text_font(time_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_heading"));
    CHECK(lv_obj_get_style_text_font(date_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK(lv_obj_is_hidden(uptime_label));

    // Mode 2 (expanded): height at/over threshold, width below w_wide().
    // Contradicting span: 1x1 (old predicate -> mode 0).
    h.resize(1, 1, w_normal(), h_tall());
    CHECK(clock_size_mode() == 2);
    CHECK(lv_obj_get_style_text_font(time_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_heading"));
    CHECK(lv_obj_get_style_text_font(date_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_body"));
    CHECK_FALSE(lv_obj_is_hidden(uptime_label));

    // Mode 3 (large): both axes at/over threshold. Contradicting span: 1x1
    // (old predicate -> mode 0).
    h.resize(1, 1, w_wide(), h_tall());
    CHECK(clock_size_mode() == 3);
    CHECK(lv_obj_get_style_text_font(time_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_xl"));
    CHECK(lv_obj_get_style_text_font(date_label, LV_PART_MAIN) ==
          theme_manager_get_font("font_heading"));
    CHECK_FALSE(lv_obj_is_hidden(uptime_label));
}
