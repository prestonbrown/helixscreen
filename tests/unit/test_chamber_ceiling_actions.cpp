// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_ceiling_actions.cpp
 * @brief Chamber-heater backend ceiling fallback + fault-reset/filter-fan
 *        actions on TemperatureController (issue #1290).
 *
 * Ceiling: the configfile max_temp always wins; when the query is silent,
 * the matched backend's conservative cap applies (60 C for appliances), and
 * a generic backend (cap 0) keeps the heater default.
 *
 * Actions: reset_chamber_fault() / set_chamber_filter_fan() send
 * backend-provided gcode through the standard api; empty wiring is a no-op.
 * PrinterState::set_hardware wires both from the discovery-matched backend,
 * and a manual chamber-heater override detaches them again.
 *
 * ConfigfileMockClient (test_helpers/configfile_mock_client.h) answers the
 * configfile query from test-controlled sections.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "temperature_controller.h"
#include "test_helpers/configfile_mock_client.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct ChamberFixture : public LVGLTestFixture {
    ConfigfileMockClient client;
    helix::PrinterState state;
    MoonrakerAPI api;
    helix::TemperatureController controller;

    ChamberFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state),
          controller(state, &api) {
        state.init_subjects(false);
        helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
        // execute_gcode gates on klippy state; the subject defaults to SHUTDOWN.
        state.set_klippy_state_sync(helix::KlippyState::READY);
    }

    ~ChamberFixture() override {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    /// Resolve a dragonbreath chamber heater through the production path
    /// (discovery parse_objects → set_hardware chamber resolution).
    void discover_dragonbreath() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"heater_generic dragonbreath", "extruder", "heater_bed"};
        hardware.parse_objects(objects);
        REQUIRE(hardware.chamber_heater_name() == "heater_generic dragonbreath");
        REQUIRE(hardware.chamber_heater_backend_id() == "dragonbreath");
        state.set_hardware(hardware);
        REQUIRE(controller.resolved_name(helix::HeaterType::Chamber) ==
                "heater_generic dragonbreath");
    }
};

} // namespace

TEST_CASE("conservative ceiling applies when configfile silent", "[chamber][ceiling]") {
    ChamberFixture f;
    f.discover_dragonbreath();

    SECTION("backend conservative cap becomes the configured max") {
        // DragonBreath wiring: 60 = the backend's conservative chamber cap.
        f.controller.set_chamber_actions("", "", 60.0);

        f.controller.ensure_limits(helix::HeaterType::Chamber);
        helix::ui::UpdateQueue::instance().drain_until_empty();

        REQUIRE(f.controller.configured_max(helix::HeaterType::Chamber) == 60);
    }

    SECTION("generic backend (cap 0) keeps the heater default") {
        f.controller.set_chamber_actions("", "", 0.0);

        f.controller.ensure_limits(helix::HeaterType::Chamber);
        helix::ui::UpdateQueue::instance().drain_until_empty();

        // No fallback: the configured max stays unknown and the keypad keeps
        // its 80 C chamber default — current behavior for silent configfile.
        REQUIRE(f.controller.configured_max(helix::HeaterType::Chamber) == 0);
        REQUIRE(f.controller.keypad_range(helix::HeaterType::Chamber).max == 80.0f);
    }
}

TEST_CASE("configfile max_temp beats conservative ceiling", "[chamber][ceiling]") {
    ChamberFixture f;
    f.discover_dragonbreath();

    f.client.config_sections = {{"heater_generic dragonbreath", {{"max_temp", 75}}}};
    f.controller.set_chamber_actions("", "", 60.0);

    f.controller.ensure_limits(helix::HeaterType::Chamber);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    REQUIRE(f.controller.configured_max(helix::HeaterType::Chamber) == 75);
}

