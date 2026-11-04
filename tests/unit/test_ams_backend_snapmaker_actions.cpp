// tests/unit/test_ams_backend_snapmaker_actions.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../helix_test_fixture.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/snapmaker_test_access.h"
#include "ams_backend_snapmaker.h"
#include "config.h"
#include "printer_discovery.h"
#include "settings_manager.h"
#include "snapmaker_nozzle_offset.h"
#include "test_helpers/backend_harness.h"

#include <algorithm>
#include <any>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::printer;

namespace {
/// An unwrapped status object carrying only print_task_config — the shape the
/// initial query response sends and handle_status() takes.
nlohmann::json frame(const nlohmann::json& ptc) {
    nlohmann::json p = nlohmann::json::object();
    p["print_task_config"] = ptc;
    return p;
}
const DeviceAction* find_action(const std::vector<DeviceAction>& as, const std::string& id) {
    auto it = std::find_if(as.begin(), as.end(), [&](const DeviceAction& a) { return a.id == id; });
    return it == as.end() ? nullptr : &(*it);
}

class CapturingU1 : public AmsBackendSnapmaker {
  public:
    using AmsBackendSnapmaker::AmsBackendSnapmaker;
    std::vector<std::string> captured;
    AmsError execute_gcode(const std::string& gcode) override {
        captured.push_back(gcode);
        return AmsErrorHelper::success();
    }
};

/// The per-tool unload-at-end choice lives in a SettingsManager subject, which
/// answers its default until init_subjects() runs.
struct EndUnloadFixture : LVGLTestFixture {
    EndUnloadFixture() {
        helix::Config::get_instance();
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().set_end_of_print_unload_tools(0);
    }
    ~EndUnloadFixture() override {
        SettingsManager::instance().set_end_of_print_unload_tools(0);
    }
};
} // namespace

TEST_CASE("snapmaker exposes its firmware settings as device actions",
          "[ams][snapmaker][actions]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"auto_replenish_filament", true},
                               {"replenish_ignore_color", false},
                               {"filament_entangle_detect", false},
                               {"filament_entangle_sen", "medium"},
                               {"end_led_turn_off", true}}));
    const auto actions = backend.get_device_actions();

    // Auto-replenish is the generic Endless Spool switch, not a device action.
    REQUIRE(find_action(actions, "snapmaker_auto_replenish") == nullptr);

    const DeviceAction* color = find_action(actions, "snapmaker_replenish_ignore_color");
    REQUIRE(color != nullptr);
    REQUIRE(color->type == ActionType::TOGGLE);
    REQUIRE(std::any_cast<bool>(color->current_value) == false);

    const DeviceAction* tangle = find_action(actions, "snapmaker_entangle_detect");
    REQUIRE(tangle != nullptr);
    REQUIRE(tangle->type == ActionType::TOGGLE);
    REQUIRE(std::any_cast<bool>(tangle->current_value) == false);

    const DeviceAction* sen = find_action(actions, "snapmaker_entangle_sen");
    REQUIRE(sen != nullptr);
    REQUIRE(sen->type == ActionType::DROPDOWN);
    REQUIRE(sen->options == std::vector<std::string>{"low", "medium", "high"});
    REQUIRE(std::any_cast<std::string>(sen->current_value) == "medium");
}

TEST_CASE("a setting the firmware has not reported is not offered", "[ams][snapmaker][actions]") {
    // Offering a toggle whose state we do not know would render it off and
    // invite the user to "change" it to the value it already has.
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    const auto actions = backend.get_device_actions();
    REQUIRE(find_action(actions, "snapmaker_replenish_ignore_color") == nullptr);
    REQUIRE_FALSE(backend.get_endless_spool_capabilities().available());
}

