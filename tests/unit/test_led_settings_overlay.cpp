// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_settings_overlay.cpp
 * @brief LED Settings overlay: the light-button chip row is gone, and
 *        Automatic LED Control's "Applies to" row drives its own targets.
 */

#include "ui_nav_manager.h"
#include "ui_settings_led.h"
#include "ui_swatch.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/static_panel_registry.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"

#include <array>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "LED settings: no selection row, an Applies to row",
                 "[led][settings]") {
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "led_settings_overlay", nullptr));
    REQUIRE(root != nullptr);
    CHECK(lv_obj_find_by_name(root, "row_led_select") == nullptr);
    CHECK(lv_obj_find_by_name(root, "row_auto_state_strips") != nullptr);
}

namespace helix::settings {
class LedSettingsOverlayTestAccess {
  public:
    static void delete_macro(int index) {
        get_led_settings_overlay().handle_delete_macro_device(index);
    }
    static void tap_applies_to(const std::string& id) {
        get_led_settings_overlay().handle_led_chip_clicked(id);
    }
};
} // namespace helix::settings

TEST_CASE_METHOD(LVGLTestFixture, "deleting a macro device removes it from Applies to",
                 "[led][settings]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    helix::led::LedMacroInfo m;
    m.display_name = "Lamp";
    m.type = helix::led::MacroLedType::TOGGLE;
    m.toggle_macro = "LIGHT_TOGGLE";
    ctrl.set_configured_macros({m});
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({"macro:Lamp", "neopixel chamber_light"});

    helix::settings::LedSettingsOverlayTestAccess::delete_macro(0);

    CHECK(as.strips() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({});
    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLTestFixture, "Applies to keeps a target that is not discovered yet",
                 "[led][settings]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
        helix::led::LedStripInfo s;
        s.id = id;
        s.name = id;
        s.backend = helix::led::LedBackendType::NATIVE;
        ctrl.native().add_strip(s);
    }
    auto& as = helix::led::LedAutoState::instance();
    // A migrated WLED strip whose discovery has not answered yet.
    as.set_strips({"neopixel chamber_light", "printer_led"});

    helix::settings::LedSettingsOverlayTestAccess::tap_applies_to("neopixel sb_leds");

    CHECK(as.strips() ==
          std::vector<std::string>{"neopixel chamber_light", "printer_led", "neopixel sb_leds"});
    as.set_strips({});
    as.save_config();
    ctrl.deinit();
}

namespace {

class LedSettingsSwatchFixture : public LVGLUITestFixture {
  protected:
    LedSettingsSwatchFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(nullptr, nullptr);
    }

    ~LedSettingsSwatchFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
        auto& as = helix::led::LedAutoState::instance();
        as.set_mapping("idle", {});
        as.save_config();
        helix::led::LedController::instance().deinit();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    static int selected_for_idle() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "led_auto_color_sel_0");
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }

    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: an auto-state colour row is painter swatches with a ring",
                 "[led][settings][swatch]") {
    const auto& presets = helix::led::LedController::instance().color_presets();
    REQUIRE(presets.size() >= 3);
    helix::led::LedStateAction action;
    action.action_type = "color";
    action.color = presets[1];
    helix::led::LedAutoState::instance().set_mapping("idle", action);

    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    lv_obj_t* ctx = lv_obj_find_by_name(lv_screen_active(), "ctx_idle");
    REQUIRE(ctx != nullptr);

    for (size_t i = 0; i < presets.size(); ++i) {
        INFO("preset " << i);
        const std::string name = "led_auto_swatch_" + std::to_string(i);
        lv_obj_t* swatch = lv_obj_find_by_name(ctx, name.c_str());
        REQUIRE(swatch != nullptr);
        lv_obj_t* disc = lv_obj_find_by_name(swatch, "color_swatch_disc");
        REQUIRE(disc != nullptr);
        CHECK(helix::ui::has_swatch_paint(disc));
        CHECK(lv_obj_get_style_radius(disc, LV_PART_MAIN) == LV_RADIUS_CIRCLE);
        CHECK((lv_color_to_u32(helix::ui::swatch_color_of(swatch)) & 0xFFFFFF) == presets[i]);
    }
    CHECK(selected_for_idle() == 1);

    // A tap picks that preset for the row and moves the ring, with no rebuild.
    lv_obj_t* third = lv_obj_find_by_name(ctx, "led_auto_swatch_2");
    REQUIRE(third != nullptr);
    lv_obj_send_event(third, LV_EVENT_CLICKED, nullptr);
    settle();
    const auto* mapping = helix::led::LedAutoState::instance().get_mapping("idle");
    REQUIRE(mapping != nullptr);
    CHECK(mapping->color == presets[2]);
    CHECK(selected_for_idle() == 2);
    CHECK(lv_obj_find_by_name(ctx, "led_auto_swatch_2") == third);
}
