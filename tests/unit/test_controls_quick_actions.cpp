// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_controls_quick_actions.cpp
 * @brief The guard contract behind the ControlsPanel quick-action buttons.
 *
 * Homing, Quad Gantry Level and Z-Tilt all run through one guarded body, so the
 * rules that body enforces are the rules for every one of the seven buttons: a
 * second tap while an operation is live is refused, a tap with no printer
 * connection says so instead of doing nothing, and the guard is released on both
 * terminal outcomes rather than only on success.
 *
 * The dispatch is supplied by the test, so these assertions need no printer —
 * what is under test is the bookkeeping around the command, not the gcode.
 */

#include "ui_panel_controls.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "../test_helpers/controls_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "safety_settings_manager.h"
#include "standard_macros.h"

#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

using helix::ui::ControlsPanelTestAccess;
using helix::ui::UpdateQueueTestAccess;

namespace {

/// A QuickActionText whose fields are distinguishable but otherwise uninteresting.
ControlsPanelTestAccess::QuickActionText probe_text() {
    return {"started", "completed", "guard timed out", "rpc timed out", "failed: {}"};
}

void drain() {
    UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "Quick action refuses to dispatch with no API",
                 "[controls][quick_action]") {
    ControlsPanel panel(state(), nullptr);

    bool dispatched = false;
    ControlsPanelTestAccess::run_quick_action(
        panel, 1000, probe_text(),
        [&dispatched](IMoonrakerAPI::SuccessCallback, IMoonrakerAPI::ErrorCallback) {
            dispatched = true;
        });

    REQUIRE_FALSE(dispatched);
    // The guard must stay clear, or the button is dead until the timeout fires.
    REQUIRE_FALSE(ControlsPanelTestAccess::guard_active(panel));
}

TEST_CASE_METHOD(XMLTestFixture, "Quick action refuses a second dispatch while one is live",
                 "[controls][quick_action]") {
    ControlsPanel panel(state(), &api());

    int dispatches = 0;
    // Hold the first tap's callbacks instead of completing them, so the guard
    // stays armed across the second tap.
    IMoonrakerAPI::SuccessCallback held_ok;
    ControlsPanelTestAccess::run_quick_action(
        panel, 1000, probe_text(),
        [&](IMoonrakerAPI::SuccessCallback ok, IMoonrakerAPI::ErrorCallback) {
            ++dispatches;
            held_ok = std::move(ok);
        });
    REQUIRE(dispatches == 1);
    REQUIRE(ControlsPanelTestAccess::guard_active(panel));

    ControlsPanelTestAccess::run_quick_action(
        panel, 1000, probe_text(),
        [&](IMoonrakerAPI::SuccessCallback, IMoonrakerAPI::ErrorCallback) { ++dispatches; });

    REQUIRE(dispatches == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "Quick action releases the guard on success",
                 "[controls][quick_action]") {
    ControlsPanel panel(state(), &api());

    ControlsPanelTestAccess::run_quick_action(
        panel, 1000, probe_text(),
        [](IMoonrakerAPI::SuccessCallback ok, IMoonrakerAPI::ErrorCallback) { ok(); });
    drain();

    REQUIRE_FALSE(ControlsPanelTestAccess::guard_active(panel));
}

TEST_CASE_METHOD(XMLTestFixture, "Quick action releases the guard on failure",
                 "[controls][quick_action]") {
    ControlsPanel panel(state(), &api());

    ControlsPanelTestAccess::run_quick_action(
        panel, 1000, probe_text(),
        [](IMoonrakerAPI::SuccessCallback, IMoonrakerAPI::ErrorCallback err) {
            err(MoonrakerError::validation_error("test", "boom"));
        });
    drain();

    REQUIRE_FALSE(ControlsPanelTestAccess::guard_active(panel));
}

TEST_CASE("Homing quick actions carry a message for every outcome", "[controls][quick_action]") {
    // A quick action with an empty `completed` leaves the user watching a button
    // that never reports finishing. Holding all five strings in one struct is
    // what makes that omission visible, so assert none of them is blank.
    const auto text = ControlsPanelTestAccess::homing_text("Homing X...");

    REQUIRE(text.started == std::string("Homing X..."));
    REQUIRE_FALSE(text.completed.empty());
    REQUIRE_FALSE(text.guard_timed_out.empty());
    REQUIRE_FALSE(text.rpc_timed_out.empty());
    REQUIRE(text.failed_fmt.find("{}") != std::string::npos);
}

// =============================================================================
// Saved parameter defaults on the macro quick buttons
// =============================================================================

namespace {

/// True when the mock client ran a script containing @p needle.
bool script_sent_containing(const MoonrakerClientMock& client, const std::string& needle) {
    for (const auto& script : client.gcode_script_history()) {
        if (script.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A ControlsPanel built the way production builds it (XML object, setup,
/// activate), running against a mock client so the dispatched gcode is
/// observable. The macro slot must be configured before build_and_activate():
/// refresh_macro_buttons() reads it during setup().
class QuickButtonMacroFixture : public LVGLUITestFixture {
  public:
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI mock_api{mock_client, state()};
    ControlsPanel panel{state(), &mock_api};
    lv_obj_t* panel_obj = nullptr;

    QuickButtonMacroFixture() {
        state().init_subjects(false);
        state().set_klippy_state_sync(helix::KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        helix::SafetySettingsManager::instance().set_macro_require_confirmation(false);
    }

    ~QuickButtonMacroFixture() override {
        if (panel_obj) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        helix::SafetySettingsManager::instance().set_macro_require_confirmation(true);
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        mock_client.disconnect();
    }

    void build_and_activate() {
        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        lv_obj_update_layout(test_screen());
        process_lvgl(20);
        panel.on_activate();
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(20);
    }

    /// Point quick button 1 at a macro that declares TEMP.
    void configure_macro(const char* name) {
        nlohmann::json config;
        config[std::string("gcode_macro ") + name]["gcode"] =
            "{% set TEMP = params.TEMP|default(200)|int %}\nG1 E10";
        helix::MacroParamCache::instance().populate_from_configfile(config, {name});
        REQUIRE(helix::MacroParamCache::instance().get(name).knowledge ==
                helix::MacroParamKnowledge::KNOWN_PARAMS);

        StandardMacros::instance().reset();
        StandardMacros::instance().set_macro(StandardMacroSlot::CleanNozzle, name);
    }
};

} // namespace

TEST_CASE_METHOD(QuickButtonMacroFixture,
                 "Quick button runs a macro with its saved parameter defaults",
                 "[controls][quick_action][macro]") {
    // The slot's macro declares TEMP; a saved ask-off record holds TEMP=210.
    // The quick button raises no param modal on this surface, so the saved
    // value is what the run sends.
    configure_macro("MY_MACRO");
    helix::MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}};
    record.ask_for_params = false;
    helix::MacroParamDefaults::instance().set("MY_MACRO", record);
    build_and_activate();

    ControlsPanelTestAccess::execute_macro(panel, 0);
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(script_sent_containing(mock_client, "MY_MACRO"));
    CHECK(script_sent_containing(mock_client, "MY_MACRO TEMP=210"));
}

TEST_CASE_METHOD(QuickButtonMacroFixture, "Quick button sends no params without a saved record",
                 "[controls][quick_action][macro]") {
    configure_macro("MY_MACRO");
    build_and_activate();

    ControlsPanelTestAccess::execute_macro(panel, 0);
    helix::ui::UpdateQueue::instance().drain();

    // No record: the macro runs bare, the way it always has.
    REQUIRE(script_sent_containing(mock_client, "MY_MACRO"));
    CHECK_FALSE(script_sent_containing(mock_client, "MY_MACRO TEMP"));
}