TEST_CASE("fault reset + filter fan send backend gcode", "[chamber][actions]") {
    ChamberFixture f;

    SECTION("backend actions round-trip as gcode") {
        f.controller.set_chamber_actions("DRAGONBREATH_RESET", "output_pin dragonbreath_filter",
                                         60.0);

        f.client.clear_gcode_script_history();
        f.controller.reset_chamber_fault();
        REQUIRE(f.client.gcode_script_history().size() == 1);
        REQUIRE(f.client.gcode_script_history()[0] == "DRAGONBREATH_RESET");

        // SET_PIN takes the BARE pin name — the "output_pin " prefix is stripped.
        f.client.clear_gcode_script_history();
        f.controller.set_chamber_filter_fan(true);
        REQUIRE(f.client.gcode_script_history().size() == 1);
        REQUIRE(f.client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=1");

        f.client.clear_gcode_script_history();
        f.controller.set_chamber_filter_fan(false);
        REQUIRE(f.client.gcode_script_history().size() == 1);
        REQUIRE(f.client.gcode_script_history()[0] == "SET_PIN PIN=dragonbreath_filter VALUE=0");
    }

    SECTION("empty wiring is a clean no-op") {
        f.controller.set_chamber_actions("", "", 0.0);

        f.client.clear_gcode_script_history();
        f.controller.reset_chamber_fault();
        f.controller.set_chamber_filter_fan(true);
        REQUIRE(f.client.gcode_script_history().empty());
    }
}

// The dryer takes the backend's commands, clamped to what the backend's
// cycle accepts, so a preset outside the range sends what the card shows.
TEST_CASE("chamber dryer start and stop send backend gcode", "[chamber][actions][dryer][1299]") {
    ChamberFixture f;

    SECTION("a backend with a dryer") {
        f.controller.set_chamber_dryer(helix::chamber::backend_by_id("panda_breath"));
        REQUIRE(f.controller.chamber_dryer().supported);

        f.client.fail_configfile = true; // no idle-timeout hold in this case
        f.client.clear_gcode_script_history();
        f.controller.start_chamber_drying(55.0f, 240);
        f.controller.start_chamber_drying(80.0f, 90); // above the cycle's ceiling
        f.controller.stop_chamber_drying();
        REQUIRE(f.client.gcode_script_history().size() == 3);
        CHECK(f.client.gcode_script_history()[0] == "PANDA_BREATH_DRY_START TEMP=55 HOURS=4");
        CHECK(f.client.gcode_script_history()[1] == "PANDA_BREATH_DRY_START TEMP=60 HOURS=2");
        CHECK(f.client.gcode_script_history()[2] == "PANDA_BREATH_DRY_STOP");
    }

    SECTION("no dryer is a clean no-op") {
        for (const auto* backend :
             {helix::chamber::backend_by_id("dragonbreath"),
              static_cast<const helix::chamber::ChamberHeaterBackend*>(nullptr)}) {
            f.controller.set_chamber_dryer(backend);
            CHECK_FALSE(f.controller.chamber_dryer().supported);
            f.client.clear_gcode_script_history();
            f.controller.start_chamber_drying(55.0f, 240);
            f.controller.stop_chamber_drying();
            CHECK(f.client.gcode_script_history().empty());
        }
    }
}

namespace {

constexpr const char* kBedOn = "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=70";
constexpr const char* kBedOff = "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0";

/// The dryer's running state as the printer reports it, delivered the way
/// production delivers it: a subject change the controller observes, drained
/// through the update queue.
void report_drying(ChamberFixture& f, bool running) {
    lv_subject_set_int(f.state.temperature_state().get_chamber_dryer_active_subject(),
                       running ? 1 : 0);
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

int count(const std::vector<std::string>& history, const std::string& line) {
    return static_cast<int>(std::count(history.begin(), history.end(), line));
}

} // namespace

// The bed assist heats the bed for the length of a drying cycle and turns it
// off however the cycle ends: our Stop, the appliance ending it (its timer or
// its own button), or the start being refused. It never touches a bed that a
// print or a hand-set target owns.
TEST_CASE("chamber dryer bed assist turns the bed off however the cycle ends",
          "[chamber][actions][dryer][1299]") {
    ChamberFixture f;
    f.controller.set_chamber_dryer(helix::chamber::backend_by_id("panda_breath"),
                                   /*has_heated_bed=*/true);
    REQUIRE(f.controller.chamber_dryer_bed_assist_c() == 70);
    report_drying(f, false);
    f.client.clear_gcode_script_history();

    SECTION("our Stop") {
        f.controller.start_chamber_drying(55.0f, 240, /*heat_bed=*/true);
        CHECK(count(f.client.gcode_script_history(), kBedOn) == 1);
        f.controller.stop_chamber_drying();
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 1);
    }

    SECTION("the appliance ends the cycle") {
        f.controller.start_chamber_drying(55.0f, 240, true);
        // Idle frames before the appliance picks the start up end nothing.
        report_drying(f, false);
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 0);
        report_drying(f, true);
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 0);
        report_drying(f, false);
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 1);
        // Ended once: a later Stop does not send a second off.
        f.controller.stop_chamber_drying();
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 1);
    }

    SECTION("a print owns the bed by the time the cycle ends") {
        f.controller.start_chamber_drying(55.0f, 240, true);
        report_drying(f, true);
        lv_subject_set_int(f.state.print_state().get_job_holds_machine_subject(), 1);
        report_drying(f, false);
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 0);
        lv_subject_set_int(f.state.print_state().get_job_holds_machine_subject(), 0);
    }

    SECTION("a target set by hand since") {
        f.controller.start_chamber_drying(55.0f, 240, true);
        report_drying(f, true);
        lv_subject_set_int(f.state.temperature_state().get_bed_target_subject(), 500);
        f.controller.stop_chamber_drying();
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 0);
    }

    SECTION("the start is refused") {
        f.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Unknown command",
                                        "PANDA_BREATH_DRY_START");
        f.controller.start_chamber_drying(55.0f, 240, true);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 1);
    }

    SECTION("no assist asked for, no bed commands") {
        f.controller.start_chamber_drying(55.0f, 240, false);
        report_drying(f, true);
        report_drying(f, false);
        f.controller.stop_chamber_drying();
        CHECK(count(f.client.gcode_script_history(), kBedOn) == 0);
        CHECK(count(f.client.gcode_script_history(), kBedOff) == 0);
    }

    SECTION("refused while a job holds the machine") {
        lv_subject_set_int(f.state.print_state().get_job_holds_machine_subject(), 1);
        f.controller.start_chamber_drying(55.0f, 240, true);
        CHECK(f.client.gcode_script_history().empty());
        lv_subject_set_int(f.state.print_state().get_job_holds_machine_subject(), 0);
    }

    SECTION("no heated bed, no assist") {
        f.controller.set_chamber_dryer(helix::chamber::backend_by_id("panda_breath"), false);
        CHECK(f.controller.chamber_dryer_bed_assist_c() == 0);
        f.controller.start_chamber_drying(55.0f, 240, true);
        CHECK(count(f.client.gcode_script_history(), kBedOn) == 0);
    }
}

