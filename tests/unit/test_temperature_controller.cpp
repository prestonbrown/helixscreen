// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../include/moonraker_client_mock.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/scoped_shared_resource.h"
#include "../test_helpers/temperature_controller_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "ams_backend_indx.h"
#include "app_globals.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "temperature_controller.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::HeaterType;
using helix::TemperatureController;

namespace {
// HelixTestFixture base drains UpdateQueue on both construction and
// destruction. PrinterState::update_from_status() unconditionally forwards
// every status update to the FilamentSensorManager singleton, which queues an
// async subject-refresh callback whenever it has not yet processed a status
// (or a sensor changed) and is not in sync mode - true even for an
// otherwise-private, per-fixture PrinterState like `state` below.
// feed_nozzle()'s calls into that path queue such a callback between test
// cases; the HelixTestFixture base drains it at teardown so it can't leak
// into the next test.
struct ControllerFixture : public HelixTestFixture {
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPI api;
    TemperatureController controller;

    ControllerFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state),
          controller(state, &api) {
        state.init_subjects(false);
    }
};
} // namespace

TEST_CASE("TemperatureController resolves heater names", "[temp_controller]") {
    ControllerFixture f;

    SECTION("bed is heater_bed") {
        REQUIRE(f.controller.resolved_name(HeaterType::Bed) == "heater_bed");
    }
    SECTION("chamber uses the resolved discovery name, never the bare default") {
        // Drive the REAL resolution path: PrinterState::set_hardware() resolves the
        // chamber heater from discovery into temperature_state_, exactly as it does
        // in production. "auto" assignment makes resolution use the discovery name.
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"heater_generic chamber_heater", "extruder", "heater_bed"};
        hardware.parse_objects(objects);
        f.state.set_hardware(hardware);

        REQUIRE(f.controller.resolved_name(HeaterType::Chamber) == "heater_generic chamber_heater");
    }
    SECTION("nozzle is the active extruder") {
        REQUIRE(f.controller.resolved_name(HeaterType::Nozzle) ==
                f.state.temperature_state().active_extruder_name());
    }
}

TEST_CASE("TemperatureController clamps keypad range to configured max", "[temp_controller]") {
    ControllerFixture f;

    SECTION("unknown configured max falls back to the heater default") {
        // Nozzle default ceiling is 350; nothing fetched yet.
        REQUIRE(f.controller.keypad_range(HeaterType::Nozzle).max == 350.0f);
    }
    SECTION("known configured max wins") {
        helix::TemperatureControllerTestAccess::set_max(f.controller, HeaterType::Chamber, 50);
        REQUIRE(f.controller.keypad_range(HeaterType::Chamber).max == 50.0f);
        REQUIRE(f.controller.configured_max(HeaterType::Chamber) == 50);
    }
}

TEST_CASE("TemperatureController preset visibility honors configured max", "[temp_controller]") {
    ControllerFixture f;
    helix::TemperatureControllerTestAccess::set_max(f.controller, HeaterType::Chamber, 50);
    REQUIRE(f.controller.preset_visible(HeaterType::Chamber, 40));
    REQUIRE(f.controller.preset_visible(HeaterType::Chamber, 50));
    REQUIRE_FALSE(f.controller.preset_visible(HeaterType::Chamber, 60));
    // Slot 2 of the chamber's enclosure ladder is still 60 — hiding it above the
    // configured max must not change the underlying value.
    REQUIRE(f.controller.presets(HeaterType::Chamber).material[2] == 60);
}

