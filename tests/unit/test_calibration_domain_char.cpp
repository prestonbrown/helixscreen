// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_calibration_domain_char.cpp
 * @brief Characterization tests for PrinterState Calibration/Config domain
 *
 * These tests capture the CURRENT behavior of calibration-related subjects
 * in PrinterState before extraction to a dedicated PrinterCalibrationState class.
 *
 * Calibration subjects (7 total):
 *
 * Firmware Retraction (4 subjects):
 * - retract_length_ (int, centimillimeters - 0.8mm stored as 80)
 * - retract_speed_ (int, mm/s - integer, e.g., 35)
 * - unretract_extra_length_ (int, centimillimeters - 0.05mm stored as 5)
 * - unretract_speed_ (int, mm/s - integer, e.g., 25)
 *
 * Manual Probe (2 subjects):
 * - manual_probe_active_ (int, 0=inactive, 1=active)
 * - manual_probe_z_position_ (int, microns - 0.125mm stored as 125)
 *
 * Motor State (1 subject):
 * - motors_enabled_ (int, 0=disabled/Idle, 1=enabled/Ready/Printing)
 *
 * Update mechanisms:
 * - Firmware retraction: update_from_status() with "firmware_retraction" JSON object
 * - Manual probe: update_from_status() with "manual_probe" JSON object
 * - Motor state: update_from_status() with "idle_timeout" JSON object
 *
 * Key behaviors:
 * - Retraction lengths stored as centimillimeters (x100) for 0.01mm precision
 * - Manual probe Z position stored as microns (x1000) for 0.001mm precision
 * - Motor state derived from idle_timeout.state string ("Ready"/"Printing" vs "Idle")
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using namespace helix;

// Helper to get subject by XML name (requires init_subjects(true))
static lv_subject_t* get_subject_by_name(const char* name) {
    return lv_xml_get_subject(NULL, name);
}

// ============================================================================
// Initial Value Tests - Document default initialization behavior
// ============================================================================

TEST_CASE("Calibration characterization: initial values after init",
          "[characterization][calibration][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true); // Need XML registration to lookup by name

    SECTION("retract_speed initializes to 20 mm/s") {
        lv_subject_t* subject = get_subject_by_name("retract_speed");
        REQUIRE(subject != nullptr);
        REQUIRE(lv_subject_get_int(subject) == 20);
    }

    SECTION("unretract_speed initializes to 10 mm/s") {
        lv_subject_t* subject = get_subject_by_name("unretract_speed");
        REQUIRE(subject != nullptr);
        REQUIRE(lv_subject_get_int(subject) == 10);
    }

    SECTION("motors_enabled initializes to 1 (enabled)") {
        // Default is enabled (Ready state) per printer_state.cpp:256
        lv_subject_t* subject = state.calibration_state().get_motors_enabled_subject();
        REQUIRE(subject != nullptr);
        REQUIRE(lv_subject_get_int(subject) == 1);
    }
}

// ============================================================================
// Firmware Retraction Update Tests - Verify JSON parsing and unit conversion
// ============================================================================

