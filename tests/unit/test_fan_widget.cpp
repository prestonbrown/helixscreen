// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A fan tile is configured from the saved layout before the printer has
// reported its fans, so the name it shows has to follow the fan list.

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/fan_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string name_shown(PanelWidgetHarness<FanWidget>& tile) {
    lv_obj_t* label = tile.child("fan_name");
    REQUIRE(label != nullptr);
    const char* text = lv_label_get_text(label);
    return text ? text : "";
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A fan tile takes its name from the fan list once the printer reports its fans",
                 "[panel_widget][fan_widget][2204]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;
    PanelWidgetManager::instance().init_widget_subjects();
    auto& fans = get_printer_state().fan_state();
    REQUIRE(fans.get_fans().empty());

    // The saved layout names the Klipper object, and no fan is known yet.
    PanelWidgetHarness<FanWidget> tile(
        test_screen(), HarnessConfig{nlohmann::json{{"fan", "fan_generic exhaust_fan"}}}, "fan");
    CHECK(name_shown(tile) == "fan_generic exhaust_fan");

    fans.init_fans({"fan", "fan_generic exhaust_fan"});
    helix::ui::UpdateQueue::instance().drain();

    // The setup reached the list: the fan is there under its own name.
    REQUIRE(fans.get_fans().size() == 2);
    CHECK(name_shown(tile) == "Exhaust Fan");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A fan tile attached after the fans are known shows the fan's name at once",
                 "[panel_widget][fan_widget][2204]") {
    ScopedRuntimeConfig runtime_config;
    get_runtime_config()->test_mode = true;
    PanelWidgetManager::instance().init_widget_subjects();
    auto& fans = get_printer_state().fan_state();
    fans.init_fans({"fan", "fan_generic exhaust_fan"});

    PanelWidgetHarness<FanWidget> tile(
        test_screen(), HarnessConfig{nlohmann::json{{"fan", "fan_generic exhaust_fan"}}}, "fan");

    CHECK(name_shown(tile) == "Exhaust Fan");
}