TEST_CASE("TemperatureController set_target routes to the resolved name", "[temp_controller]") {
    // Verify that set_target(HeaterType::Chamber, ...) sends the RESOLVED heater name
    // (e.g. "heater_generic chamber_heater" → bare object name "chamber_heater" in the
    // gcode), never the bare klipper type prefix that would be rejected by the firmware.
    ControllerFixture f;

    // Resolve chamber via the production path: PrinterState::set_hardware resolves the
    // chamber heater from discovery into temperature_state_, same as production.
    helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    helix::PrinterDiscovery hardware;
    nlohmann::json objects = {"heater_generic chamber_heater", "extruder", "heater_bed"};
    hardware.parse_objects(objects);
    f.state.set_hardware(hardware);

    // Confirm resolution before we exercise set_target
    REQUIRE(f.controller.resolved_name(HeaterType::Chamber) == "heater_generic chamber_heater");

    // execute_gcode gates on klippy state; the LVGL klippy_state subject defaults to
    // SHUTDOWN, so drive it to READY before exercising the gcode path.
    f.state.set_klippy_state_sync(helix::KlippyState::READY);

    // End-to-end check: set_target(HeaterType::Chamber, ...) passes the *resolved* chamber
    // name ("heater_generic chamber_heater", NOT the bare "chamber") down to
    // api_->set_temperature. The controller does NOT manipulate the name itself — the gcode
    // layer (build_heater_gcode inside MoonrakerAPI) strips the "heater_generic " prefix to
    // produce a valid "HEATER=chamber_heater". We can only observe the result: on_success
    // fires and on_error does not, which confirms the gcode layer received a usable
    // resolved name and emitted a valid HEATER= value (the mock returns an error for a
    // malformed one).
    bool success_fired = false;
    bool error_fired = false;
    f.controller.set_target(
        HeaterType::Chamber, 45.0,
        helix::SendOptions{.toast = false,
                           .on_success = [&] { success_fired = true; },
                           .on_error = [&](const MoonrakerError&) { error_fired = true; }});

    REQUIRE(success_fired);
    REQUIRE_FALSE(error_fired);

    // Verify the RPC method used was printer.gcode.script (confirms the gcode path ran)
    REQUIRE(f.client.last_send_method() == "printer.gcode.script");
}

// A Voron 2.4 whose heater_generic, chamber sensor and exhaust-fan M141 all say
// "chamber" while its only temperature_fan is ptc_fan (prestonbrown/helixscreen#1886).
// M141 there sets a fan Klipper does not have, so the chamber row's target must go
// straight to the heater.
TEST_CASE("TemperatureController sets a heater_generic chamber directly past a fan-only M141",
          "[temp_controller][chamber][m141]") {
    ControllerFixture f;
    auto& settings = helix::SettingsManager::instance();
    settings.set_chamber_heater_assignment("auto");
    settings.set_chamber_sensor_assignment("auto");

    auto& macros = helix::MacroParamCache::instance();
    struct CacheReset {
        helix::MacroParamCache& cache;
        ~CacheReset() {
            cache.clear();
        }
    } cache_reset{macros};
    macros.populate_from_configfile(
        {{"gcode_macro m141",
          {{"gcode", "SET_TEMPERATURE_FAN_TARGET temperature_fan=chamber "
                     "target={params.S|default(0)}"}}}},
        {"M141"});
    REQUIRE(macros.has_macro("m141"));

    helix::PrinterDiscovery hardware;
    hardware.parse_objects({"extruder", "heater_bed", "heater_generic chamber",
                            "temperature_sensor chamber", "temperature_fan ptc_fan",
                            "gcode_macro M141"});
    f.state.set_hardware(hardware);
    REQUIRE(f.controller.resolved_name(HeaterType::Chamber) == "heater_generic chamber");
    f.state.set_klippy_state_sync(helix::KlippyState::READY);

    f.controller.set_target(HeaterType::Chamber, 60.0, helix::SendOptions{.toast = false});

    REQUIRE(f.client.last_send_method() == "printer.gcode.script");
    CHECK(f.client.last_send_script() == "SET_HEATER_TEMPERATURE HEATER=chamber TARGET=60");
}