TEST_CASE("Calibration characterization: firmware retraction updates from JSON",
          "[characterization][calibration][retraction]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("retract_length converts mm to centimillimeters (x100)") {
        json status = {{"firmware_retraction",
                        {{"retract_length", 0.8},
                         {"retract_speed", 35},
                         {"unretract_extra_length", 0.05},
                         {"unretract_speed", 25}}}};
        state.update_from_status(status);

        lv_subject_t* subject = get_subject_by_name("retract_length");
        // 0.8mm * 100 = 80 centimillimeters
        REQUIRE(lv_subject_get_int(subject) == 80);
    }

    SECTION("retract_speed stored as integer mm/s") {
        json status = {{"firmware_retraction", {{"retract_speed", 35}}}};
        state.update_from_status(status);

        lv_subject_t* subject = get_subject_by_name("retract_speed");
        REQUIRE(lv_subject_get_int(subject) == 35);
    }

    SECTION("unretract_extra_length converts mm to centimillimeters (x100)") {
        json status = {{"firmware_retraction", {{"unretract_extra_length", 0.05}}}};
        state.update_from_status(status);

        lv_subject_t* subject = get_subject_by_name("unretract_extra_length");
        // 0.05mm * 100 = 5 centimillimeters
        REQUIRE(lv_subject_get_int(subject) == 5);
    }

    SECTION("unretract_speed stored as integer mm/s") {
        json status = {{"firmware_retraction", {{"unretract_speed", 25}}}};
        state.update_from_status(status);

        lv_subject_t* subject = get_subject_by_name("unretract_speed");
        REQUIRE(lv_subject_get_int(subject) == 25);
    }

    SECTION("various retraction lengths convert correctly") {
        // Test with 1.0mm
        json status1 = {{"firmware_retraction", {{"retract_length", 1.0}}}};
        state.update_from_status(status1);
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 100);

        // Test with 0.5mm
        json status2 = {{"firmware_retraction", {{"retract_length", 0.5}}}};
        state.update_from_status(status2);
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 50);

        // Test with 0.0mm (disabled)
        json status3 = {{"firmware_retraction", {{"retract_length", 0.0}}}};
        state.update_from_status(status3);
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 0);
    }

    SECTION("missing firmware_retraction key leaves values unchanged") {
        // First set a known value
        json initial = {{"firmware_retraction", {{"retract_length", 0.8}}}};
        state.update_from_status(initial);
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 80);

        // Update with status that doesn't contain firmware_retraction
        json empty = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
        state.update_from_status(empty);

        // Value should remain unchanged
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 80);
    }
}

// ============================================================================
// Manual Probe Update Tests - Verify is_active and z_position parsing
// ============================================================================

TEST_CASE("Calibration characterization: manual probe updates from JSON",
          "[characterization][calibration][manual_probe]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("manual_probe.is_active true sets subject to 1") {
        json status = {{"manual_probe", {{"is_active", true}, {"z_position", 0.125}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
    }

    SECTION("manual_probe.is_active false sets subject to 0") {
        // First activate
        json activate = {{"manual_probe", {{"is_active", true}}}};
        state.update_from_status(activate);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);

        // Then deactivate
        json deactivate = {{"manual_probe", {{"is_active", false}}}};
        state.update_from_status(deactivate);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                0);
    }

    SECTION("manual_probe.z_position converts mm to microns (x1000)") {
        json status = {{"manual_probe", {{"is_active", true}, {"z_position", 0.125}}}};
        state.update_from_status(status);

        // 0.125mm * 1000 = 125 microns
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 125);
    }

    SECTION("various Z positions convert correctly") {
        json status1 = {{"manual_probe", {{"z_position", 0.5}}}};
        state.update_from_status(status1);
        // 0.5mm * 1000 = 500 microns
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 500);

        json status2 = {{"manual_probe", {{"z_position", 0.025}}}};
        state.update_from_status(status2);
        // 0.025mm * 1000 = 25 microns
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 25);

        json status3 = {{"manual_probe", {{"z_position", 1.234}}}};
        state.update_from_status(status3);
        // 1.234mm * 1000 = 1234 microns
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 1234);
    }

    SECTION("negative Z positions convert correctly") {
        json status = {{"manual_probe", {{"z_position", -0.05}}}};
        state.update_from_status(status);

        // -0.05mm * 1000 = -50 microns
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == -50);
    }

    SECTION("missing manual_probe key leaves values unchanged") {
        // First set a known value
        json initial = {{"manual_probe", {{"is_active", true}, {"z_position", 0.5}}}};
        state.update_from_status(initial);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 500);

        // Update with unrelated status
        json unrelated = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
        state.update_from_status(unrelated);

        // Values should remain unchanged
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 500);
    }
}

// ============================================================================
// Motor State Update Tests - Verify stepper_enable.steppers parsing
// ============================================================================

