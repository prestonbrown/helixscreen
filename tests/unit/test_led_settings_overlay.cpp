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
    static void add_macro() {
        get_led_settings_overlay().handle_add_macro_device();
    }
    static void edit_macro(int index) {
        get_led_settings_overlay().handle_edit_macro_device(index);
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

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: the startup readout and detail panels follow their subjects",
                 "[led][settings]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.set_startup_brightness(65);
    helix::led::LedStateAction brightness;
    brightness.action_type = "brightness";
    helix::led::LedAutoState::instance().set_mapping("idle", brightness);

    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();

    lv_obj_t* row = lv_obj_find_by_name(lv_screen_active(), "row_startup_brightness");
    REQUIRE(row != nullptr);
    lv_obj_t* readout = lv_obj_find_by_name(row, "value_label");
    REQUIRE(readout != nullptr);
    CHECK(std::string(lv_label_get_text(readout)) == "65%");
    overlay.handle_startup_brightness_changed(42);
    CHECK(std::string(lv_label_get_text(readout)) == "42%");

    // A row with an action shows its detail panel; an "off" row hides it.
    lv_obj_t* idle = lv_obj_find_by_name(lv_screen_active(), "detail_idle");
    lv_obj_t* heating = lv_obj_find_by_name(lv_screen_active(), "detail_heating");
    REQUIRE(idle != nullptr);
    REQUIRE(heating != nullptr);
    CHECK_FALSE(lv_obj_is_hidden(idle));
    CHECK(lv_obj_is_hidden(heating));

    // Picking "off" (the first option) in the idle row's dropdown hides its panel.
    lv_obj_t* idle_row = lv_obj_get_sibling(idle, -1);
    REQUIRE(idle_row != nullptr);
    lv_obj_t* dropdown = lv_obj_find_by_name(idle_row, "dropdown");
    REQUIRE(dropdown != nullptr);
    lv_dropdown_set_selected(dropdown, 0);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK(lv_obj_is_hidden(idle));

    ctrl.set_startup_brightness(80);
}

// ---------------------------------------------------------------------------
// Macro devices: the card list and its edit form, driven through widget names
// ---------------------------------------------------------------------------

namespace {

lv_obj_t* named(const char* name) {
    return lv_obj_find_by_name(lv_screen_active(), name);
}

void type_into(const char* name, const char* text) {
    lv_obj_t* ta = named(name);
    REQUIRE(ta != nullptr);
    lv_textarea_set_text(ta, text);
}

void pick(const char* name, uint32_t index) {
    lv_obj_t* dd = named(name);
    REQUIRE(dd != nullptr);
    lv_dropdown_set_selected(dd, index);
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
}

void tap(const char* name) {
    lv_obj_t* btn = named(name);
    REQUIRE(btn != nullptr);
    lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
}

/// Shown on screen: the widget and every ancestor are unhidden.
bool shown(const char* name) {
    lv_obj_t* obj = named(name);
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o))
            return false;
    }
    return obj != nullptr;
}

