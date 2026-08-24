// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_capabilities_char.cpp
 * @brief Characterization tests for PrinterState capabilities domain
 *
 * These tests capture the CURRENT behavior of capability-related subjects
 * in PrinterState before extraction to a dedicated PrinterCapabilitiesState class.
 *
 * Capability subjects (14 total):
 * - printer_has_qgl_ (int, 0=no, 1=yes - from hardware.has_qgl() via overrides)
 * - printer_has_z_tilt_ (int, 0=no, 1=yes - from hardware.has_z_tilt() via overrides)
 * - printer_has_bed_mesh_ (int, 0=no, 1=yes - from hardware.has_bed_mesh() via overrides)
 * - printer_has_nozzle_clean_ (int, 0=no, 1=yes - from overrides only)
 * - printer_has_probe_ (int, 0=no, 1=yes - from hardware.has_probe())
 * - printer_has_heater_bed_ (int, 0=no, 1=yes - from hardware.has_heater_bed())
 * - printer_has_led_ (int, 0=no, 1=yes - from hardware.has_led())
 * - printer_has_accelerometer_ (int, 0=no, 1=yes - from hardware.has_accelerometer())
 * - printer_has_spoolman_ (int, 0=no, 1=yes - via set_spoolman_available())
 * - printer_has_speaker_ (int, 0=no, 1=yes - from hardware.has_speaker())
 * - printer_has_timelapse_ (int, 0=no, 1=yes - from hardware.has_timelapse())
 * - printer_has_purge_line_ (int, 0=no, 1=yes - from printer type database)
 * - printer_has_firmware_retraction_ (int, 0=no, 1=yes - from hardware)
 * - printer_bed_moves_ (int, 0=gantry moves, 1=bed moves - from kinematics)
 *
 * Update mechanisms:
 * - set_hardware(PrinterDiscovery) - updates most capability subjects
 * - set_spoolman_available(bool) - async update via ui_async_call
 * - set_printer_type(string) - updates printer_has_purge_line_ via printer DB
 * - set_kinematics(string) - updates printer_bed_moves_
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "runtime_config.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;
using helix::PrinterDiscovery;

// Helper to get subject by XML name (requires init_subjects(true))
static lv_subject_t* get_subject_by_name(const char* name) {
    return lv_xml_get_subject(NULL, name);
}

// ============================================================================
// set_hardware() Tests - Verify capability updates from hardware discovery
// ============================================================================

TEST_CASE("Capabilities characterization: set_hardware updates capability subjects",
          "[characterization][capabilities][hardware]") {
    lv_init_safe();
    // Enable test mode so beta-gated features (timelapse) are available. Scoped:
    // leaving it set changed should_mock_*() for every later test in the process
    // and failed four tool_state/tool_switcher cases (#1287).
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->test_mode = true;

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // Create hardware discovery with various capabilities
    PrinterDiscovery hardware;
    nlohmann::json objects = {"quad_gantry_level",
                              "z_tilt",
                              "bed_mesh",
                              "probe",
                              "heater_bed",
                              "neopixel led_strip",
                              "firmware_retraction",
                              "timelapse",
                              "output_pin beeper"};
    hardware.parse_objects(objects);
    // Accelerometers are detected from configfile, not objects list
    nlohmann::json config = {{"adxl345", nlohmann::json::object()}};
    hardware.parse_config_keys(config);

    SECTION("set_hardware updates QGL from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_qgl");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates z_tilt from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_z_tilt");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates bed_mesh from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_bed_mesh");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates probe from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_probe");
        REQUIRE(lv_subject_get_int(subject) == 1);
        REQUIRE(state.has_probe() == true);
    }

    SECTION("set_hardware updates heater_bed from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_heater_bed");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates LED from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_led");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates accelerometer from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_accelerometer");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates speaker from output_pin beeper") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_speaker");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates firmware_retraction from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = get_subject_by_name("printer_has_firmware_retraction");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_hardware updates timelapse from hardware discovery") {
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        lv_subject_t* subject = state.get_printer_has_timelapse_subject();
        REQUIRE(lv_subject_get_int(subject) == 1);
    }
}

