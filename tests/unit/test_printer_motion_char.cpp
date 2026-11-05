// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_motion_char.cpp
 * @brief Characterization tests for PrinterState motion domain
 *
 * These tests capture the CURRENT behavior of motion-related subjects
 * in PrinterState before extraction to a dedicated PrinterMotionState class.
 *
 * Motion subjects (8 total):
 * - position_x_ (int, centimm - 150.5mm stored as 15050)
 * - position_y_ (int, centimm - 200.3mm stored as 20030)
 * - position_z_ (int, centimm - 10.7mm stored as 1070)
 * - homed_axes_ (string, e.g., "xyz", "xy", "")
 * - speed_factor_ (int, percent - 1.5 stored as 150%)
 * - flow_factor_ (int, percent - 0.95 stored as 95%)
 * - gcode_z_offset_ (int, microns - -0.15mm stored as -150)
 * - pending_z_offset_delta_ (int, microns - user-set accumulator)
 *
 * Position format: Integer centimillimeters (mm * 100)
 * Factor format: value * 100 for percentage (divide by 100 for 0.0-1.0 range)
 * Offset format: value * 1000 for microns (divide by 1000 for mm)
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
// ============================================================================
// Initial State Tests - Document non-obvious default initialization
// ============================================================================

TEST_CASE("Motion characterization: non-obvious initial values after init",
          "[characterization][motion][init]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("speed_factor initializes to 100%") {
        REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 100);
    }

    SECTION("flow_factor initializes to 100%") {
        REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 100);
    }
}

// ============================================================================
// Position Update Tests - Verify toolhead position parsing
// ============================================================================

