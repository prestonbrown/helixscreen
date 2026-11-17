// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_harness.cpp
 * @brief The harness itself: does it build the right component and attach?
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "panel_widget_manager.h"
#include "src/ui/panel_widgets/fan_stack_widget.h"
#include "src/ui/panel_widgets/tips_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture, "harness builds the widget's own component",
                 "[widget_size][harness]") {
    PanelWidgetManager::instance().init_widget_subjects();
    PanelWidgetHarness<TipsWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);
    REQUIRE(h.child("status_text_label") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "font tokens resolve distinctly under this fixture",
                 "[widget_size][harness]") {
    // If this fails, every font assertion in this plan is vacuous.
    require_font_tokens_distinct();
}

TEST_CASE_METHOD(LVGLUITestFixture, "harness applies config before component resolution",
                 "[widget_size][harness]") {
    // FanStackWidget::get_component_name() picks between panel_widget_fan_stack
    // and panel_widget_fan_carousel based on config_["display_mode"], which is
    // only populated by set_config(). PanelWidgetManager's real order is
    // construct -> set_config() -> get_component_name() -> attach(); this
    // proves the harness reproduces that order rather than always resolving
    // to the stack component.
    PanelWidgetHarness<FanStackWidget> h(
        test_screen(), HarnessConfig{{{"display_mode", "carousel"}}}, "fan_stack", state());
    REQUIRE(h.root() != nullptr);
    // "fan_carousel" is the ui_carousel child named in
    // ui_xml/components/panel_widget_fan_carousel.xml; it does not exist in
    // panel_widget_fan_stack.xml, so its presence proves the carousel
    // component was built, not the stack one.
    REQUIRE(h.child("fan_carousel") != nullptr);
}
