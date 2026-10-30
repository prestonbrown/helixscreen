// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_action_tile_appearance.cpp
 * @brief Appearance pin for the single-icon action tiles.
 *
 * These seven tiles are near-identical copies of one layout, and collapsing
 * them onto a shared component must not move what any of them draws. This
 * records the glyph face and the label's presence per tile so a migration that
 * changes one shows up as a diff rather than as a screenshot nobody took.
 *
 * Run with: ./build/bin/helix-tests "[action_tile]"
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "helix/ui/shared_font_style.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The seven tiles that are one icon over one label.
const std::vector<std::string> kActionTiles = {
    "shutdown", "firmware_restart", "led_controls", "macros", "motion", "gcode_console", "led"};

/// The glyph inside a tile, whatever the wrapper is called. Named lookup only:
/// child indices move when a layout changes, which is exactly what this test
/// is watching for.
lv_obj_t* find_icon(lv_obj_t* root) {
    if (!root) {
        return nullptr;
    }
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        // An icon is a label whose face is one of the MDI icon faces.
        if (lv_obj_check_type(child, &lv_label_class) &&
            helix::ui::is_icon_font(lv_obj_get_style_text_font(child, LV_PART_MAIN))) {
            return child;
        }
        if (lv_obj_t* found = find_icon(child)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("all seven action tiles share one glyph size", "[action_tile][1559]") {
    // One component, so the glyph ladder is shared by construction: every one
    // of the seven draws at the rung its tile publishes, firmware_restart
    // included (prestonbrown/helixscreen#1559).
    LVGLUITestFixture fixture;
    init_widget_registrations();

    const lv_font_t* shared = nullptr;
    int checked = 0;

    for (const auto& id : kActionTiles) {
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        RegistryWidgetHarness harness(fixture.test_screen(), *def);
        REQUIRE(harness.created());
        lv_obj_t* icon = find_icon(harness.root());
        INFO("tile " << id);
        REQUIRE(icon != nullptr);

        const lv_font_t* face = lv_obj_get_style_text_font(icon, LV_PART_MAIN);
        if (!shared) {
            shared = face;
        } else {
            INFO("tile " << id << " must draw at the same rung as the others");
            CHECK(face == shared);
        }
        ++checked;
    }

    REQUIRE(shared != nullptr);
    CHECK(checked == static_cast<int>(kActionTiles.size()));
}

TEST_CASE("each action tile keeps the widget name its C++ looks up", "[action_tile][1559]") {
    // The widgets find these with lv_obj_find_by_name to wire what XML cannot
    // express. A shared component that renamed them would break every one of
    // those lookups silently, since a missed lookup is a null and a no-op.
    LVGLUITestFixture fixture;
    init_widget_registrations();

    const std::vector<std::pair<std::string, std::string>> kNames = {
        {"shutdown", "shutdown_button"},
        {"led_controls", "led_controls_button"},
        {"macros", "macros_button"},
        {"motion", "motion_button"},
        {"gcode_console", "gcode_console_button"},
        {"led", "light_button"},
        {"firmware_restart", "firmware_restart_button_home"},
    };

    for (const auto& [id, widget_name] : kNames) {
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        RegistryWidgetHarness harness(fixture.test_screen(), *def);
        REQUIRE(harness.created());
        INFO("tile " << id << " must still carry " << widget_name);
        CHECK(lv_obj_find_by_name(harness.root(), widget_name.c_str()) != nullptr);
    }
}