TEST_CASE("Motion characterization: position updates from JSON",
          "[characterization][motion][position]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("positions store as centimillimeters with 0.01mm precision") {
        json status = {{"toolhead", {{"position", {150.5, 200.3, 10.7}}}}};
        state.update_from_status(status);

        // Positions stored as centimillimeters (mm × 100)
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 15050);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 20030);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 1070);
    }

    SECTION("whole positions store correctly") {
        json status = {{"toolhead", {{"position", {100.0, 200.0, 50.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 20000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 5000);
    }

    SECTION("zero positions store correctly") {
        json status = {{"toolhead", {{"position", {0.0, 0.0, 0.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 0);
    }

    SECTION("large positions store correctly") {
        json status = {{"toolhead", {{"position", {350.0, 350.0, 400.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 35000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 35000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 40000);
    }

    SECTION("negative positions store correctly") {
        // Note: Klipper can report negative positions in some configs
        json status = {{"toolhead", {{"position", {-10.5, -5.2, 0.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == -1050);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == -520);
        REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 0);
    }
}

// ============================================================================
// Homed Axes Update Tests - Verify homing state parsing
// ============================================================================

TEST_CASE("Motion characterization: homed_axes updates from JSON",
          "[characterization][motion][homing]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("empty homed_axes (nothing homed)") {
        json status = {{"toolhead", {{"homed_axes", ""}}}};
        state.update_from_status(status);

        const char* axes = lv_subject_get_string(state.motion_state().get_homed_axes_subject());
        REQUIRE(std::string(axes) == "");
    }

    SECTION("x only homed") {
        json status = {{"toolhead", {{"homed_axes", "x"}}}};
        state.update_from_status(status);

        const char* axes = lv_subject_get_string(state.motion_state().get_homed_axes_subject());
        REQUIRE(std::string(axes) == "x");
    }

    SECTION("xy homed") {
        json status = {{"toolhead", {{"homed_axes", "xy"}}}};
        state.update_from_status(status);

        const char* axes = lv_subject_get_string(state.motion_state().get_homed_axes_subject());
        REQUIRE(std::string(axes) == "xy");
    }

    SECTION("xyz fully homed") {
        json status = {{"toolhead", {{"homed_axes", "xyz"}}}};
        state.update_from_status(status);

        const char* axes = lv_subject_get_string(state.motion_state().get_homed_axes_subject());
        REQUIRE(std::string(axes) == "xyz");
    }
}

// ============================================================================
// Speed/Flow Factor Update Tests - Verify percentage conversion
// ============================================================================

TEST_CASE("Motion characterization: speed_factor updates from JSON",
          "[characterization][motion][speed]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("normal speed factor (100%)") {
        json status = {{"gcode_move", {{"speed_factor", 1.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 100);
    }

    SECTION("increased speed factor (150%)") {
        json status = {{"gcode_move", {{"speed_factor", 1.5}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 150);
    }

    SECTION("decreased speed factor (50%)") {
        json status = {{"gcode_move", {{"speed_factor", 0.5}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 50);
    }

    SECTION("maximum speed factor (200%)") {
        json status = {{"gcode_move", {{"speed_factor", 2.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 200);
    }
}

TEST_CASE("Motion characterization: live_velocity updates from motion_report",
          "[characterization][motion][speed][tune_units]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("rounds the measured toolhead speed to whole mm/s") {
        json status = {{"motion_report", {{"live_velocity", 148.6}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_live_velocity_subject()) == 149);
    }

    SECTION("falls back to zero when the toolhead stops") {
        state.update_from_status({{"motion_report", {{"live_velocity", 120.0}}}});
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_velocity_subject()) == 120);

        state.update_from_status({{"motion_report", {{"live_velocity", 0.0}}}});
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_velocity_subject()) == 0);
    }
}

TEST_CASE("Motion characterization: live_position updates from motion_report",
          "[characterization][motion][position]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("stores all three axes as centimillimeters") {
        json status = {{"motion_report", {{"live_position", {150.5, 200.3, 10.7}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_x_subject()) == 15050);
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_y_subject()) == 20030);
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_z_subject()) == 1070);
    }

    SECTION("a malformed array leaves the subjects unchanged") {
        state.update_from_status(
            {{"motion_report", {{"live_position", {12.0, 34.0}}}}}); // too short
        state.update_from_status(
            {{"motion_report", {{"live_position", {nullptr, 34.0, 56.0}}}}}); // null element

        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_x_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_y_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_z_subject()) == 0);
    }

    SECTION("a frame without live_position leaves the subjects unchanged") {
        state.update_from_status({{"motion_report", {{"live_position", {12.0, 34.0, 56.0}}}}});
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_x_subject()) == 1200);

        // Delta frames omit unchanged fields; the next frame carries only
        // velocity and must not blank what the last one set.
        state.update_from_status({{"motion_report", {{"live_velocity", 42.0}}}});
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_x_subject()) == 1200);
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_y_subject()) == 3400);
        REQUIRE(lv_subject_get_int(state.motion_state().get_live_position_z_subject()) == 5600);
    }
}

TEST_CASE("Motion characterization: flow_factor updates from JSON",
          "[characterization][motion][flow]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("normal flow factor (100%)") {
        json status = {{"gcode_move", {{"extrude_factor", 1.0}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 100);
    }

    SECTION("decreased flow factor (95%)") {
        json status = {{"gcode_move", {{"extrude_factor", 0.95}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 95);
    }

    SECTION("increased flow factor (110%)") {
        json status = {{"gcode_move", {{"extrude_factor", 1.1}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 110);
    }
}

// ============================================================================
// Z-Offset Update Tests - Verify micron conversion
// ============================================================================

TEST_CASE("Motion characterization: gcode_z_offset updates from JSON",
          "[characterization][motion][zoffset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("zero Z-offset") {
        json status = {{"gcode_move", {{"homing_origin", {0.0, 0.0, 0.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == 0);
    }

    SECTION("negative Z-offset (-0.15mm = -150 microns)") {
        json status = {{"gcode_move", {{"homing_origin", {0.0, 0.0, -0.15}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == -150);
    }

    SECTION("positive Z-offset (0.2mm = 200 microns)") {
        json status = {{"gcode_move", {{"homing_origin", {0.0, 0.0, 0.2}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == 200);
    }

    SECTION("small Z-offset (0.025mm = 25 microns)") {
        json status = {{"gcode_move", {{"homing_origin", {0.0, 0.0, 0.025}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == 25);
    }
}

// ============================================================================
// Pending Z-Offset Delta Tests - Verify accumulation methods
// ============================================================================

TEST_CASE("Motion characterization: pending_z_offset_delta methods",
          "[characterization][motion][pending_z]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("initial state has no pending adjustment") {
        REQUIRE(state.motion_state().get_pending_z_offset_delta() == 0);
        REQUIRE(state.motion_state().has_pending_z_offset_adjustment() == false);
    }

    SECTION("add_pending_z_offset_delta accumulates values") {
        state.motion_state().add_pending_z_offset_delta(10);
        REQUIRE(state.motion_state().get_pending_z_offset_delta() == 10);
        REQUIRE(state.motion_state().has_pending_z_offset_adjustment() == true);

        state.motion_state().add_pending_z_offset_delta(15);
        REQUIRE(state.motion_state().get_pending_z_offset_delta() == 25);
    }

    SECTION("negative deltas subtract from total") {
        state.motion_state().add_pending_z_offset_delta(50);
        REQUIRE(state.motion_state().get_pending_z_offset_delta() == 50);

        state.motion_state().add_pending_z_offset_delta(-20);
        REQUIRE(state.motion_state().get_pending_z_offset_delta() == 30);
    }

    SECTION("clear_pending_z_offset_delta resets to zero") {
        state.motion_state().add_pending_z_offset_delta(100);
        REQUIRE(state.motion_state().has_pending_z_offset_adjustment() == true);

        state.motion_state().clear_pending_z_offset_delta();
        REQUIRE(state.motion_state().get_pending_z_offset_delta() == 0);
        REQUIRE(state.motion_state().has_pending_z_offset_adjustment() == false);
    }

    SECTION("subject reflects pending delta changes") {
        REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 0);

        state.motion_state().add_pending_z_offset_delta(75);
        REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) ==
                75);

        state.motion_state().clear_pending_z_offset_delta();
        REQUIRE(lv_subject_get_int(state.motion_state().get_pending_z_offset_delta_subject()) == 0);
    }
}

// ============================================================================
// Observer Notification Tests - Verify observers fire on motion changes
// ============================================================================

TEST_CASE("Motion characterization: observer fires when position_x changes",
          "[characterization][motion][observer]") {
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

    lv_observer_t* observer = lv_subject_add_observer(state.motion_state().get_position_x_subject(),
                                                      observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Initial value is 0

    // Update position via status update
    json status = {{"toolhead", {{"position", {150.5, 200.0, 10.0}}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);     // At least one more notification
    REQUIRE(user_data[1] == 15050); // 150.5mm in centimm

    lv_observer_remove(observer);
}

TEST_CASE("Motion characterization: observer fires when homed_axes changes",
          "[characterization][motion][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    int callback_count = 0;
    std::string last_value;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        auto* data =
            static_cast<std::pair<int*, std::string*>*>(lv_observer_get_user_data(observer));
        (*data->first)++;
        *data->second = lv_subject_get_string(subject);
    };

    auto user_data = std::make_pair(&callback_count, &last_value);

    lv_observer_t* observer = lv_subject_add_observer(state.motion_state().get_homed_axes_subject(),
                                                      observer_cb, &user_data);

    // Initial notification
    REQUIRE(callback_count == 1);
    REQUIRE(last_value == "");

    // Update homed_axes
    json status = {{"toolhead", {{"homed_axes", "xyz"}}}};
    state.update_from_status(status);

    REQUIRE(callback_count >= 2);
    REQUIRE(last_value == "xyz");

    lv_observer_remove(observer);
}

TEST_CASE("Motion characterization: observer fires when speed_factor changes",
          "[characterization][motion][observer]") {
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
        state.motion_state().get_speed_factor_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 100); // Initial value is 100%

    // Update speed factor
    json status = {{"gcode_move", {{"speed_factor", 1.5}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);
    REQUIRE(user_data[1] == 150);

    lv_observer_remove(observer);
}

// ============================================================================
// Independence Tests - Verify motion subjects are independent
// ============================================================================

TEST_CASE("Motion characterization: toolhead update does not affect gcode_move subjects",
          "[characterization][motion][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set initial gcode_move values
    json initial = {
        {"gcode_move",
         {{"speed_factor", 1.5}, {"extrude_factor", 0.95}, {"homing_origin", {0.0, 0.0, -0.1}}}}};
    state.update_from_status(initial);

    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 150);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 95);
    REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == -100);

    // Update only toolhead
    json toolhead_only = {
        {"toolhead", {{"position", {100.0, 200.0, 50.0}}, {"homed_axes", "xyz"}}}};
    state.update_from_status(toolhead_only);

    // Positions should update (stored in centimm)
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 20000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 5000);
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "xyz");

    // gcode_move subjects should be unchanged
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 150);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 95);
    REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == -100);
}

TEST_CASE("Motion characterization: gcode_move update does not affect toolhead subjects",
          "[characterization][motion][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set initial toolhead values
    json initial = {{"toolhead", {{"position", {150.0, 200.0, 30.0}}, {"homed_axes", "xy"}}}};
    state.update_from_status(initial);

    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 15000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 20000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 3000);
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "xy");

    // Update only gcode_move
    json gcode_only = {{"gcode_move", {{"speed_factor", 0.75}, {"extrude_factor", 1.1}}}};
    state.update_from_status(gcode_only);

    // gcode_move should update
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 75);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 110);

    // toolhead subjects should be unchanged (stored in centimm)
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 15000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 20000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 3000);
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "xy");
}

TEST_CASE("Motion characterization: simultaneous updates work correctly",
          "[characterization][motion][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Update all motion values in a single status message
    json status = {
        {"toolhead", {{"position", {120.5, 180.3, 25.7}}, {"homed_axes", "xyz"}}},
        {"gcode_move",
         {{"speed_factor", 1.25}, {"extrude_factor", 0.98}, {"homing_origin", {0.0, 0.0, -0.05}}}}};
    state.update_from_status(status);

    // All values should be updated independently (positions in centimm)
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 12050);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 18030);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 2570);
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "xyz");
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 125);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 98);
    REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == -50);
}

// ============================================================================
// Reset Cycle Tests - Verify subjects survive reset_for_testing cycle
// ============================================================================

TEST_CASE("Motion characterization: subjects survive reset_for_testing cycle",
          "[characterization][motion][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set some motion values
    json status = {
        {"toolhead", {{"position", {100.0, 150.0, 20.0}}, {"homed_axes", "xyz"}}},
        {"gcode_move",
         {{"speed_factor", 1.2}, {"extrude_factor", 0.9}, {"homing_origin", {0.0, 0.0, 0.1}}}}};
    state.update_from_status(status);
    state.motion_state().add_pending_z_offset_delta(50);

    // Verify values were set (position in centimm)
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 120);
    REQUIRE(state.motion_state().get_pending_z_offset_delta() == 50);

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // After reset, values should be back to defaults
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 0);
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "");
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 100);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 100);
    REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == 0);
    REQUIRE(state.motion_state().get_pending_z_offset_delta() == 0);

    // Subjects should still be functional after reset
    json new_status = {{"toolhead", {{"position", {50.0, 75.0, 10.0}}}}};
    state.update_from_status(new_status);

    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 5000); // centimm
}