TEST_CASE_METHOD(EndUnloadFixture, "end-unload offers one toggle per reported toolhead",
                 "[ams][snapmaker][actions][end_unload]") {
    // The firmware's list says how many heads there are; the values are ours.
    // It clears every entry when a print ends, so its echo is not the choice.
    SettingsManager::instance().set_end_of_print_unload_tools(0b0001);
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, true, true, true}}}));
    const auto actions = backend.get_device_actions();
    REQUIRE(find_action(actions, "snapmaker_end_unload_t0") != nullptr);
    REQUIRE(find_action(actions, "snapmaker_end_unload_t3") != nullptr);
    REQUIRE(find_action(actions, "snapmaker_end_unload_t4") == nullptr);
    REQUIRE(std::any_cast<bool>(find_action(actions, "snapmaker_end_unload_t0")->current_value) ==
            true);
    REQUIRE(std::any_cast<bool>(find_action(actions, "snapmaker_end_unload_t1")->current_value) ==
            false);
}

TEST_CASE_METHOD(EndUnloadFixture, "end-unload toggles carry their tool for the UI to name",
                 "[ams][snapmaker][actions][end_unload]") {
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}}}));
    const auto actions = backend.get_device_actions();
    const DeviceAction* t1 = find_action(actions, "snapmaker_end_unload_t1");
    REQUIRE(t1 != nullptr);
    CHECK(t1->label == "Unload at end");
    CHECK(t1->tool_index == 1);
    CHECK(t1->section == "print_prefs");
}

TEST_CASE_METHOD(EndUnloadFixture, "quick end-unload toggles compose instead of wiping each other",
                 "[ams][snapmaker][actions][end_unload]") {
    // Each toggle sends the whole list. The firmware echo has not caught up
    // between taps a second apart, so a list built from it would carry only
    // the latest tap.
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}}}));
    backend.execute_device_action("snapmaker_end_unload_t1", std::any(true));
    backend.execute_device_action("snapmaker_end_unload_t2", std::any(true));
    backend.execute_device_action("snapmaker_end_unload_t3", std::any(true));

    REQUIRE(SettingsManager::instance().get_end_of_print_unload_tools() == 0b1110);
    REQUIRE(backend.build_preference_gcode("snapmaker_end_unload_t0", std::any(false)) ==
            "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[0,1,1,1]");

    backend.execute_device_action("snapmaker_end_unload_t2", std::any(false));
    REQUIRE(SettingsManager::instance().get_end_of_print_unload_tools() == 0b1010);
}

TEST_CASE_METHOD(EndUnloadFixture, "every print start carries the saved end-unload list",
                 "[ams][snapmaker][actions][end_unload]") {
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;

    SECTION("nothing before the firmware reports how many heads it has") {
        SettingsManager::instance().set_end_of_print_unload_tools(0b0101);
        REQUIRE(backend.build_preprint_gcode({}, {}).empty());
    }

    SECTION("the list comes from the setting, not the cleared echo") {
        backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}}}));
        SettingsManager::instance().set_end_of_print_unload_tools(0b0101);
        REQUIRE(backend.build_preprint_gcode({}, {}) ==
                "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,1,0]");
    }

    SECTION("all off is sent too, so a stale firmware list cannot ride into the print") {
        backend.fire_status(frame({{"end_unload_filament", {true, true, true, true}}}));
        REQUIRE(backend.build_preprint_gcode({}, {}) ==
                "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[0,0,0,0]");
    }

    SECTION("it rides after the extruder map of a print with known tools") {
        backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}}}));
        SettingsManager::instance().set_end_of_print_unload_tools(0b0001);
        const std::string gcode = backend.build_preprint_gcode({0}, {});
        REQUIRE(gcode.find("SET_PRINT_EXTRUDER_MAP") != std::string::npos);
        const std::string tail = "\nSET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,0,0]";
        REQUIRE(gcode.size() > tail.size());
        REQUIRE(gcode.compare(gcode.size() - tail.size(), tail.size(), tail) == 0);
    }
}