TEST_CASE("TemperatureController reports an error when the chamber heater is not found",
          "[temp_controller]") {
    // A Voron 2.4 with NO chamber heater configured: the chamber resolves to an empty
    // name. Setting a chamber target must NOT silently no-op — it must surface the
    // not-found condition (toast + on_error) so the user knows nothing happened.
    ControllerFixture f;

    // No chamber heater in discovery → resolved chamber name is empty.
    helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
    helix::PrinterDiscovery hardware;
    nlohmann::json objects = {"extruder", "heater_bed"};
    hardware.parse_objects(objects);
    f.state.set_hardware(hardware);
    f.state.set_klippy_state_sync(helix::KlippyState::READY);

    REQUIRE(f.controller.resolved_name(HeaterType::Chamber).empty());

    SECTION("toast=true: on_error fires and no gcode is sent") {
        f.client.clear_gcode_script_history();
        bool error_fired = false;
        bool success_fired = false;
        f.controller.set_target(
            HeaterType::Chamber, 50.0,
            helix::SendOptions{.toast = true,
                               .on_success = [&] { success_fired = true; },
                               .on_error = [&](const MoonrakerError&) { error_fired = true; }});

        REQUIRE(error_fired);
        REQUIRE_FALSE(success_fired);
        // No temperature gcode was sent to the printer.
        REQUIRE(f.client.gcode_script_history().empty());
        REQUIRE(f.client.last_send_method().empty());
    }

    SECTION("toast=false: stays a clean no-op (on_error not invoked, no gcode)") {
        f.client.clear_gcode_script_history();
        bool error_fired = false;
        bool success_fired = false;
        f.controller.set_target(
            HeaterType::Chamber, 50.0,
            helix::SendOptions{.toast = false,
                               .on_success = [&] { success_fired = true; },
                               .on_error = [&](const MoonrakerError&) { error_fired = true; }});

        REQUIRE_FALSE(error_fired);
        REQUIRE_FALSE(success_fired);
        REQUIRE(f.client.gcode_script_history().empty());
        REQUIRE(f.client.last_send_method().empty());
    }
}