TEST_CASE("Motion characterization: subject pointers remain valid after reset",
          "[characterization][motion][reset]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Capture subject pointers
    lv_subject_t* position_x_before = state.motion_state().get_position_x_subject();
    lv_subject_t* speed_factor_before = state.motion_state().get_speed_factor_subject();
    lv_subject_t* homed_axes_before = state.motion_state().get_homed_axes_subject();

    // Reset and reinitialize
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Pointers should be the same (singleton subjects are reused)
    lv_subject_t* position_x_after = state.motion_state().get_position_x_subject();
    lv_subject_t* speed_factor_after = state.motion_state().get_speed_factor_subject();
    lv_subject_t* homed_axes_after = state.motion_state().get_homed_axes_subject();

    REQUIRE(position_x_before == position_x_after);
    REQUIRE(speed_factor_before == speed_factor_after);
    REQUIRE(homed_axes_before == homed_axes_after);
}

// ============================================================================
// Partial Update Tests - Document behavior with incomplete data
// ============================================================================

TEST_CASE("Motion characterization: partial status updates preserve other values",
          "[characterization][motion][partial]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set initial values
    json initial = {
        {"toolhead", {{"position", {100.0, 200.0, 30.0}}, {"homed_axes", "xyz"}}},
        {"gcode_move",
         {{"speed_factor", 1.5}, {"extrude_factor", 0.95}, {"homing_origin", {0.0, 0.0, -0.1}}}}};
    state.update_from_status(initial);

    // Verify initial values (position in centimm)
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 150);

    // Update only position - other values should not change
    json partial = {{"toolhead", {{"position", {150.0, 250.0, 40.0}}}}};
    state.update_from_status(partial);

    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 15000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 25000);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 4000);
    // These should be unchanged:
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "xyz");
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 150);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 95);
    REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_z_offset_subject()) == -100);
}