TEST_CASE_METHOD(
    EndUnloadFixture,
    "a soft-filament head's unload toggle is disabled with a reason, and its saved choice survives",
    "[ams][snapmaker][actions][end_unload]") {
    // The firmware's end macro skips a soft-filament head, so a toggle reading
    // "on" there would promise an unload that never happens.
    SettingsManager::instance().set_end_of_print_unload_tools(0b0011);
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}},
                               {"filament_soft", {false, true, false, false}}}));

    const auto actions = backend.get_device_actions();
    const DeviceAction* soft = find_action(actions, "snapmaker_end_unload_t1");
    REQUIRE(soft != nullptr);
    REQUIRE_FALSE(soft->enabled);
    REQUIRE_FALSE(soft->disable_reason.empty());
    REQUIRE(std::any_cast<bool>(soft->current_value) == false);
    REQUIRE(find_action(actions, "snapmaker_end_unload_t0")->enabled);

    REQUIRE(backend.build_preference_gcode("snapmaker_end_unload_t1", std::any(true)).empty());
    // Listed disabled, so the device-action gate refuses it before the backend.
    REQUIRE(backend.execute_device_action("snapmaker_end_unload_t1", std::any(true)).result ==
            AmsResult::WRONG_STATE);
    // The saved choice stays for when the head holds a hard filament again.
    REQUIRE(SettingsManager::instance().get_end_of_print_unload_tools() == 0b0011);

    // The soft head goes out as 0 whatever is saved: the firmware's end macro
    // reads its own soft flag, which can disagree with the loaded material.
    REQUIRE(backend.build_preprint_gcode({}, {}) ==
            "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,0,0]");
    REQUIRE(backend.build_preference_gcode("snapmaker_end_unload_t2", std::any(true)) ==
            "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,1,0]");

    // Rigid filament back in the head: the saved choice reads on again.
    backend.fire_status(frame({{"filament_soft", {false, false, false, false}}}));
    const auto after = backend.get_device_actions();
    REQUIRE(find_action(after, "snapmaker_end_unload_t1")->enabled);
    REQUIRE(std::any_cast<bool>(find_action(after, "snapmaker_end_unload_t1")->current_value) ==
            true);
}

TEST_CASE_METHOD(EndUnloadFixture,
                 "a soft material is soft even when the firmware's soft flag says otherwise",
                 "[ams][snapmaker][actions][end_unload]") {
    // The firmware derives filament_soft from its own table keyed by the exact
    // FILAMENT_TYPE, so a type it has no row for (TPU-85A) reads rigid.
    SettingsManager::instance().set_end_of_print_unload_tools(0b1111);
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}},
                               {"filament_type", {"PLA", "TPU-85A", "PEBA", "ASA"}},
                               {"filament_soft", {false, false, false, false}}}));

    const auto actions = backend.get_device_actions();
    for (const char* id : {"snapmaker_end_unload_t1", "snapmaker_end_unload_t2"}) {
        const DeviceAction* soft = find_action(actions, id);
        REQUIRE(soft != nullptr);
        CHECK_FALSE(soft->enabled);
        CHECK_FALSE(soft->disable_reason.empty());
        CHECK(std::any_cast<bool>(soft->current_value) == false);
    }
    for (const char* id : {"snapmaker_end_unload_t0", "snapmaker_end_unload_t3"}) {
        const DeviceAction* rigid = find_action(actions, id);
        REQUIRE(rigid != nullptr);
        CHECK(rigid->enabled);
        CHECK(std::any_cast<bool>(rigid->current_value) == true);
    }

    REQUIRE(backend.build_preprint_gcode({}, {}) ==
            "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,0,1]");
    REQUIRE(backend.build_preference_gcode("snapmaker_end_unload_t1", std::any(true)).empty());
    REQUIRE(SettingsManager::instance().get_end_of_print_unload_tools() == 0b1111);
}