bool editing_card(int index) {
    lv_obj_t* edit = named(("macro_edit_" + std::to_string(index)).c_str());
    return edit != nullptr && !lv_obj_is_hidden(edit) && named("macro_name_input") != nullptr;
}

} // namespace

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a new on/off macro device is named, saved and closes its form",
                 "[led][settings][macro_devices]") {
    auto& ctrl = helix::led::LedController::instance();
    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();

    helix::settings::LedSettingsOverlayTestAccess::add_macro();
    settle();
    REQUIRE(editing_card(0));

    type_into("macro_name_input", "  Lamp ");
    type_into("macro_on_text", "LAMP_ON");
    type_into("macro_off_text", "LAMP_OFF");
    tap("macro_save_0");
    settle();

    REQUIRE(ctrl.configured_macros().size() == 1);
    const auto& m = ctrl.configured_macros()[0];
    CHECK(m.display_name == "Lamp");
    CHECK(m.type == helix::led::MacroLedType::ON_OFF);
    CHECK(m.on_macro == "LAMP_ON");
    CHECK(m.off_macro == "LAMP_OFF");
    CHECK_FALSE(editing_card(0));
    CHECK(named("macro_edit_btn_0") != nullptr);
    CHECK(named("macro_delete_btn_0") != nullptr);
}

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a macro device's save refuses a blank name and matching on/off",
                 "[led][settings][macro_devices]") {
    auto& ctrl = helix::led::LedController::instance();
    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    helix::settings::LedSettingsOverlayTestAccess::add_macro();
    settle();

    tap("macro_save_0");
    settle();
    CHECK(editing_card(0));
    CHECK(ctrl.configured_macros()[0].display_name.empty());

    type_into("macro_name_input", "Lamp");
    type_into("macro_on_text", "SAME");
    type_into("macro_off_text", "SAME");
    tap("macro_save_0");
    settle();
    CHECK(editing_card(0));
    CHECK(ctrl.configured_macros()[0].display_name.empty());
}

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a preset macro device adds, keeps and removes preset rows",
                 "[led][settings][macro_devices]") {
    auto& ctrl = helix::led::LedController::instance();
    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    helix::settings::LedSettingsOverlayTestAccess::add_macro();
    settle();

    pick("macro_type_dropdown", 2);
    settle();
    REQUIRE(editing_card(0));
    CHECK_FALSE(shown("macro_on_text"));
    CHECK(named("preset_text_0_0") == nullptr);

    tap("macro_add_preset_0");
    settle();
    tap("macro_add_preset_0");
    settle();
    REQUIRE(named("preset_text_0_0") != nullptr);
    REQUIRE(named("preset_text_0_1") != nullptr);

    tap("preset_remove_0_0");
    settle();
    CHECK(named("preset_text_0_0") != nullptr);
    CHECK(named("preset_text_0_1") == nullptr);

    type_into("macro_name_input", "Scenes");
    type_into("preset_text_0_0", "SCENE_A");
    tap("macro_save_0");
    settle();
    const auto& m = ctrl.configured_macros()[0];
    CHECK(m.display_name == "Scenes");
    CHECK(m.type == helix::led::MacroLedType::PRESET);
    CHECK(m.presets == std::vector<std::string>{"SCENE_A"});
    CHECK(m.on_macro.empty());
}

TEST_CASE_METHOD(
    LedSettingsSwatchFixture,
    "LED settings: a saved macro device's edit button toggles its form and delete removes it",
    "[led][settings][macro_devices]") {
    auto& ctrl = helix::led::LedController::instance();
    helix::led::LedMacroInfo m;
    m.display_name = "Lamp";
    m.type = helix::led::MacroLedType::TOGGLE;
    m.toggle_macro = "LAMP_TOGGLE";
    ctrl.set_configured_macros({m});

    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    CHECK_FALSE(editing_card(0));

    tap("macro_edit_btn_0");
    settle();
    REQUIRE(editing_card(0));
    CHECK(std::string(lv_textarea_get_text(named("macro_name_input"))) == "Lamp");
    CHECK(std::string(lv_textarea_get_text(named("macro_toggle_text"))) == "LAMP_TOGGLE");

    tap("macro_edit_btn_0");
    settle();
    CHECK_FALSE(editing_card(0));

    tap("macro_delete_btn_0");
    settle();
    CHECK(ctrl.configured_macros().empty());
    CHECK(named("macro_edit_btn_0") == nullptr);
}

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: changing a macro device's type keeps what was typed",
                 "[led][settings][macro_devices]") {
    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    helix::settings::LedSettingsOverlayTestAccess::add_macro();
    settle();

    type_into("macro_name_input", "Lamp");
    pick("macro_type_dropdown", 1);
    settle();

    REQUIRE(editing_card(0));
    CHECK(std::string(lv_textarea_get_text(named("macro_name_input"))) == "Lamp");
    CHECK(shown("macro_toggle_text"));
    CHECK_FALSE(shown("macro_on_text"));
}

// ---------------------------------------------------------------------------
// Auto-state rows
// ---------------------------------------------------------------------------

namespace {

/// The first visible-or-not widget of @p cls under @p root, depth first.
lv_obj_t* first_of_class(lv_obj_t* root, const lv_obj_class_t* cls) {
    if (root == nullptr)
        return nullptr;
    if (lv_obj_check_type(root, cls))
        return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); ++i) {
        if (lv_obj_t* hit = first_of_class(lv_obj_get_child(root, static_cast<int32_t>(i)), cls))
            return hit;
    }
    return nullptr;
}