TEST_CASE("Motion characterization: empty status does not affect values",
          "[characterization][motion][partial]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Set initial values
    json initial = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
    state.update_from_status(initial);

    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000); // centimm

    // Empty status should not change anything
    json empty = json::object();
    state.update_from_status(empty);

    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 10000); // centimm
}

// ============================================================================
// Observer Independence Tests - Verify observer isolation
// ============================================================================

TEST_CASE("Motion characterization: observers on different subjects are independent",
          "[characterization][motion][observer][independence]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    int position_count = 0;
    int speed_count = 0;

    auto position_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    auto speed_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* position_observer = lv_subject_add_observer(
        state.motion_state().get_position_x_subject(), position_cb, &position_count);
    lv_observer_t* speed_observer = lv_subject_add_observer(
        state.motion_state().get_speed_factor_subject(), speed_cb, &speed_count);

    // Both observers fire on initial add
    REQUIRE(position_count == 1);
    REQUIRE(speed_count == 1);

    // Update only position
    json status = {{"toolhead", {{"position", {100.0, 200.0, 30.0}}}}};
    state.update_from_status(status);

    // Only position observer should fire
    REQUIRE(position_count >= 2);
    REQUIRE(speed_count == 1);

    // Update only speed factor
    status = {{"gcode_move", {{"speed_factor", 1.5}}}};
    state.update_from_status(status);

    // Only speed observer should fire
    int position_count_after_first = position_count;
    REQUIRE(speed_count >= 2);
    REQUIRE(position_count == position_count_after_first);

    lv_observer_remove(position_observer);
    lv_observer_remove(speed_observer);
}