TEST_CASE_METHOD(EndUnloadFixture,
                 "soft grades and flexible names read soft; rigid families stay rigid",
                 "[ams][snapmaker][actions][end_unload]") {
    SettingsManager::instance().set_end_of_print_unload_tools(0b1111);
    const auto enabled = [](const std::vector<std::string>& types) {
        helix::test::StartedBackend<AmsBackendSnapmaker> backend;
        backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}},
                                   {"filament_type", types},
                                   {"filament_soft", {false, false, false, false}}}));
        const auto actions = backend.get_device_actions();
        std::vector<bool> out;
        for (int t = 0; t < 4; ++t) {
            const DeviceAction* a =
                find_action(actions, "snapmaker_end_unload_t" + std::to_string(t));
            REQUIRE(a != nullptr);
            out.push_back(a->enabled);
        }
        return out;
    };
    CHECK(enabled({"TPU95A", "TPU+", "PVA+", "Flex"}) ==
          std::vector<bool>{false, false, false, false});
    CHECK(enabled({"flexible", "PLA", "PETG", "PA-CF"}) ==
          std::vector<bool>{false, true, true, true});
    // A soft family name only counts as a whole word or a grade of it.
    CHECK(enabled({"TPUX", "PLA+", "TPE", "TPU-Soft"}) ==
          std::vector<bool>{true, true, false, false});
}

TEST_CASE("executing a toggle emits only that field", "[ams][snapmaker][actions]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_led_turn_off", false}}));
    const std::string g = backend.build_preference_gcode("snapmaker_end_led_off", std::any(true));
    REQUIRE(g == "SET_PRINT_PREFERENCES END_LED_TURN_OFF=1");
}

TEST_CASE_METHOD(EndUnloadFixture, "executing the per-tool toggle rewrites the whole array",
                 "[ams][snapmaker][actions][end_unload]") {
    // END_UNLOAD_FILAMENT takes the full list, so flipping one tool must send
    // the others at their saved values or they are cleared.
    SettingsManager::instance().set_end_of_print_unload_tools(0b0101);
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, false, false, false}}}));
    const std::string g = backend.build_preference_gcode("snapmaker_end_unload_t1", std::any(true));
    REQUIRE(g == "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,1,1,0]");
}

TEST_CASE("an unknown action id produces no gcode", "[ams][snapmaker][actions]") {
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    REQUIRE(backend.build_preference_gcode("not_a_real_action", std::any(true)).empty());
}

TEST_CASE("the replenish and tangle toggles each emit only their own field",
          "[ams][snapmaker][actions]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(
        frame({{"replenish_ignore_color", false}, {"filament_entangle_detect", true}}));
    REQUIRE(backend.build_preference_gcode("snapmaker_replenish_ignore_color", std::any(true)) ==
            "SET_PRINT_PREFERENCES REPLENISH_IGNORE_COLOR=1");
    REQUIRE(backend.build_preference_gcode("snapmaker_entangle_detect", std::any(false)) ==
            "SET_PRINT_PREFERENCES FILAMENT_ENTANGLE_DETECT=0");
}

TEST_CASE("an overlong tool suffix is refused rather than parsed", "[ams][snapmaker][actions]") {
    // stoul throws out_of_range past ULONG_MAX, and this mapping runs inside
    // the UI callback that invoked the action. No real tool index is that
    // large, so an all-digit suffix this long can only be a malformed id.
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    REQUIRE(
        backend.build_preference_gcode("snapmaker_end_unload_t99999999999999999999", std::any(true))
            .empty());
}

TEST_CASE("a value whose type does not match the action is refused, not thrown",
          "[ams][snapmaker][actions]") {
    // The overlay types each action's value by the control it renders, so a
    // mismatched type is a malformed payload and must read as "not ours" —
    // the same refusal a malformed id gets — because a bad_any_cast out of
    // here would escape the UI-thread action callback uncaught.
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"end_unload_filament", {false, false}}}));
    REQUIRE(
        backend
            .build_preference_gcode("snapmaker_replenish_ignore_color", std::any(std::string("on")))
            .empty());
    REQUIRE(backend.build_preference_gcode("snapmaker_entangle_detect", std::any(std::string("on")))
                .empty());
    REQUIRE(backend.build_preference_gcode("snapmaker_end_led_off", std::any(std::string("on")))
                .empty());
    REQUIRE(backend.build_preference_gcode("snapmaker_end_unload_t0", std::any(std::string("on")))
                .empty());
    REQUIRE(backend.build_preference_gcode("snapmaker_entangle_sen", std::any(true)).empty());

    // The public entry the UI callback drives must refuse the same way rather
    // than let the exception cross the virtual.
    const AmsError err = backend.execute_device_action("snapmaker_end_led_off", std::any(1));
    REQUIRE(err.result == AmsResult::NOT_SUPPORTED);
}