lv_obj_t* state_dropdown(const char* key) {
    lv_obj_t* detail = named(("detail_" + std::string(key)).c_str());
    REQUIRE(detail != nullptr);
    lv_obj_t* row = lv_obj_get_sibling(detail, -1);
    REQUIRE(row != nullptr);
    lv_obj_t* dd = lv_obj_find_by_name(row, "dropdown");
    REQUIRE(dd != nullptr);
    return dd;
}

} // namespace

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: an auto-state row offers only the actions its targets support",
                 "[led][settings][auto_state_rows]") {
    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    // No strips, effects, WLED or macros: only the two universal actions.
    CHECK(std::string(lv_dropdown_get_options(state_dropdown("idle"))) == "Off\nBrightness");
    CHECK(std::string(lv_dropdown_get_options(state_dropdown("error"))) == "Off\nBrightness");
}

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a brightness action shows a slider that writes the mapping",
                 "[led][settings][auto_state_rows]") {
    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();

    lv_obj_t* dd = state_dropdown("printing");
    lv_dropdown_set_selected(dd, 1);
    lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();

    auto& as = helix::led::LedAutoState::instance();
    const auto* mapping = as.get_mapping("printing");
    REQUIRE(mapping != nullptr);
    CHECK(mapping->action_type == "brightness");
    CHECK(mapping->brightness == 100);
    CHECK_FALSE(lv_obj_is_hidden(named("detail_printing")));

    lv_obj_t* slider = first_of_class(named("ctx_printing"), &lv_slider_class);
    REQUIRE(slider != nullptr);
    CHECK(lv_slider_get_value(slider) == 100);
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
    settle();
    CHECK(as.get_mapping("printing")->brightness == 40);

    as.set_mapping("printing", {});
    as.save_config();
}

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a stored brightness action opens with its level",
                 "[led][settings][auto_state_rows]") {
    helix::led::LedStateAction action;
    action.action_type = "brightness";
    action.brightness = 55;
    helix::led::LedAutoState::instance().set_mapping("paused", action);

    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();

    CHECK(lv_dropdown_get_selected(state_dropdown("paused")) == 1);
    lv_obj_t* slider = first_of_class(named("ctx_paused"), &lv_slider_class);
    REQUIRE(slider != nullptr);
    CHECK(lv_slider_get_value(slider) == 55);

    helix::led::LedAutoState::instance().set_mapping("paused", {});
}

TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a macro device named with a leading # shows its name",
                 "[led][settings][macro_devices]") {
    auto& ctrl = helix::led::LedController::instance();
    helix::led::LedMacroInfo m;
    m.display_name = "#1 Strip";
    m.type = helix::led::MacroLedType::TOGGLE;
    m.toggle_macro = "STRIP_TOGGLE";
    ctrl.set_configured_macros({m});

    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    lv_obj_t* name = named("macro_device_name");
    REQUIRE(name != nullptr);
    CHECK(std::string(lv_label_get_text(name)) == "#1 Strip");
}

// The edit form is bound to one draft. Adding a device loads the new, blank
// device into it, so the form never offers the last device's name to save
// onto the new one.
TEST_CASE_METHOD(LedSettingsSwatchFixture,
                 "LED settings: a new macro device opens with a blank form",
                 "[led][settings][macro_devices]") {
    auto& ctrl = helix::led::LedController::instance();
    helix::led::LedMacroInfo m;
    m.display_name = "Strip";
    m.type = helix::led::MacroLedType::TOGGLE;
    m.toggle_macro = "STRIP_TOGGLE";
    ctrl.set_configured_macros({m});

    auto& overlay = helix::settings::get_led_settings_overlay();
    REQUIRE(overlay.show(lv_screen_active()));
    settle();
    lv_subject_t* name = lv_xml_get_subject(nullptr, "led_macro_draft_name");
    REQUIRE(name != nullptr);

    helix::settings::LedSettingsOverlayTestAccess::edit_macro(0);
    settle();
    REQUIRE(std::string(lv_subject_get_string(name)) == "Strip");

    helix::settings::LedSettingsOverlayTestAccess::add_macro();
    settle();
    CHECK(std::string(lv_subject_get_string(name)).empty());
}
