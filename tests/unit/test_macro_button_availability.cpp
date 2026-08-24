// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_macro_button_availability.cpp
 * @brief A configured macro button is only usable if the printer defines it
 *
 * Printer presets seed /standard_macros/<slot> from a template machine, so a
 * printer set up from someone else's preset can carry macro names its Klipper
 * has never heard of. Those names used to be taken at face value: the button
 * rendered as working, and the only feedback on a tap was an "Unknown command"
 * rejection from the printer.
 */

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "filament_op_dispatch.h"
#include "printer_discovery.h"
#include "standard_macros.h"

#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

/// Build a PrinterDiscovery whose macro list is exactly `macros`.
helix::PrinterDiscovery discovery_with(const std::vector<std::string>& macros) {
    helix::PrinterDiscovery hw;
    json objects = json::array({"extruder", "heater_bed"});
    for (const auto& m : macros) {
        objects.push_back("gcode_macro " + m);
    }
    hw.parse_objects(objects);
    return hw;
}

/// Point a slot at `macro` the way Settings > Macro Buttons persists it.
void configure_slot(const std::string& slot_name, const std::string& macro) {
    helix::Config::get_instance()->set<std::string>("/standard_macros/" + slot_name, macro);
}

} // namespace

// ============================================================================
// Configured macro validation
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Configured macro the printer defines stays usable",
                 "[standard_macros][macro_availability]") {
    configure_slot("clean_nozzle", "MY_WIPE");

    auto& macros = StandardMacros::instance();
    macros.reset();
    auto hw = discovery_with({"MY_WIPE"});
    macros.init(hw);

    const auto& info = macros.get(StandardMacroSlot::CleanNozzle);
    REQUIRE(info.configured_macro == "MY_WIPE");
    REQUIRE_FALSE(info.has_missing_macro());
    REQUIRE_FALSE(info.is_empty());
    REQUIRE(info.get_macro() == "MY_WIPE");
    REQUIRE(info.get_source() == MacroSource::CONFIGURED);
}

TEST_CASE_METHOD(HelixTestFixture, "Configured macro the printer lacks is not dispatchable",
                 "[standard_macros][macro_availability]") {
    // The reported case: a Voron seeded from the FlashForge AD5M preset. Its
    // Klipper defines neither CLEAR_NOZZLE nor any clean-nozzle alias, so the
    // slot has nothing to fall back to.
    configure_slot("clean_nozzle", "CLEAR_NOZZLE");

    auto& macros = StandardMacros::instance();
    macros.reset();
    auto hw = discovery_with({"PRINT_START", "PRINT_END", "G32"});
    macros.init(hw);

    const auto& info = macros.get(StandardMacroSlot::CleanNozzle);
    REQUIRE(info.has_missing_macro());
    REQUIRE(info.missing_macro == "CLEAR_NOZZLE");
    REQUIRE(info.configured_macro.empty());
    REQUIRE(info.is_empty());
    REQUIRE(info.get_macro().empty());
    REQUIRE(info.get_source() == MacroSource::NONE);
    // Still remembered, so the UI can name it and save_to_config() keeps it.
    REQUIRE(info.requested_macro() == "CLEAR_NOZZLE");
}

TEST_CASE_METHOD(HelixTestFixture, "Missing configured macro yields to a detected one",
                 "[standard_macros][macro_availability]") {
    configure_slot("clean_nozzle", "CLEAR_NOZZLE");

    auto& macros = StandardMacros::instance();
    macros.reset();
    auto hw = discovery_with({"CLEAN_NOZZLE"});
    macros.init(hw);

    const auto& info = macros.get(StandardMacroSlot::CleanNozzle);
    REQUIRE(info.has_missing_macro());
    REQUIRE_FALSE(info.is_empty());
    REQUIRE(info.get_macro() == "CLEAN_NOZZLE");
    REQUIRE(info.get_source() == MacroSource::DETECTED);
}