TEST_CASE("Calibration characterization: motor state updates from JSON",
          "[characterization][calibration][motors]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("all steppers enabled sets motors_enabled to 1") {
        json status = {{"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", true},
                           {"stepper_y", true},
                           {"stepper_z", true},
                           {"extruder", true}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("any stepper enabled sets motors_enabled to 1") {
        // Only X stepper enabled - should still count as motors enabled
        json status = {{"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", true},
                           {"stepper_y", false},
                           {"stepper_z", false},
                           {"extruder", false}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("all steppers disabled sets motors_enabled to 0") {
        json status = {{"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", false},
                           {"stepper_y", false},
                           {"stepper_z", false},
                           {"extruder", false}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("transition from enabled to disabled (M84 simulation)") {
        // Start with motors enabled
        json enabled = {{"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(enabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        // M84 disables all steppers
        json disabled = {{"stepper_enable",
                          {{"steppers",
                            {{"stepper_x", false},
                             {"stepper_y", false},
                             {"stepper_z", false},
                             {"extruder", false}}}}}};
        state.update_from_status(disabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("transition from disabled to enabled (homing simulation)") {
        // Start with motors disabled
        json disabled = {{"stepper_enable",
                          {{"steppers",
                            {{"stepper_x", false},
                             {"stepper_y", false},
                             {"stepper_z", false},
                             {"extruder", false}}}}}};
        state.update_from_status(disabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

        // Homing re-enables steppers
        json enabled = {{"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(enabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("missing stepper_enable key leaves motors_enabled unchanged") {
        // Disable motors first
        json disabled = {{"stepper_enable",
                          {{"steppers",
                            {{"stepper_x", false},
                             {"stepper_y", false},
                             {"stepper_z", false},
                             {"extruder", false}}}}}};
        state.update_from_status(disabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

        // Update with unrelated status
        json unrelated = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
        state.update_from_status(unrelated);

        // Should remain disabled
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("idle_timeout does not affect motors_enabled") {
        // Verify idle_timeout is independent of motor state
        // First set motors enabled via stepper_enable
        json enabled = {{"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(enabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        // idle_timeout "Idle" should NOT change motors_enabled
        // (idle_timeout is for activity state, not motor state)
        json idle = {{"idle_timeout", {{"state", "Idle"}}}};
        state.update_from_status(idle);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }
}

// ============================================================================
// Combined Status Update Tests - Multiple sections in one JSON
// ============================================================================

TEST_CASE("Calibration characterization: combined status updates",
          "[characterization][calibration][combined]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("all calibration sections update in single status message") {
        json status = {{"firmware_retraction",
                        {{"retract_length", 0.8},
                         {"retract_speed", 40},
                         {"unretract_extra_length", 0.1},
                         {"unretract_speed", 30}}},
                       {"manual_probe", {{"is_active", true}, {"z_position", 0.25}}},
                       {"idle_timeout", {{"state", "Ready"}}}};
        state.update_from_status(status);

        // Verify all firmware retraction values
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 80);
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_speed")) == 40);
        REQUIRE(lv_subject_get_int(get_subject_by_name("unretract_extra_length")) == 10);
        REQUIRE(lv_subject_get_int(get_subject_by_name("unretract_speed")) == 30);

        // Verify manual probe values
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 250);

        // Verify motor state
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("partial updates only affect specified sections") {
        // Set initial values
        json initial = {{"firmware_retraction", {{"retract_length", 0.5}}},
                        {"manual_probe", {{"is_active", true}}},
                        {"idle_timeout", {{"state", "Ready"}}}};
        state.update_from_status(initial);

        // Update only firmware_retraction
        json partial = {{"firmware_retraction", {{"retract_length", 1.0}}}};
        state.update_from_status(partial);

        // Only retract_length should change
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 100);
        // Others should remain unchanged
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }
}

// ============================================================================
// Observer Notification Tests - Verify observers fire on calibration changes
// ============================================================================

TEST_CASE("Calibration characterization: observer fires when manual_probe_active changes",
          "[characterization][calibration][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer = lv_subject_add_observer(
        state.calibration_state().get_manual_probe_active_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Initial value is 0

    // Activate manual probe
    json status = {{"manual_probe", {{"is_active", true}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 1);

    lv_observer_remove(observer);
}

TEST_CASE("Calibration characterization: observer fires when motors_enabled changes",
          "[characterization][calibration][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer = lv_subject_add_observer(
        state.calibration_state().get_motors_enabled_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 1); // Default is enabled

    // Disable motors (using stepper_enable - all steppers disabled)
    json status = {{"stepper_enable",
                    {{"steppers",
                      {{"stepper_x", false},
                       {"stepper_y", false},
                       {"stepper_z", false},
                       {"extruder", false}}}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 0);

    lv_observer_remove(observer);
}

// ============================================================================
// Reset Cycle Tests - Verify subjects survive reset_for_testing cycle
// ============================================================================

TEST_CASE("Calibration characterization: subjects survive reset_for_testing cycle",
          "[characterization][calibration][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // Set calibration values (stepper_enable with all disabled = motors off)
    json status = {{"firmware_retraction", {{"retract_length", 0.8}}},
                   {"manual_probe", {{"is_active", true}, {"z_position", 0.5}}},
                   {"stepper_enable",
                    {{"steppers",
                      {{"stepper_x", false},
                       {"stepper_y", false},
                       {"stepper_z", false},
                       {"extruder", false}}}}}};
    state.update_from_status(status);

    // Verify values were set
    REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 80);
    REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) == 1);
    REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    // After reset, values should be back to defaults
    REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("retract_speed")) == 20);
    REQUIRE(lv_subject_get_int(get_subject_by_name("unretract_extra_length")) == 0);
    REQUIRE(lv_subject_get_int(get_subject_by_name("unretract_speed")) == 10);
    REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_z_position_subject()) ==
            0);
    REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) ==
            1); // Default enabled

    // Subjects should still be functional after reset
    json new_status = {{"firmware_retraction", {{"retract_length", 0.5}}}};
    state.update_from_status(new_status);
    REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 50);
}

// ============================================================================
// Independence Tests - Verify calibration subjects are independent
// ============================================================================

TEST_CASE("Calibration characterization: subjects are independent",
          "[characterization][calibration][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("firmware_retraction update does not affect manual_probe or motors") {
        // Set known values for all (stepper_enable with all disabled = motors off)
        json initial = {{"manual_probe", {{"is_active", true}, {"z_position", 0.5}}},
                        {"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", false},
                            {"stepper_y", false},
                            {"stepper_z", false},
                            {"extruder", false}}}}}};
        state.update_from_status(initial);

        // Update only firmware_retraction
        json fr_only = {{"firmware_retraction", {{"retract_length", 1.0}, {"retract_speed", 50}}}};
        state.update_from_status(fr_only);

        // Firmware retraction should change
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 100);
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_speed")) == 50);

        // Manual probe and motors should be unchanged
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 500);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("manual_probe update does not affect firmware_retraction or motors") {
        // Set known values (stepper_enable with all enabled = motors on)
        json initial = {{"firmware_retraction", {{"retract_length", 0.8}}},
                        {"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(initial);

        // Update only manual_probe
        json mp_only = {{"manual_probe", {{"is_active", true}, {"z_position", 0.25}}}};
        state.update_from_status(mp_only);

        // Manual probe should change
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
        REQUIRE(lv_subject_get_int(
                    state.calibration_state().get_manual_probe_z_position_subject()) == 250);

        // Firmware retraction and motors should be unchanged
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 80);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("stepper_enable update does not affect firmware_retraction or manual_probe") {
        // Set known values
        json initial = {{"firmware_retraction", {{"retract_length", 0.6}}},
                        {"manual_probe", {{"is_active", true}}}};
        state.update_from_status(initial);

        // Update only stepper_enable (all disabled = motors off)
        json stepper_only = {{"stepper_enable",
                              {{"steppers",
                                {{"stepper_x", false},
                                 {"stepper_y", false},
                                 {"stepper_z", false},
                                 {"extruder", false}}}}}};
        state.update_from_status(stepper_only);

        // Motors should change
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

        // Others should be unchanged
        REQUIRE(lv_subject_get_int(get_subject_by_name("retract_length")) == 60);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_manual_probe_active_subject()) ==
                1);
    }

    SECTION("idle_timeout does NOT affect motors_enabled (stepper_enable is the source)") {
        // Motors start enabled by default
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        // idle_timeout "Idle" state should NOT disable motors
        // (idle_timeout is for activity state, not motor driver state)
        json idle_status = {{"idle_timeout", {{"state", "Idle"}}}};
        state.update_from_status(idle_status);

        // Motors should still be enabled (not affected by idle_timeout)
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        // Now actually disable motors via stepper_enable
        json stepper_status = {{"stepper_enable",
                                {{"steppers",
                                  {{"stepper_x", false},
                                   {"stepper_y", false},
                                   {"stepper_z", false},
                                   {"extruder", false}}}}}};
        state.update_from_status(stepper_status);

        // NOW motors should be disabled
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }
}

