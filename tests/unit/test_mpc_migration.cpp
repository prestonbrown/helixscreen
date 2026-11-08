// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mpc_migration.cpp
 * @brief Unit tests for heater control-type migration between PID and MPC
 *
 * Tests:
 * - Which (current, target) pairs require a config migration
 * - The config edits each migration direction produces
 * - A PID -> MPC -> PID round trip through the real config editor
 */

#include "../../include/klipper_config_editor.h"
#include "../../include/ui_panel_calibration_pid.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::system::ConfigEdit;
using helix::system::KlipperConfigEditor;
using ControlType = PIDCalibrationPanel::ControlType;
using CalibMethod = PIDCalibrationPanel::CalibMethod;

namespace {

/// True when `edits` contains a REMOVE_KEY for `key`.
bool removes(const std::vector<ConfigEdit>& edits, const std::string& key) {
    for (const auto& e : edits) {
        if (e.type == ConfigEdit::Type::REMOVE_KEY && e.key == key)
            return true;
    }
    return false;
}

/// The value a SET_VALUE/ADD_KEY edit assigns to `key`, or "" when absent.
std::string value_of(const std::vector<ConfigEdit>& edits, const std::string& key) {
    for (const auto& e : edits) {
        if (e.type != ConfigEdit::Type::REMOVE_KEY && e.key == key)
            return e.value;
    }
    return "";
}

} // namespace

// ============================================================================
// needs_control_migration()
// ============================================================================

TEST_CASE("PID heater selecting MPC needs a migration", "[mpc_migration]") {
    REQUIRE(PIDCalibrationPanel::needs_control_migration(ControlType::PID, CalibMethod::MPC));
}

TEST_CASE("MPC heater selecting PID needs a migration", "[mpc_migration]") {
    REQUIRE(PIDCalibrationPanel::needs_control_migration(ControlType::MPC, CalibMethod::PID));
}

TEST_CASE("Selecting the method already in the config needs no migration", "[mpc_migration]") {
    REQUIRE_FALSE(PIDCalibrationPanel::needs_control_migration(ControlType::PID, CalibMethod::PID));
    REQUIRE_FALSE(PIDCalibrationPanel::needs_control_migration(ControlType::MPC, CalibMethod::MPC));
}

TEST_CASE("An unknown control type never migrates", "[mpc_migration]") {
    // Non-Kalico firmware answers no control-type query, so the panel must not
    // offer to rewrite a config it has not read.
    REQUIRE_FALSE(
        PIDCalibrationPanel::needs_control_migration(ControlType::UNKNOWN, CalibMethod::PID));
    REQUIRE_FALSE(
        PIDCalibrationPanel::needs_control_migration(ControlType::UNKNOWN, CalibMethod::MPC));
}

// ============================================================================
// build_control_migration_edits()
// ============================================================================

TEST_CASE("Migrating to MPC sets control and adds heater_power", "[mpc_migration]") {
    auto edits = PIDCalibrationPanel::build_control_migration_edits(CalibMethod::MPC, 50);

    REQUIRE(value_of(edits, "control") == "mpc");
    REQUIRE(value_of(edits, "heater_power") == "50");
    // SET_VALUE would fail the whole list on a section that names control only
    // in a comment, which is the usual shape once SAVE_CONFIG owns it.
    for (const auto& e : edits) {
        REQUIRE(e.type != ConfigEdit::Type::SET_VALUE);
    }
}

TEST_CASE("Migrating to PID sets control and removes every MPC key", "[mpc_migration]") {
    auto edits = PIDCalibrationPanel::build_control_migration_edits(CalibMethod::PID, 50);

    REQUIRE(value_of(edits, "control") == "pid");

    // heater_power is ours. The other four normally live in Klipper's autosave
    // block, but a hand-written config can carry them in the section itself,
    // where they fail the unused-option check once no MPC object reads them.
    REQUIRE(removes(edits, "heater_power"));
    REQUIRE(removes(edits, "block_heat_capacity"));
    REQUIRE(removes(edits, "sensor_responsiveness"));
    REQUIRE(removes(edits, "ambient_transfer"));
    REQUIRE(removes(edits, "fan_ambient_transfer"));
    REQUIRE(removes(edits, "cooling_fan"));
}

TEST_CASE("Migrating to PID never adds heater_power", "[mpc_migration]") {
    auto edits = PIDCalibrationPanel::build_control_migration_edits(CalibMethod::PID, 50);
    REQUIRE(value_of(edits, "heater_power").empty());
}

TEST_CASE("Migrating to MPC names the part fan unless the section names a fan", "[mpc_migration]") {
    KlipperConfigEditor editor;
    const auto edits =
        PIDCalibrationPanel::build_control_migration_edits(CalibMethod::MPC, 50, "fan");

    auto added = editor.apply_edits("[extruder]\nstep_pin: PA1\n", "extruder", edits);
    REQUIRE(added.has_value());
    CHECK(added->find("\ncooling_fan: fan") != std::string::npos);

    // Kalico reads any fan object; a user's own choice is not ours to replace.
    auto kept =
        editor.apply_edits("[extruder]\ncooling_fan: fan_generic nozzle\n", "extruder", edits);
    REQUIRE(kept.has_value());
    CHECK(kept->find("cooling_fan: fan_generic nozzle") != std::string::npos);
    CHECK(kept->find("cooling_fan: fan\n") == std::string::npos);

    CHECK(value_of(PIDCalibrationPanel::build_control_migration_edits(CalibMethod::MPC, 50),
                   "cooling_fan")
              .empty());
    CHECK(value_of(PIDCalibrationPanel::build_control_migration_edits(CalibMethod::PID, 50, "fan"),
                   "cooling_fan")
              .empty());
}