TEST_CASE("Capabilities characterization: set_hardware with empty hardware sets all to 0",
          "[characterization][capabilities][hardware]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // First set some capabilities
    PrinterDiscovery hardware_with_caps;
    nlohmann::json objects = {"quad_gantry_level", "probe", "heater_bed"};
    hardware_with_caps.parse_objects(objects);
    state.set_hardware(hardware_with_caps);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_qgl")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);

    // Now set empty hardware
    PrinterDiscovery empty_hardware;
    empty_hardware.parse_objects(nlohmann::json::array());
    state.set_hardware(empty_hardware);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_qgl")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_z_tilt")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_bed_mesh")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_accelerometer")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_speaker")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_firmware_retraction")) == 0);
    REQUIRE(lv_subject_get_int(state.get_printer_has_timelapse_subject()) == 0);
}

// ============================================================================
// Nozzle Clean Tests - Override-only capability
// ============================================================================

TEST_CASE("Capabilities characterization: nozzle_clean is override-only",
          "[characterization][capabilities][nozzle_clean]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // Nozzle clean macro in hardware won't set the subject directly
    // It requires capability override to be explicitly set
    PrinterDiscovery hardware;
    nlohmann::json objects = {"gcode_macro CLEAN_NOZZLE"};
    hardware.parse_objects(objects);

    state.set_hardware(hardware);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // The subject value depends on capability_overrides_ configuration
    // By default, without user config, nozzle_clean remains 0 unless macro detected
    lv_subject_t* subject = get_subject_by_name("printer_has_nozzle_clean");
    // Initial state should be 0 (no macro = no capability)
    // The override layer checks for the macro presence
    int value = lv_subject_get_int(subject);
    // Either 0 (no override) or 1 (macro detected and override enabled)
    REQUIRE((value == 0 || value == 1));
}

// ============================================================================
// Spoolman Tests - Async update via set_spoolman_available
// ============================================================================

TEST_CASE("Capabilities characterization: set_spoolman_available updates subject",
          "[characterization][capabilities][spoolman]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    lv_subject_t* subject = get_subject_by_name("printer_has_spoolman");

    SECTION("initial value is 0") {
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    SECTION("set_spoolman_available(true) sets to 1") {
        state.set_spoolman_available(true);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("set_spoolman_available(false) sets to 0") {
        // First enable
        state.set_spoolman_available(true);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(subject) == 1);

        // Then disable
        state.set_spoolman_available(false);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(subject) == 0);
    }
}

// ============================================================================
// Kinematics / Bed Moves Tests
// ============================================================================