TEST_CASE("set_hardware wires backend actions into the controller", "[chamber][actions]") {
    ChamberFixture f;

    // PrinterState hands the actions to the GLOBAL controller (app_globals),
    // so register the fixture's controller for the production wire-up.
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        &f.controller);

    SECTION("dragonbreath discovery arms reset gcode + filter pin") {
        f.discover_dragonbreath();

        f.client.clear_gcode_script_history();
        f.controller.reset_chamber_fault();
        f.controller.set_chamber_filter_fan(true);
        REQUIRE(f.client.gcode_script_history().size() == 2);
        REQUIRE(f.client.gcode_script_history()[0] == "DRAGONBREATH_RESET");
        REQUIRE(f.client.gcode_script_history()[1] == "SET_PIN PIN=dragonbreath_filter VALUE=1");
    }

    SECTION("the dryer follows the matched backend") {
        f.discover_dragonbreath();
        CHECK_FALSE(f.controller.chamber_dryer().supported);

        helix::PrinterDiscovery stock;
        stock.parse_objects(nlohmann::json{"heater_generic panda_breath", "panda_breath",
                                           "extruder", "heater_bed"});
        f.state.set_hardware(stock);
        CHECK(f.controller.chamber_dryer().supported);

        helix::SettingsManager::instance().set_chamber_heater_assignment("none");
        f.state.set_hardware(stock);
        CHECK_FALSE(f.controller.chamber_dryer().supported);
    }

    SECTION("manual chamber-heater override detaches the backend actions") {
        f.discover_dragonbreath();
        // Override to "none": the resolved heater is no longer the discovery
        // pick, so a fault reset / fan toggle would aim at the wrong heater —
        // the actions must clear along with the diagnostics source (Task 4).
        helix::SettingsManager::instance().set_chamber_heater_assignment("none");
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"heater_generic dragonbreath", "extruder", "heater_bed"};
        hardware.parse_objects(objects);
        f.state.set_hardware(hardware);

        f.client.clear_gcode_script_history();
        f.controller.reset_chamber_fault();
        f.controller.set_chamber_filter_fan(true);
        REQUIRE(f.client.gcode_script_history().empty());
    }

    // Drop the registration so later tests' set_hardware sees no controller
    // instead of this fixture's soon-to-be-destroyed one, and restore the
    // global chamber assignment the override section disturbed.
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        std::shared_ptr<helix::TemperatureController>{});
    helix::SettingsManager::instance().set_chamber_heater_assignment("auto");
}

TEST_CASE("set_hardware reads the chamber ceiling before any input surface asks",
          "[chamber][ceiling]") {
    ChamberFixture f;
    f.client.config_sections = {{"heater_generic chamber", {{"max_temp", "60"}}}};
    helix::PanelWidgetManager::instance().register_shared_resource<helix::TemperatureController>(
        &f.controller);
    struct Unregister {
        ~Unregister() {
            helix::PanelWidgetManager::instance()
                .register_shared_resource<helix::TemperatureController>(
                    std::shared_ptr<helix::TemperatureController>{});
        }
    } unregister;

    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"heater_generic chamber", "extruder", "heater_bed"});
    f.state.set_hardware(hardware);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // A label built from keypad_range() on first open shows the real cap.
    CHECK(f.controller.configured_max(helix::HeaterType::Chamber) == 60);
    CHECK(f.controller.keypad_range(helix::HeaterType::Chamber).max == 60.0f);
}