// ============================================================================
// Round trip through the real editor
// ============================================================================

TEST_CASE("PID -> MPC -> PID round trip leaves no MPC keys active", "[mpc_migration]") {
    KlipperConfigEditor editor;
    const std::string original = "[extruder]\n"
                                 "step_pin: PA1\n"
                                 "control: pid\n"
                                 "pid_kp: 22.865\n"
                                 "pid_ki: 1.292\n"
                                 "pid_kd: 101.178\n";

    // Forward: the panel's migration.
    auto to_mpc = editor.apply_edits(
        original, "extruder",
        PIDCalibrationPanel::build_control_migration_edits(CalibMethod::MPC, 50, "fan"));
    REQUIRE(to_mpc.has_value());
    REQUIRE(to_mpc->find("control: mpc") != std::string::npos);
    REQUIRE(to_mpc->find("heater_power: 50") != std::string::npos);
    REQUIRE(to_mpc->find("\ncooling_fan: fan") != std::string::npos);

    // A hand-written config carrying the thermal model in the section itself.
    auto calibrated = editor.apply_edits(
        *to_mpc, "extruder",
        {
            {ConfigEdit::Type::ADD_KEY, "block_heat_capacity", "22.7"},
            {ConfigEdit::Type::ADD_KEY, "sensor_responsiveness", "0.148"},
            {ConfigEdit::Type::ADD_KEY, "ambient_transfer", "0.148"},
            {ConfigEdit::Type::ADD_KEY, "fan_ambient_transfer", "0.05, 0.1, 0.2"},
        });
    REQUIRE(calibrated.has_value());

    // Reverse: back to PID.
    auto to_pid = editor.apply_edits(
        *calibrated, "extruder",
        PIDCalibrationPanel::build_control_migration_edits(CalibMethod::PID, 50));
    REQUIRE(to_pid.has_value());

    REQUIRE(to_pid->find("control: pid") != std::string::npos);
    REQUIRE(to_pid->find("control: mpc") == std::string::npos);

    // REMOVE_KEY comments keys out rather than deleting the line, so assert on
    // the active form: no MPC key may survive at the start of a line.
    for (const char* key : {"heater_power", "block_heat_capacity", "sensor_responsiveness",
                            "ambient_transfer", "fan_ambient_transfer", "cooling_fan"}) {
        const std::string active = "\n" + std::string(key) + ":";
        INFO("MPC key still active after reverse migration: " << key);
        REQUIRE(to_pid->find(active) == std::string::npos);
    }

    // The PID values the heater needs are untouched.
    REQUIRE(to_pid->find("pid_kp: 22.865") != std::string::npos);
    REQUIRE(to_pid->find("step_pin: PA1") != std::string::npos);
}

// ============================================================================
// Klipper's autosave block
// ============================================================================

TEST_CASE("Migration edits leave the SAVE_CONFIG block untouched", "[mpc_migration]") {
    // Klipper refuses to start with "autosave state corrupted" if a "#*# " line
    // appears above the autosave header, so edits must never reach past it.
    KlipperConfigEditor editor;
    const std::string autosave =
        "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
        "#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.\n"
        "#*#\n"
        "#*# [extruder]\n"
        "#*# control = pid\n"
        "#*# pid_kp = 35.492\n"
        "#*# block_heat_capacity = 8.88445\n";
    const std::string content = "[extruder]\nstep_pin: PA1\ncontrol: pid\n\n" + autosave;

    for (auto target : {CalibMethod::MPC, CalibMethod::PID}) {
        auto result = editor.apply_edits(
            content, "extruder", PIDCalibrationPanel::build_control_migration_edits(target, 50));
        REQUIRE(result.has_value());

        INFO("target = " << (target == CalibMethod::MPC ? "mpc" : "pid"));
        // The block survives verbatim...
        REQUIRE(result->find(autosave) != std::string::npos);
        // ...and nothing was inserted above the header.
        REQUIRE(result->find("#*# ") == result->find(autosave));
    }
}

// ============================================================================
// Sections whose control key lives in the autosave block
// ============================================================================

TEST_CASE("Migration works when the section has no active control key", "[mpc_migration]") {
    // Klipper's SAVE_CONFIG writes `control` into the autosave block, not the
    // section, so a real [extruder] often names it only in a comment. The edits
    // must still take, in both directions.
    KlipperConfigEditor editor;
    const std::string content = "[extruder]\n"
                                "step_pin: PA1\n"
                                "#control: mpc\n"
                                "#heater_power: 70\n"
                                "sensor_type: ATC Semitec 104NT-4-R025H42G\n"
                                "\n"
                                "#*# <---------------------- SAVE_CONFIG ---------------------->\n"
                                "#*# [extruder]\n"
                                "#*# control = pid\n";

    SECTION("to MPC") {
        auto result = editor.apply_edits(
            content, "extruder",
            PIDCalibrationPanel::build_control_migration_edits(CalibMethod::MPC, 50));
        REQUIRE(result.has_value());
        REQUIRE(result->find("\ncontrol: mpc") != std::string::npos);
        REQUIRE(result->find("\nheater_power: 50") != std::string::npos);
    }

    SECTION("to PID") {
        auto result = editor.apply_edits(
            content, "extruder",
            PIDCalibrationPanel::build_control_migration_edits(CalibMethod::PID, 50));
        REQUIRE(result.has_value());
        REQUIRE(result->find("\ncontrol: pid") != std::string::npos);
    }
}