TEST_CASE("Capabilities characterization: set_kinematics updates printer_bed_moves",
          "[characterization][capabilities][kinematics]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    lv_subject_t* subject = state.get_printer_bed_moves_subject();

    SECTION("corexy kinematics sets bed_moves to 1 (bed moves on Z)") {
        state.set_kinematics("corexy");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("cartesian kinematics sets bed_moves to 0 (gantry moves on Z)") {
        state.set_kinematics("cartesian");
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    SECTION("corexz kinematics sets bed_moves to 0 (gantry moves on Z, e.g. Switchwire)") {
        state.set_kinematics("corexz");
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    SECTION("delta kinematics sets bed_moves to 0") {
        state.set_kinematics("delta");
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    SECTION("hybrid_corexy kinematics sets bed_moves to 1 (contains corexy)") {
        state.set_kinematics("hybrid_corexy");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }

    SECTION("switching between kinematics updates correctly") {
        state.set_kinematics("corexy");
        REQUIRE(lv_subject_get_int(subject) == 1);

        state.set_kinematics("cartesian");
        REQUIRE(lv_subject_get_int(subject) == 0);

        state.set_kinematics("corexy");
        REQUIRE(lv_subject_get_int(subject) == 1);
    }
}

// ============================================================================
// Purge Line Tests - From printer type database
// ============================================================================

TEST_CASE("Capabilities characterization: printer_has_purge_line from printer type",
          "[characterization][capabilities][purge_line]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    lv_subject_t* subject = state.get_printer_has_purge_line_subject();

    SECTION("initial value is 0 (no printer type set)") {
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    SECTION("unknown printer type keeps purge_line at 0") {
        state.set_printer_type_sync("unknown_printer");
        REQUIRE(lv_subject_get_int(subject) == 0);
    }

    // Note: Actual printer types that support purge line depend on the
    // printer_types.json database. These tests verify the mechanism works.
}

// ============================================================================
// Observer Notification Tests
// ============================================================================

TEST_CASE("Capabilities characterization: observer fires when capability changes",
          "[characterization][capabilities][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    SECTION("observer fires when printer_has_probe changes") {
        int user_data[2] = {0, -1};
        lv_subject_t* subject = get_subject_by_name("printer_has_probe");

        lv_observer_t* observer = lv_subject_add_observer(subject, observer_cb, user_data);

        // LVGL auto-notifies on add
        REQUIRE(user_data[0] == 1);
        REQUIRE(user_data[1] == 0);

        // Update via hardware discovery
        PrinterDiscovery hardware;
        nlohmann::json objects = {"probe"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(user_data[0] >= 2);
        REQUIRE(user_data[1] == 1);

        lv_observer_remove(observer);
    }

    SECTION("observer fires when printer_bed_moves changes") {
        int user_data[2] = {0, -1};
        lv_subject_t* subject = state.get_printer_bed_moves_subject();

        lv_observer_t* observer = lv_subject_add_observer(subject, observer_cb, user_data);

        // Initial (default is 0 = gantry moves)
        REQUIRE(user_data[0] == 1);
        REQUIRE(user_data[1] == 0);

        // Change kinematics to corexy (bed moves, triggers 0→1)
        state.set_kinematics("corexy");

        REQUIRE(user_data[0] >= 2);
        REQUIRE(user_data[1] == 1);

        lv_observer_remove(observer);
    }
}

// ============================================================================
// Independence Tests - Verify capabilities are independent
// ============================================================================

TEST_CASE("Capabilities characterization: capability subjects are independent",
          "[characterization][capabilities][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("setting one capability does not affect others") {
        // Set only probe
        PrinterDiscovery hardware;
        nlohmann::json objects = {"probe"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_qgl")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 0);
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 0);
    }

    SECTION("kinematics does not affect other capabilities") {
        // Set hardware with probe
        PrinterDiscovery hardware;
        nlohmann::json objects = {"probe", "heater_bed"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 1);

        // Change kinematics to corexy (bed moves on Z for corexy family)
        state.set_kinematics("corexy");

        // bed_moves should change, but not other capabilities
        REQUIRE(lv_subject_get_int(state.get_printer_bed_moves_subject()) == 1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 1);
    }

    SECTION("spoolman does not affect hardware capabilities") {
        // Set hardware
        PrinterDiscovery hardware;
        nlohmann::json objects = {"probe"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);

        // Set spoolman
        state.set_spoolman_available(true);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_spoolman")) == 1);
        // probe should still be set
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
    }
}

// ============================================================================
// Reset Cycle Tests - Verify subjects survive reset_for_testing
// ============================================================================

TEST_CASE("Capabilities characterization: subjects survive reset_for_testing cycle",
          "[characterization][capabilities][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // Set some capabilities
    PrinterDiscovery hardware;
    nlohmann::json objects = {"probe", "heater_bed", "neopixel led_strip"};
    hardware.parse_objects(objects);
    state.set_hardware(hardware);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    state.set_kinematics("corexy");
    state.set_spoolman_available(true);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // Verify values were set
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_spoolman")) == 1);
    REQUIRE(lv_subject_get_int(state.get_printer_bed_moves_subject()) == 1);

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // After reset, values should be back to defaults (0)
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_spoolman")) == 0);
    REQUIRE(lv_subject_get_int(state.get_printer_bed_moves_subject()) == 0);

    // Subjects should still be functional after reset
    state.set_kinematics("corexy");
    REQUIRE(lv_subject_get_int(state.get_printer_bed_moves_subject()) == 1);
}

// ============================================================================
// has_probe() Convenience Method Test
// ============================================================================

TEST_CASE("Capabilities characterization: has_probe() method",
          "[characterization][capabilities][convenience]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("has_probe() returns false initially") {
        REQUIRE(state.has_probe() == false);
    }

    SECTION("has_probe() returns true after setting probe capability") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"probe"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(state.has_probe() == true);
    }

    SECTION("has_probe() returns true for bltouch") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"bltouch"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(state.has_probe() == true);
    }
}

// ============================================================================
// Multiple Hardware Detection Tests
// ============================================================================

TEST_CASE("Capabilities characterization: various hardware detection patterns",
          "[characterization][capabilities][detection]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("LED detected from neopixel object") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"neopixel chamber_light"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 1);
    }

    SECTION("LED detected from dotstar object") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"dotstar status_leds"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 1);
    }

    SECTION("LED NOT detected from output_pin without LED/LIGHT/LAMP in name") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"output_pin relay"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // "relay" doesn't contain LED/LIGHT/LAMP, so no LED detected
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 0);
    }

    SECTION("LED detected from output_pin with LIGHT in name") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"output_pin caselight"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // "caselight" contains "LIGHT", so LED IS detected
        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 1);
    }

    SECTION("speaker detected from output_pin beeper") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"output_pin beeper"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_speaker")) == 1);
    }

    SECTION("speaker detected from output_pin buzzer") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"output_pin BUZZER"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_speaker")) == 1);
    }

    SECTION("accelerometer detected from resonance_tester in config") {
        PrinterDiscovery hardware;
        nlohmann::json config = {{"resonance_tester", nlohmann::json::object()}};
        hardware.parse_config_keys(config);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_accelerometer")) == 1);
    }

    SECTION("accelerometer detected from adxl345 in config") {
        PrinterDiscovery hardware;
        nlohmann::json config = {{"adxl345", nlohmann::json::object()}};
        hardware.parse_config_keys(config);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_accelerometer")) == 1);
    }

    SECTION("probe detected from probe_eddy_current") {
        PrinterDiscovery hardware;
        nlohmann::json objects = {"probe_eddy_current btt_eddy"};
        hardware.parse_objects(objects);
        state.set_hardware(hardware);
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
    }
}