// ============================================================================
// Stepper Enable State Tests - Comprehensive coverage of motor state
// ============================================================================

TEST_CASE("Calibration characterization: stepper_enable motor state tracking",
          "[characterization][calibration][stepper_enable]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("all steppers enabled = motors_enabled is 1") {
        json status = {{"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", true},
                           {"stepper_y", true},
                           {"stepper_z", true},
                           {"extruder", true}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("any single stepper enabled = motors_enabled is 1") {
        // Only extruder enabled (e.g., during filament change)
        json status = {{"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", false},
                           {"stepper_y", false},
                           {"stepper_z", false},
                           {"extruder", true}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("all steppers disabled = motors_enabled is 0") {
        json status = {{"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", false},
                           {"stepper_y", false},
                           {"stepper_z", false},
                           {"extruder", false}}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("M84 simulation: transition from enabled to disabled") {
        // Start with motors enabled (e.g., after homing)
        json enabled = {{"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(enabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        // User presses "Motors Off" button (sends M84)
        // Klipper immediately updates stepper_enable
        json disabled = {{"stepper_enable",
                          {{"steppers",
                            {{"stepper_x", false},
                             {"stepper_y", false},
                             {"stepper_z", false},
                             {"extruder", false}}}}}};
        state.update_from_status(disabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("homing simulation: transition from disabled to enabled") {
        // Start with motors disabled
        json disabled = {{"stepper_enable",
                          {{"steppers",
                            {{"stepper_x", false},
                             {"stepper_y", false},
                             {"stepper_z", false},
                             {"extruder", false}}}}}};
        state.update_from_status(disabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

        // User initiates homing (G28), steppers get enabled
        json enabled = {{"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(enabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("missing stepper_enable key leaves motors_enabled unchanged") {
        // Set known state first
        json enabled = {{"stepper_enable",
                         {{"steppers",
                           {{"stepper_x", true},
                            {"stepper_y", true},
                            {"stepper_z", true},
                            {"extruder", true}}}}}};
        state.update_from_status(enabled);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        // Update with unrelated status (no stepper_enable)
        json other_status = {{"print_stats", {{"state", "printing"}}}};
        state.update_from_status(other_status);

        // Motors should still be enabled (not affected)
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }
}

// ============================================================================
// Homed Axes Motor State Tests - Primary motor state indicator
// M84 clears homed_axes on all Klipper printers, making it the most reliable
// indicator. stepper_enable.steppers is not reported by all firmware (e.g. AD5M).
// ============================================================================

TEST_CASE("Calibration characterization: homed_axes motor state tracking",
          "[characterization][calibration][homed_axes]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("homed_axes 'xyz' sets motors_enabled to 1") {
        json status = {{"toolhead", {{"homed_axes", "xyz"}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("homed_axes empty string sets motors_enabled to 0") {
        json status = {{"toolhead", {{"homed_axes", ""}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("partial homed_axes still means motors enabled") {
        json status = {{"toolhead", {{"homed_axes", "xy"}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("M84 simulation via homed_axes: enabled to disabled") {
        json homed = {{"toolhead", {{"homed_axes", "xyz"}}}};
        state.update_from_status(homed);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);

        json m84 = {{"toolhead", {{"homed_axes", ""}}}};
        state.update_from_status(m84);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("homing after M84: disabled to enabled") {
        json m84 = {{"toolhead", {{"homed_axes", ""}}}};
        state.update_from_status(m84);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

        json homed = {{"toolhead", {{"homed_axes", "xyz"}}}};
        state.update_from_status(homed);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 1);
    }

    SECTION("toolhead without homed_axes does not affect motors_enabled") {
        json m84 = {{"toolhead", {{"homed_axes", ""}}}};
        state.update_from_status(m84);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);

        json position_only = {{"toolhead", {{"position", {100.0, 200.0, 30.0, 0.0}}}}};
        state.update_from_status(position_only);
        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }

    SECTION("homed_axes takes priority over empty stepper_enable") {
        // AD5M scenario: stepper_enable is empty but homed_axes works
        json status = {{"toolhead", {{"homed_axes", ""}}}, {"stepper_enable", json::object()}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.calibration_state().get_motors_enabled_subject()) == 0);
    }
}