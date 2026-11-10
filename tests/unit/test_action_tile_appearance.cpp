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

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "core/update_queue.h"
#include "helix/ui/shared_font_style.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <array>
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

TEST_CASE("a tap on a home tile reaches the widget that owns it", "[action_tile][1142]") {
    // The tap is the XML event_cb on the tile's button; the callback finds its
    // widget through the tile root, not a C++-attached user_data. A callback that
    // cannot find its widget is a silent no-op, so each tap must open something.
    LVGLUITestFixture fixture;
    init_widget_registrations();
    helix::ui::destroy_static_panels();
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(fixture.test_screen());
    }
    NavigationManager::instance().set_panels(panels.data());
    REQUIRE(fixture.api() != nullptr); // the shutdown tile asks it what to offer
    helix_test::ScopedSharedResource<IMoonrakerAPI> api(
        std::shared_ptr<IMoonrakerAPI>(fixture.api(), [](IMoonrakerAPI*) {}));

    const std::vector<std::pair<std::string, std::string>> kButtons = {
        {"motion", "motion_button"},
        {"macros", "macros_button"},
        {"led_controls", "led_controls_button"},
        {"shutdown", "shutdown_button"},
        {"temperature", "temp_btn"},
        {"network", "network_btn"},
    };

    for (const auto& [id, button] : kButtons) {
        INFO("tile " << id);
        const auto* def = find_widget_def(id);
        REQUIRE(def != nullptr);
        RegistryWidgetHarness harness(fixture.test_screen(), *def);
        REQUIRE(harness.created());
        lv_obj_t* btn = harness.child(button.c_str());
        REQUIRE(btn != nullptr);
        REQUIRE_FALSE(NavigationManager::instance().has_open_overlays());
        REQUIRE(ModalStack::instance().empty());

        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(
            (NavigationManager::instance().has_open_overlays() || !ModalStack::instance().empty()));

        if (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        while (NavigationManager::instance().has_open_overlays()) {
            NavigationManager::instance().go_back();
            helix::ui::UpdateQueue::instance().drain_until_empty();
        }
    }
    NavigationManager::instance().shutdown();
    helix::ui::destroy_static_panels();
}

TEST_CASE("a pressed tile area dims and an idle one does not", "[action_tile][1142]") {
    // The dim is an XML style on the pressed state; nothing in C++ sets it.
    LVGLUITestFixture fixture;
    init_widget_registrations();
    const std::vector<std::pair<std::string, std::string>> kAreas = {
        {"printer_image", "printer_container"},
        {"temp_stack", "temp_stack_nozzle_row"},
        {"temp_stack", "temp_stack_bed_row"},
        {"temp_stack", "temp_stack_chamber_row"},
    };
    for (const auto& [id, area] : kAreas) {
        INFO("tile " << id << " area " << area);
        RegistryWidgetHarness harness(fixture.test_screen(), *find_widget_def(id));
        REQUIRE(harness.created());
        lv_obj_t* obj = harness.child(area.c_str());
        REQUIRE(obj != nullptr);
        CHECK(lv_obj_get_style_opa(obj, LV_PART_MAIN) == LV_OPA_COVER);
        lv_obj_add_state(obj, LV_STATE_PRESSED);
        CHECK(lv_obj_get_style_opa(obj, LV_PART_MAIN) == LV_OPA_70);
    }
}