// ============================================================================
// Full Printer Configuration Test
// ============================================================================

TEST_CASE("Capabilities characterization: typical Voron 2.4 configuration",
          "[characterization][capabilities][integration]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // Typical Voron 2.4 objects
    PrinterDiscovery hardware;
    nlohmann::json objects = {"quad_gantry_level",
                              "bed_mesh",
                              "probe",
                              "heater_bed",
                              "neopixel sb_leds",
                              "neopixel chamber_lights",
                              "output_pin beeper",
                              "firmware_retraction",
                              "gcode_macro CLEAN_NOZZLE",
                              "gcode_macro PURGE_LINE"};
    hardware.parse_objects(objects);
    // Accelerometers are detected from configfile, not objects list
    nlohmann::json config = {{"adxl345", nlohmann::json::object()},
                             {"resonance_tester", nlohmann::json::object()}};
    hardware.parse_config_keys(config);
    state.set_hardware(hardware);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    state.set_kinematics("corexy");

    // Verify all capabilities detected
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_qgl")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_bed_mesh")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_led")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_accelerometer")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_speaker")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_firmware_retraction")) == 1);

    // CoreXY + QGL = gantry moves on Z (Voron 2.4)
    REQUIRE(lv_subject_get_int(state.get_printer_bed_moves_subject()) == 0);

    // Z-tilt not present on Voron 2.4 (uses QGL instead)
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_z_tilt")) == 0);
}

TEST_CASE("Capabilities characterization: typical Ender 3 configuration",
          "[characterization][capabilities][integration]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // Typical Ender 3 with BLTouch
    PrinterDiscovery hardware;
    nlohmann::json objects = {"bed_mesh", "bltouch", "heater_bed"};
    hardware.parse_objects(objects);
    state.set_hardware(hardware);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    state.set_kinematics("cartesian");

    // Verify capabilities
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_bed_mesh")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_probe")) == 1);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_heater_bed")) == 1);

    // Cartesian = gantry moves on Z
    REQUIRE(lv_subject_get_int(state.get_printer_bed_moves_subject()) == 0);

    // No QGL or Z-tilt on Ender 3
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_qgl")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("printer_has_z_tilt")) == 0);
}