TEST_CASE(
    "TemperatureController sends a preset's missing chamber heater target only to discovery's",
    "[temp_controller][chamber]") {
    // A model preset names its family's chamber heater before the wizard runs; this
    // member of the family does not have it.
    struct AssignmentRestore {
        ~AssignmentRestore() {
            helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
        }
    } restore;
    ControllerFixture f;
    // No M141 macro, so the raw heater or fan command is what reaches Klipper.
    helix::MacroParamCache::instance().clear();
    helix::SettingsManager::instance().set_chamber_heater_assignment(
        "heater_generic chamber_heater");

    const auto discover = [&f](std::initializer_list<const char*> names) {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = nlohmann::json::array();
        for (const char* name : names) {
            objects.push_back(name);
        }
        hardware.parse_objects(objects);
        f.state.set_hardware(hardware);
        f.state.set_klippy_state_sync(helix::KlippyState::READY);
        f.client.clear_gcode_script_history();
    };
    const auto sent = [&f](const std::string& fragment) {
        for (const auto& gcode : f.client.gcode_script_history()) {
            if (gcode.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    SECTION("a chamber-named temperature_fan takes the chamber target") {
        discover({"temperature_fan chamber_fan", "temperature_sensor chamber_temp", "extruder",
                  "heater_bed"});
        CHECK(f.controller.resolved_name(HeaterType::Chamber) == "temperature_fan chamber_fan");

        bool error_fired = false;
        f.controller.set_target(
            HeaterType::Chamber, 35.0,
            helix::SendOptions{.toast = true,
                               .on_error = [&](const MoonrakerError&) { error_fired = true; }});

        CHECK_FALSE(error_fired);
        CHECK(sent("SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber_fan TARGET=35"));
        CHECK_FALSE(sent("chamber_heater"));
    }

    SECTION("with neither a heater nor a chamber fan") {
        discover({"temperature_sensor chamber_temp", "extruder", "heater_bed"});
        CHECK(f.controller.resolved_name(HeaterType::Chamber).empty());

        SECTION("a chamber target is refused and nothing reaches Klipper") {
            bool error_fired = false;
            f.controller.set_target(
                HeaterType::Chamber, 50.0,
                helix::SendOptions{.toast = true,
                                   .on_error = [&](const MoonrakerError&) { error_fired = true; }});

            CHECK(error_fired);
            CHECK(f.client.gcode_script_history().empty());
        }
    }
}

// --------------------------------------------------------------------------
// Swap-preheat guard: keep_previous_hot floors the nozzle target at the hotter
// of the latched last-nonzero target and the current actual nozzle temp so a
// filament switch never drops below what's needed to purge the old material.
// --------------------------------------------------------------------------
namespace {
// Drive the latch + actual nozzle temp through the REAL status path. Feeding a
// non-zero target latches last_nonzero_target; a later 0 clears the live target
// but the latch survives. `temperature` sets the actual nozzle temp subject.
void feed_nozzle(helix::PrinterState& state, double actual_c, double target_c) {
    nlohmann::json status = {{"extruder", {{"temperature", actual_c}, {"target", target_c}}}};
    state.update_from_status(status);
}

// The last temperature gcode the controller emitted (e.g. "SET_HEATER_TEMPERATURE
// HEATER=extruder TARGET=250"). Empty if nothing was sent. Returned exactly as
// it reached the wire — the send layer decorates nothing (test_gcode_verbatim.cpp).
std::string last_temp_gcode(const MoonrakerClientMock& client) {
    const auto& hist = client.gcode_script_history();
    if (hist.empty()) {
        return {};
    }
    return hist.back();
}
} // namespace

TEST_CASE("TemperatureController swap-preheat guard holds the previous filament temp",
          "[temp_controller][temperature][swap_preheat]") {
    ControllerFixture f;
    f.state.temperature_state().init_extruders({"extruder"});
    f.state.set_klippy_state_sync(helix::KlippyState::READY);

    SECTION("raises a lower request to the latched target when the nozzle is still hot") {
        // ABS at 250 loaded/hot; switch to TPU at 230. Latch=250, actual=245.
        feed_nozzle(f.state, /*actual=*/245.0, /*target=*/250.0);
        REQUIRE(f.state.temperature_state().get_active_extruder_last_nonzero_target() ==
                Catch::Approx(250.0));

        f.client.clear_gcode_script_history();
        f.controller.set_target(helix::HeaterType::Nozzle, 230.0,
                                helix::SendOptions{.toast = false, .keep_previous_hot = true});

        // Floor = max(latch 250, actual 245) = 250 → request 230 is raised to 250.
        REQUIRE(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=250");
    }

    SECTION("raises to the current actual temp when it exceeds both request and latch") {
        // Nozzle cooled to a live 0 target but is physically at 255; latch=250.
        feed_nozzle(f.state, /*actual=*/255.0, /*target=*/250.0);
        feed_nozzle(f.state, /*actual=*/255.0, /*target=*/0.0); // target→0, latch survives at 250

        f.client.clear_gcode_script_history();
        f.controller.set_target(helix::HeaterType::Nozzle, 230.0,
                                helix::SendOptions{.toast = false, .keep_previous_hot = true});

        // Floor = max(latch 250, actual 255) = 255.
        REQUIRE(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=255");
    }

    SECTION("cold nozzle with no latch sends the requested target unchanged") {
        // Never heated: latch=0, actual=25. Request 230 stands.
        feed_nozzle(f.state, /*actual=*/25.0, /*target=*/0.0);
        REQUIRE(f.state.temperature_state().get_active_extruder_last_nonzero_target() ==
                Catch::Approx(0.0));

        f.client.clear_gcode_script_history();
        f.controller.set_target(helix::HeaterType::Nozzle, 230.0,
                                helix::SendOptions{.toast = false, .keep_previous_hot = true});

        REQUIRE(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=230");
    }

    SECTION("cooldown-to-0 without the flag is never guarded") {
        // Latch=250 present, but keep_previous_hot is off → request 0 must send 0.
        feed_nozzle(f.state, /*actual=*/245.0, /*target=*/250.0);

        f.client.clear_gcode_script_history();
        f.controller.set_target(helix::HeaterType::Nozzle, 0.0,
                                helix::SendOptions{.toast = false, .keep_previous_hot = false});

        REQUIRE(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0");
    }

    SECTION("guard is nozzle-only: bed is unaffected by the nozzle latch") {
        // Latch the nozzle at 250, then set the bed to 60 WITH keep_previous_hot.
        feed_nozzle(f.state, /*actual=*/245.0, /*target=*/250.0);

        f.client.clear_gcode_script_history();
        f.controller.set_target(helix::HeaterType::Bed, 60.0,
                                helix::SendOptions{.toast = false, .keep_previous_hot = true});

        REQUIRE(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60");
    }
}

TEST_CASE("get_temperature_controller returns the registered shared resource",
          "[temp_controller][globals]") {
    helix_test::ScopedSharedResource<helix::TemperatureController> scope(
        std::make_shared<helix::TemperatureController>(get_printer_state(), nullptr));
    REQUIRE(get_temperature_controller() == scope.ptr());
}

namespace {

/// A shared-heater backend that refuses heat with no tool mounted.
helix::PrinterDiscovery indx_discovery() {
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array(
        {"indx", "save_variables", "extruder", "heater_bed", "gcode_macro TOOL_POSITIONS",
         "gcode_macro _TOOLCHANGE_VARS", "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL"}));
    hw.parse_config_keys(
        nlohmann::json{{"gcode_macro TOOL_POSITIONS", {{"variable_tool_count", "3"}}}});
    return hw;
}

nlohmann::json mounted(int tool) {
    return {{"save_variables", {{"variables", {{"active_tool", tool}}}}}};
}

} // namespace

TEST_CASE("TemperatureController refuses nozzle heat the backend blocks",
          "[temp_controller][indx][heat]") {
    ControllerFixture f;
    f.state.temperature_state().init_extruders({"extruder"});
    helix::test::RegisteredBackend<helix::test::GcodeCaptureBackend<helix::AmsBackendIndx>> harness;
    harness->set_discovery(indx_discovery());
    harness->feed(mounted(-1));

    f.client.clear_gcode_script_history();
    bool refused = false;
    f.controller.set_target(
        helix::HeaterType::Nozzle, 210.0,
        helix::SendOptions{.toast = false,
                           .on_error = [&](const MoonrakerError&) { refused = true; }});
    CHECK(refused);
    CHECK(last_temp_gcode(f.client).empty());

    // Turning the heater off is always allowed.
    f.controller.set_target(helix::HeaterType::Nozzle, 0.0, helix::SendOptions{.toast = false});
    CHECK(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0");

    // The bed has nothing to do with the nozzle gate.
    f.controller.set_target(helix::HeaterType::Bed, 60.0, helix::SendOptions{.toast = false});
    CHECK(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60");

    // A mounted tool heats.
    harness->feed(mounted(1));
    f.controller.set_target(helix::HeaterType::Nozzle, 210.0, helix::SendOptions{.toast = false});
    CHECK(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210");
}

TEST_CASE("TemperatureController refuses heat on a mounted tool with no hotend",
          "[temp_controller][indx][heat]") {
    ControllerFixture f;
    f.state.temperature_state().init_extruders({"extruder"});
    helix::test::RegisteredBackend<helix::test::GcodeCaptureBackend<helix::AmsBackendIndx>> harness;
    harness->set_discovery(indx_discovery());
    harness->feed(
        nlohmann::json{{"save_variables", {{"variables", {{"active_tool", 2}}}}},
                       {"gcode_macro TOOL_POSITIONS",
                        {{"tool_count", 3}, {"no_heat_tools", nlohmann::json::array({2})}}}});
    REQUIRE(harness->get_current_tool() == 2);

    f.client.clear_gcode_script_history();
    f.controller.set_target(helix::HeaterType::Nozzle, 210.0, helix::SendOptions{.toast = false});
    CHECK(last_temp_gcode(f.client).empty());

    // The same tool, once it is not listed as a pen or cutter, heats.
    harness->feed(nlohmann::json{
        {"gcode_macro TOOL_POSITIONS", {{"no_heat_tools", nlohmann::json::array()}}}});
    f.controller.set_target(helix::HeaterType::Nozzle, 210.0, helix::SendOptions{.toast = false});
    CHECK(last_temp_gcode(f.client) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210");
}