TEST_CASE("Motion characterization: multiple observers on same subject all fire",
          "[characterization][motion][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    int count1 = 0, count2 = 0, count3 = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer1 = lv_subject_add_observer(
        state.motion_state().get_position_x_subject(), observer_cb, &count1);
    lv_observer_t* observer2 = lv_subject_add_observer(
        state.motion_state().get_position_x_subject(), observer_cb, &count2);
    lv_observer_t* observer3 = lv_subject_add_observer(
        state.motion_state().get_position_x_subject(), observer_cb, &count3);

    // All observers fire on initial add
    REQUIRE(count1 == 1);
    REQUIRE(count2 == 1);
    REQUIRE(count3 == 1);

    // Single update should fire all three
    json status = {{"toolhead", {{"position", {150.0, 200.0, 30.0}}}}};
    state.update_from_status(status);

    REQUIRE(count1 >= 2);
    REQUIRE(count2 >= 2);
    REQUIRE(count3 >= 2);

    lv_observer_remove(observer1);
    lv_observer_remove(observer2);
    lv_observer_remove(observer3);
}

// ============================================================================
// Gcode Position Tests - Verify gcode_position_x/y/z subjects read from
// gcode_move["gcode_position"] (commanded position), NOT gcode_move["position"]
// ============================================================================