namespace {
/// The objects a stock U1 lists for its toolhead offset calibration.
helix::PrinterDiscovery nozzle_offset_discovery() {
    nlohmann::json objects = nlohmann::json::array({"gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
                                                    "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"});
    for (int t = 0; t < 4; ++t) {
        for (const char* step :
             {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
            objects.push_back(std::string("gcode_macro _EXTRUDER_OFFSET_ACTION_") + step + "_T" +
                              std::to_string(t));
        }
    }
    helix::PrinterDiscovery d;
    d.parse_objects(objects);
    return d;
}
} // namespace

TEST_CASE("toolhead offset calibration is not a Multi-Filament setting",
          "[ams][snapmaker][actions][nozzle_offset]") {
    // It lives under Controls > Calibration, through helix::tool_offset_calibration.
    HelixTestFixture fixture;
    AmsBackendSnapmaker backend(nullptr, nullptr);
    backend.set_discovery(nozzle_offset_discovery());
    for (const auto& a : backend.get_device_actions()) {
        CHECK(a.guided_flow != snapmaker::nozzle_offset::kGuidedFlowId);
    }
    CHECK(backend.get_device_sections().empty());
}

TEST_CASE("snapmaker auto-replenish is the Endless Spool switch", "[ams][snapmaker][endless]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<CapturingU1> backend;

    SECTION("unreported, the switch is not offered and refuses") {
        CHECK_FALSE(backend.get_endless_spool_capabilities().available());
        CHECK_FALSE(backend.set_endless_spool_enabled(true).success());
        CHECK(backend.captured.empty());
    }

    SECTION("the reported flag is the enabled state") {
        backend.fire_status(frame({{"auto_replenish_filament", false}}));
        auto caps = backend.get_endless_spool_capabilities();
        CHECK(caps.available());
        CHECK(caps.switchable);
        CHECK(caps.enabled == EndlessSpoolEnabled::Off);

        backend.fire_status(frame({{"auto_replenish_filament", true}}));
        CHECK(backend.get_endless_spool_capabilities().enabled == EndlessSpoolEnabled::On);
    }

    SECTION("switching it sends the firmware preference") {
        backend.fire_status(frame({{"auto_replenish_filament", false}}));
        REQUIRE(backend.set_endless_spool_enabled(true).success());
        REQUIRE(backend.captured.size() == 1);
        CHECK(backend.captured[0] == "SET_PRINT_PREFERENCES AUTO_REPLENISH_FILAMENT=1");
    }
}

TEST_CASE("the retired auto-replenish action id is refused and sends nothing",
          "[ams][snapmaker][actions][endless_spool]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<CapturingU1> backend;
    backend.fire_status(frame({{"auto_replenish_filament", false}}));
    CHECK(backend.build_preference_gcode("snapmaker_auto_replenish", std::any(true)).empty());
    CHECK_FALSE(
        backend.execute_device_action("snapmaker_auto_replenish", std::any(true)).success());
    CHECK(backend.captured.empty());
}

TEST_CASE("an auto-replenish change announces a state change and a repeat does not",
          "[ams][snapmaker][actions][endless_spool]") {
    HelixTestFixture fixture;
    helix::test::StartedBackend<AmsBackendSnapmaker> backend;
    backend.fire_status(frame({{"auto_replenish_filament", false}}));
    int changes = 0;
    backend.set_event_callback([&](const std::string& ev, const std::string&) {
        if (ev == AmsBackend::EVENT_STATE_CHANGED) {
            ++changes;
        }
    });

    backend.fire_status(frame({{"auto_replenish_filament", true}}));
    CHECK(changes == 1);
    backend.fire_status(frame({{"auto_replenish_filament", true}}));
    CHECK(changes == 1);
}