namespace {

void drain() {
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

std::vector<std::string> idle_lines(ChamberFixture& f) {
    std::vector<std::string> out;
    for (const auto& line : f.client.gcode_script_history()) {
        if (line.rfind("SET_IDLE_TIMEOUT", 0) == 0) {
            out.push_back(line);
        }
    }
    return out;
}

} // namespace

// A dry run moves nothing, so Klipper's idle_timeout would fire mid-run and run
// its gcode (TURN_OFF_HEATERS on most printers), zeroing the bed assist and the
// appliance's own heater. The run holds the timeout off for its length plus a
// margin and puts the configured value back however it ends.
TEST_CASE("chamber dryer holds Klipper's idle timeout for the run",
          "[chamber][actions][dryer][1299]") {
    ChamberFixture f;
    f.controller.set_chamber_dryer(helix::chamber::backend_by_id("panda_breath"),
                                   /*has_heated_bed=*/true);
    f.client.config_sections = {{"idle_timeout", {{"timeout", "300"}}}};
    report_drying(f, false);
    f.client.clear_gcode_script_history();

    // 240 min = 4 h run, plus the 30 min margin.
    const std::string hold = "SET_IDLE_TIMEOUT TIMEOUT=16200";
    const std::string restore = "SET_IDLE_TIMEOUT TIMEOUT=300";

    SECTION("our Stop restores it") {
        f.controller.start_chamber_drying(55.0f, 240);
        drain();
        CHECK(idle_lines(f) == std::vector<std::string>{hold});
        f.controller.stop_chamber_drying();
        CHECK(idle_lines(f) == std::vector<std::string>{hold, restore});
    }

    SECTION("the appliance ending the cycle restores it, once") {
        f.controller.start_chamber_drying(55.0f, 240);
        drain();
        report_drying(f, true);
        report_drying(f, false);
        CHECK(idle_lines(f) == std::vector<std::string>{hold, restore});
        f.controller.stop_chamber_drying();
        CHECK(idle_lines(f) == std::vector<std::string>{hold, restore});
    }

    SECTION("a refused start restores it") {
        f.client.force_next_gcode_error(MoonrakerErrorType::JSON_RPC_ERROR, "Unknown command",
                                        "PANDA_BREATH_DRY_START");
        f.controller.start_chamber_drying(55.0f, 240);
        drain();
        const auto lines = idle_lines(f);
        CHECK(std::count(lines.begin(), lines.end(), restore) ==
              std::count(lines.begin(), lines.end(), hold));
    }

    SECTION("a print running at the end owns the timeout") {
        f.controller.start_chamber_drying(55.0f, 240);
        drain();
        report_drying(f, true);
        lv_subject_set_int(f.state.print_state().get_job_holds_machine_subject(), 1);
        report_drying(f, false);
        CHECK(idle_lines(f) == std::vector<std::string>{hold});
        lv_subject_set_int(f.state.print_state().get_job_holds_machine_subject(), 0);
    }

    SECTION("without a bed assist the hold still applies") {
        f.controller.set_chamber_dryer(helix::chamber::backend_by_id("panda_breath"), false);
        f.controller.start_chamber_drying(55.0f, 240, true);
        drain();
        CHECK(idle_lines(f) == std::vector<std::string>{hold});
    }

    SECTION("no configured section: Klipper's default 600 is restored") {
        f.client.config_sections = nlohmann::json::object();
        f.controller.start_chamber_drying(55.0f, 240);
        drain();
        f.controller.stop_chamber_drying();
        CHECK(idle_lines(f) == std::vector<std::string>{hold, "SET_IDLE_TIMEOUT TIMEOUT=600"});
    }

    SECTION("no readable configfile: nothing is held, so nothing needs restoring") {
        f.client.fail_configfile = true;
        f.controller.start_chamber_drying(55.0f, 240);
        drain();
        f.controller.stop_chamber_drying();
        CHECK(idle_lines(f).empty());
    }

    SECTION("a run that ends before the configfile answer arrives holds nothing") {
        f.controller.start_chamber_drying(55.0f, 240);
        f.controller.stop_chamber_drying();
        drain();
        CHECK(idle_lines(f).empty());
    }
}