TEST_CASE("Motion characterization: gcode_position updates from gcode_move.gcode_position",
          "[characterization][motion][gcode_position]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    SECTION("gcode positions store as centimillimeters from gcode_move.gcode_position") {
        // This tests that we read from gcode_position, NOT position
        json status = {{"gcode_move", {{"gcode_position", {150.5, 200.3, 10.7}}}}};
        state.update_from_status(status);

        // Values should be stored as centimillimeters (mm * 100)
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == 15050);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == 20030);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == 1070);
    }

    SECTION("zero gcode positions store correctly") {
        json status = {{"gcode_move", {{"gcode_position", {0.0, 0.0, 0.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == 0);
    }

    SECTION("negative gcode positions store correctly") {
        // Klipper can report negative positions in some configurations
        json status = {{"gcode_move", {{"gcode_position", {-10.5, -5.2, -0.15}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == -1050);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == -520);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == -15);
    }

    SECTION("large gcode positions store correctly") {
        json status = {{"gcode_move", {{"gcode_position", {350.0, 350.0, 400.0}}}}};
        state.update_from_status(status);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == 35000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == 35000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == 40000);
    }
}

TEST_CASE("Motion characterization: gcode_position vs position are independent",
          "[characterization][motion][gcode_position]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // This is the critical test: gcode_move contains BOTH position and gcode_position
    // - position: raw commanded position (before offset adjustments)
    // - gcode_position: effective commanded position (what gcode coordinates actually mean)
    // The UI should use gcode_position for display

    SECTION("gcode_position reads from gcode_position key, not position key") {
        // Set different values for position and gcode_position
        // When there's a z-offset, these values will differ
        json status = {{"gcode_move",
                        {
                            {"position", {100.0, 100.0, 10.0}},      // Raw commanded
                            {"gcode_position", {150.5, 200.3, 10.7}} // Effective position
                        }}};
        state.update_from_status(status);

        // gcode_position subjects should reflect gcode_position values
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == 15050);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == 20030);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == 1070);
    }

    SECTION("gcode_position unchanged when only position key updates") {
        // First set gcode_position
        json initial = {{"gcode_move", {{"gcode_position", {50.0, 60.0, 5.0}}}}};
        state.update_from_status(initial);

        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == 5000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == 6000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == 500);

        // Update only position (not gcode_position) - should NOT change gcode_position subjects
        json update = {{"gcode_move", {{"position", {999.0, 888.0, 777.0}}}}};
        state.update_from_status(update);

        // gcode_position subjects should be unchanged
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_x_subject()) == 5000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_y_subject()) == 6000);
        REQUIRE(lv_subject_get_int(state.motion_state().get_gcode_position_z_subject()) == 500);
    }
}

TEST_CASE("Motion characterization: gcode_position observer fires on update",
          "[characterization][motion][gcode_position][observer]") {
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
        state.motion_state().get_gcode_position_x_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Initial value is 0

    // Update gcode_position via status update
    json status = {{"gcode_move", {{"gcode_position", {150.5, 200.0, 10.0}}}}};
    state.update_from_status(status);

    REQUIRE(user_data[0] >= 2);     // At least one more notification
    REQUIRE(user_data[1] == 15050); // 150.5mm in centimm

    lv_observer_remove(observer);
}
