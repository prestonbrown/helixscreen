// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's speed dropdown edits extrude_speed and a held length button edits
// its length setting; Extrude / Retract move at both.

#include "ui_button.h"
#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_panel_filament.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_macro_harness.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "core/update_queue.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

using TA = helix::ui::FilamentPanelTestAccess;
helix::SettingsManager& sm() {
    return helix::SettingsManager::instance();
}

constexpr int kDefaultLengths[] = {5, 10, 25};

/// Navigation panels and the numeric keypad, which a length button's hold opens.
struct KeypadHost {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    KeypadHost() {
        for (auto& p : panels) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }
    ~KeypadHost() {
        NavigationManager::instance().shutdown();
    }
    KeypadHost(const KeypadHost&) = delete;
    KeypadHost& operator=(const KeypadHost&) = delete;
};

/// The XML-built panel over the macro harness, observing live settings subjects.
struct SpeedHarness {
    LVGLUITestFixture& fx;
    bool settings_live = (sm().init_subjects(), true);
    helix::test::FilamentPanelMacroHarness h;
    int saved_speed = sm().get_extrude_speed();
    bool saved_cold = helix::SafetySettingsManager::instance().get_allow_cold_extrude();
    lv_obj_t* root = nullptr;
    lv_obj_t* dd = nullptr;

    explicit SpeedHarness(LVGLUITestFixture& f) : fx(f) {
        sm().set_extrude_speed(5);
        reset_lengths();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(true);
        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        h.panel->setup(root, fx.test_screen());
        dd = lv_obj_find_by_name(root, "extrude_speed_dropdown");
        REQUIRE(dd != nullptr);
        settle();
    }
    ~SpeedHarness() {
        lv_obj_delete(root);
        fx.process_lvgl(10);
        sm().set_extrude_speed(saved_speed);
        reset_lengths();
        helix::SafetySettingsManager::instance().set_allow_cold_extrude(saved_cold);
    }
    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        fx.process_lvgl(20);
        helix::ui::UpdateQueue::instance().drain();
    }
    /// The user picks option @p index from the open list.
    void choose(uint32_t index) {
        lv_dropdown_set_selected(dd, index);
        lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, nullptr);
        settle();
    }
    [[nodiscard]] std::string shown() const {
        return lv_dropdown_get_text(dd);
    }
    static void reset_lengths() {
        for (int i = 0; i < helix::SettingsManager::kExtrudeLengthPresetCount; ++i) {
            sm().set_extrude_length_preset(i, kDefaultLengths[i]);
        }
    }
    [[nodiscard]] bool sent(const std::string& needle) const {
        const auto& hist = h.client.gcode_script_history();
        return std::any_of(hist.begin(), hist.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        });
    }
};

/// Presses @p keys on the open keypad, then its Set, and lets the keypad close.
void type_and_confirm(SpeedHarness& s, std::initializer_list<const char*> keys) {
    lv_obj_t* keypad = lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
    REQUIRE(keypad != nullptr);
    for (const char* key : keys) {
        lv_obj_t* btn = lv_obj_find_by_name(keypad, key);
        REQUIRE(btn != nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
    }
    lv_xml_get_event_cb(nullptr, "on_keypad_confirm")(nullptr);
    s.fx.process_lvgl(600);
    s.settle();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: the speed dropdown sets the extrude speed",
                 "[filament][extrude_speed]") {
    SpeedHarness s(*this);
    CHECK(s.shown() == "5 mm/s");
    s.choose(5); // SettingsManager::kExtrudeSpeedPresets[5]
    CHECK(sm().get_extrude_speed() == 10);
    CHECK(s.shown() == "10 mm/s");

    // Machine Limits may set a speed that is not a preset.
    sm().set_extrude_speed(7);
    s.settle();
    CHECK(s.shown() == "7 mm/s");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Filament panel: extrude and retract move at the chosen speed",
                 "[filament][extrude_speed]") {
    SpeedHarness s(*this);
    s.choose(7); // 20 mm/s
    s.h.client.clear_gcode_script_history();
    SECTION("extrude") {
        TA::handle_extrude_button(*s.h.panel);
        s.settle();
        CHECK(s.sent("G1 E10 F1200"));
    }
    SECTION("retract") {
        TA::handle_retract_button(*s.h.panel);
        s.settle();
        CHECK(s.sent("G1 E-10 F1200"));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: a length button's keypad sets its length in range, or resets it",
                 "[filament][extrude_speed][extrude_length]") {
    KeypadHost host;
    SpeedHarness s(*this);
    lv_obj_t* button = lv_obj_find_by_name(s.root, "extrude_length_2");
    const auto shown = [&] { return std::string(lv_label_get_text(ui_button_get_label(button))); };
    REQUIRE(s.h.panel->handle_extrude_length_longpress(2));
    s.settle();

    for (auto out_of_range : {std::initializer_list<const char*>{"btn_5", "btn_0", "btn_1"},
                              std::initializer_list<const char*>{"btn_0"}}) {
        type_and_confirm(s, out_of_range);
        CHECK(ui_keypad_is_visible());
        CHECK(sm().get_extrude_length_preset(2) == 25);
        type_and_confirm(s, {"btn_backspace", "btn_backspace", "btn_backspace"});
    }
    type_and_confirm(s, {"btn_4", "btn_0"});
    CHECK(sm().get_extrude_length_preset(2) == 40);
    CHECK(shown() == "40mm");
    s.h.client.clear_gcode_script_history();
    TA::handle_extrude_button(*s.h.panel); // the edited button is the chosen one
    s.settle();
    CHECK(s.sent("G1 E40 F300"));

    REQUIRE(s.h.panel->handle_extrude_length_longpress(2));
    s.settle();
    type_and_confirm(s, {"btn_default"});
    CHECK(sm().get_extrude_length_preset(2) == 25);
    CHECK(shown() == "25mm");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Filament panel: a held length button opens its keypad and is not also tapped",
                 "[filament][extrude_length]") {
    KeypadHost host;
    SpeedHarness s(*this);
    s.h.client.clear_gcode_script_history();

    lv_obj_t* chip = lv_obj_find_by_name(s.root, "extrude_length_0");
    REQUIRE(chip != nullptr);
    lv_obj_update_layout(s.root);
    lv_area_t a;
    lv_obj_get_coords(chip, &a);
    lv_point_t p{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &p) == chip);
    int taps = 0;
    lv_obj_add_event_cb(
        chip, [](lv_event_t* e) { ++*static_cast<int*>(lv_event_get_user_data(e)); },
        LV_EVENT_CLICKED, &taps);

    helix_test::ScopedPointerIndev finger;
    finger.press(p.x, p.y);
    finger.hold(finger.long_press_hold_ms());
    finger.release(p.x, p.y);
    s.settle();

    CHECK(ui_keypad_is_visible());
    CHECK(taps == 0);
    CHECK(s.h.client.gcode_script_history().empty());
}