TEST_CASE_METHOD(HelixTestFixture, "Demoted macro survives a save of another slot",
                 "[standard_macros][macro_availability]") {
    // save_to_config() writes every slot. Persisting the resolved value instead
    // of the requested one would delete the user's clean_nozzle assignment the
    // first time they touched any other slot.
    configure_slot("clean_nozzle", "CLEAR_NOZZLE");

    auto& macros = StandardMacros::instance();
    macros.reset();
    auto hw = discovery_with({"PURGE"});
    macros.init(hw);
    REQUIRE(macros.get(StandardMacroSlot::CleanNozzle).has_missing_macro());

    macros.set_macro(StandardMacroSlot::Purge, "PURGE");

    REQUIRE(helix::Config::get_instance()->get<std::string>("/standard_macros/clean_nozzle", "") ==
            "CLEAR_NOZZLE");
}

// ============================================================================
// Reactivity — availability tracks the printer's live macro list
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Macro availability flips when the printer's macro list changes",
                 "[standard_macros][macro_availability][reactive]") {
    configure_slot("clean_nozzle", "CLEAR_NOZZLE");

    auto& macros = StandardMacros::instance();
    macros.reset();
    macros.init_subjects(false);

    lv_subject_t* version = macros.get_macros_version_subject();
    REQUIRE(version != nullptr);

    // Count notifications so this proves the subject actually fires, not just
    // that its value moved.
    static int notify_count = 0;
    notify_count = 0;
    lv_observer_t* obs = lv_subject_add_observer(
        version, [](lv_observer_t*, lv_subject_t*) { ++notify_count; }, nullptr);
    REQUIRE(obs != nullptr);

    const auto& info = macros.get(StandardMacroSlot::CleanNozzle);

    // 1. Printer has the macro — usable.
    auto with_macro = discovery_with({"CLEAR_NOZZLE"});
    macros.init(with_macro);
    REQUIRE_FALSE(info.is_empty());
    REQUIRE_FALSE(info.has_missing_macro());
    const int after_first = notify_count;
    REQUIRE(after_first > 0);

    // 2. Klipper restarts without it — the same slot goes unavailable, with no
    //    panel rebuild and no re-read of config in between.
    auto without_macro = discovery_with({"PRINT_START"});
    macros.init(without_macro);
    REQUIRE(info.is_empty());
    REQUIRE(info.has_missing_macro());
    REQUIRE(info.missing_macro == "CLEAR_NOZZLE");
    const int after_second = notify_count;
    REQUIRE(after_second > after_first);

    // 3. It comes back — so does the button.
    auto restored = discovery_with({"CLEAR_NOZZLE"});
    macros.init(restored);
    REQUIRE_FALSE(info.is_empty());
    REQUIRE_FALSE(info.has_missing_macro());
    REQUIRE(info.get_macro() == "CLEAR_NOZZLE");
    REQUIRE(notify_count > after_second);

    lv_observer_remove(obs);
}

// ============================================================================
// Filament dispatch — a phantom macro must not win tier 1
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Missing Load macro falls through to the raw-gcode tier",
                 "[standard_macros][macro_availability][filament]") {
    // A user-configured macro outranks even an AMS backend (plan_load()), so an
    // unverified name was the one input that could dead-end every tier below it.
    configure_slot("load_filament", "LOAD_FILAMENT");

    auto& macros = StandardMacros::instance();
    macros.reset();
    auto hw = discovery_with({"PRINT_START"}); // no LOAD_FILAMENT, no alias
    macros.init(hw);

    const auto& info = macros.get(StandardMacroSlot::LoadFilament);
    REQUIRE(info.has_missing_macro());

    AmsSystemInfo sys;
    helix::ui::BackendCaps caps; // no filament system present
    const auto plan = helix::ui::plan_load(sys, caps, -1, !info.is_empty(),
                                           info.get_source() == MacroSource::CONFIGURED);

    REQUIRE(plan.tier == helix::ui::FilamentTier::RawGcode);
}

TEST_CASE_METHOD(HelixTestFixture, "Present Load macro still wins tier 1",
                 "[standard_macros][macro_availability][filament]") {
    configure_slot("load_filament", "LOAD_FILAMENT");

    auto& macros = StandardMacros::instance();
    macros.reset();
    auto hw = discovery_with({"LOAD_FILAMENT"});
    macros.init(hw);

    const auto& info = macros.get(StandardMacroSlot::LoadFilament);
    REQUIRE_FALSE(info.has_missing_macro());

    AmsSystemInfo sys;
    helix::ui::BackendCaps caps;
    caps.present = true; // a filament system the configured macro must outrank
    caps.requires_slot_selection_for_load = true;
    const auto plan = helix::ui::plan_load(sys, caps, -1, !info.is_empty(),
                                           info.get_source() == MacroSource::CONFIGURED);

    REQUIRE(plan.tier == helix::ui::FilamentTier::Macro);
}

// ============================================================================
// Controls panel — the rendered button follows the printer, not the config file
// ============================================================================

namespace {

/// Builds the real Controls panel from XML so the assertions land on the
/// widget's own state, i.e. on the XML bindings as well as the C++ subjects.
class ControlsMacroFixture : public LVGLUITestFixture {
  public:
    ControlsMacroFixture() : panel(state(), nullptr) {}

    ~ControlsMacroFixture() override {
        if (panel_obj) {
            panel.on_deactivate();
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Re-run printer discovery with exactly `macros` defined.
    void connect_printer_with(const std::vector<std::string>& macros) {
        auto hw = discovery_with(macros);
        StandardMacros::instance().init(hw);
        settle();
    }

    void build_and_activate() {
        StandardMacros::instance().init_subjects(true);
        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        lv_obj_update_layout(test_screen());
        process_lvgl(20);
        panel.on_activate();
        settle();
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(10);
    }

    lv_obj_t* macro_button(const char* name) {
        lv_obj_t* btn = lv_obj_find_by_name(panel_obj, name);
        REQUIRE(btn != nullptr);
        return btn;
    }

    static bool hidden(lv_obj_t* o) {
        return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    static bool disabled(lv_obj_t* o) {
        return lv_obj_has_state(o, LV_STATE_DISABLED);
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsMacroFixture, "Quick macro button is usable when the printer has it",
                 "[ui_integration][controls][macro_availability]") {
    configure_slot("clean_nozzle", "MY_WIPE");
    StandardMacros::instance().reset();
    connect_printer_with({"MY_WIPE"});

    build_and_activate();

    lv_obj_t* btn = macro_button("btn_macro_1");
    CHECK_FALSE(hidden(btn));
    CHECK_FALSE(disabled(btn));
}

TEST_CASE_METHOD(ControlsMacroFixture, "Quick macro button is disabled when the printer lacks it",
                 "[ui_integration][controls][macro_availability][regression]") {
    // Four dead buttons since install was the reported symptom: they looked and
    // behaved like working buttons, and Klipper's "Unknown command" was the only
    // hint that anything was wrong.
    configure_slot("clean_nozzle", "CLEAR_NOZZLE");
    StandardMacros::instance().reset();
    connect_printer_with({"PRINT_START"});

    build_and_activate();

    lv_obj_t* btn = macro_button("btn_macro_1");
    CHECK_FALSE(hidden(btn)); // still where the user left it
    CHECK(disabled(btn));     // but plainly not usable
}

TEST_CASE_METHOD(ControlsMacroFixture,
                 "Quick macro button state follows a mid-session macro list change",
                 "[ui_integration][controls][macro_availability][reactive]") {
    configure_slot("clean_nozzle", "CLEAR_NOZZLE");
    StandardMacros::instance().reset();
    connect_printer_with({"CLEAR_NOZZLE"});

    build_and_activate();

    lv_obj_t* btn = macro_button("btn_macro_1");
    REQUIRE_FALSE(disabled(btn));

    // Klipper restarts without the macro. The panel is still on screen and is
    // never rebuilt or reactivated — only discovery re-runs.
    connect_printer_with({"PRINT_START"});
    CHECK(disabled(btn));
    CHECK_FALSE(hidden(btn));

    // ...and back again.
    connect_printer_with({"CLEAR_NOZZLE"});
    CHECK_FALSE(disabled(btn));
}
