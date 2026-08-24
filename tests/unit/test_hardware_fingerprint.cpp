// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/// @file test_hardware_fingerprint.cpp
/// @brief Unit tests for helix::compute_hardware_fingerprint.
///
/// The fingerprint gates user-facing side-effects in
/// Application::on_discovery_complete so that a reconnect with unchanged
/// hardware skips LED chip population, hardware validation toasts, the
/// targeted reconfig wizard, and telemetry snapshots (issue #1117 run-up:
/// klippy_state 2→0→READY rebuilding dozens of widgets in one UpdateQueue
/// batch). These tests verify the fingerprint's contract: deterministic for
/// identical hardware, order-independent, sensitive to component / capability
/// / macro / identity changes, and stable across volatile-field changes
/// (build_volume, software_version, etc.) that don't reflect a hardware
/// shape change.

#include "hardware_fingerprint.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Build a PrinterDiscovery with a representative hardware shape.
/// Used as the baseline for "same hardware" comparisons.
PrinterDiscovery make_baseline_discovery() {
    PrinterDiscovery hw;
    // Heaters: extruder + bed + chamber
    nlohmann::json objects = nlohmann::json::array(
        {"extruder", "heater_bed", "heater_generic chamber", "fan", "heater_fan hotend_fan",
         "controller_fan controller_fan", "temperature_sensor chamber_sensor", "neopixel case_led",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro CLEAN_NOZZLE", "bed_mesh", "probe",
         "quad_gantry_level"});
    hw.parse_objects(objects);
    hw.set_hostname("voron-350");
    hw.set_mcu("stm32f446");
    hw.set_kinematics("corexy");
    return hw;
}

} // namespace

TEST_CASE("Hardware fingerprint is deterministic", "[hardware_fingerprint]") {
    auto a = make_baseline_discovery();
    auto b = make_baseline_discovery();
    REQUIRE(compute_hardware_fingerprint(a) == compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint is order-independent", "[hardware_fingerprint]") {
    // Same components, different parse order — Klipper doesn't guarantee
    // object-list iteration order is stable across reconnects. Two parses
    // with shuffled input must yield the same fingerprint.
    auto a = make_baseline_discovery();

    PrinterDiscovery b;
    nlohmann::json shuffled = nlohmann::json::array(
        {"quad_gantry_level", "probe", "bed_mesh", "gcode_macro CLEAN_NOZZLE",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "neopixel case_led",
         "temperature_sensor chamber_sensor", "controller_fan controller_fan",
         "heater_fan hotend_fan", "fan", "heater_generic chamber", "heater_bed", "extruder"});
    b.parse_objects(shuffled);
    b.set_hostname("voron-350");
    b.set_mcu("stm32f446");
    b.set_kinematics("corexy");

    REQUIRE(compute_hardware_fingerprint(a) == compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects added heater", "[hardware_fingerprint]") {
    auto a = make_baseline_discovery();

    PrinterDiscovery b;
    b.parse_objects(
        nlohmann::json::array({"extruder", "extruder1", "heater_bed", "heater_generic chamber",
                               "fan", "heater_fan hotend_fan", "controller_fan controller_fan",
                               "temperature_sensor chamber_sensor", "neopixel case_led",
                               "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro CLEAN_NOZZLE",
                               "bed_mesh", "probe", "quad_gantry_level"}));
    b.set_hostname("voron-350");
    b.set_mcu("stm32f446");
    b.set_kinematics("corexy");

    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects added fan", "[hardware_fingerprint]") {
    auto a = make_baseline_discovery();

    PrinterDiscovery b;
    b.parse_objects(nlohmann::json::array(
        {"extruder", "heater_bed", "heater_generic chamber", "fan", "heater_fan hotend_fan",
         "controller_fan controller_fan", "fan_generic aux_fan",
         "temperature_sensor chamber_sensor", "neopixel case_led",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro CLEAN_NOZZLE", "bed_mesh", "probe",
         "quad_gantry_level"}));
    b.set_hostname("voron-350");
    b.set_mcu("stm32f446");
    b.set_kinematics("corexy");

    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects added macro", "[hardware_fingerprint]") {
    auto a = make_baseline_discovery();

    PrinterDiscovery b;
    b.parse_objects(nlohmann::json::array(
        {"extruder", "heater_bed", "heater_generic chamber", "fan", "heater_fan hotend_fan",
         "controller_fan controller_fan", "temperature_sensor chamber_sensor", "neopixel case_led",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro CLEAN_NOZZLE",
         "gcode_macro PURGE_LINE", "bed_mesh", "probe", "quad_gantry_level"}));
    b.set_hostname("voron-350");
    b.set_mcu("stm32f446");
    b.set_kinematics("corexy");

    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects capability change", "[hardware_fingerprint]") {
    // Same component list, but z_tilt instead of quad_gantry_level — both
    // are bed-leveling objects, but the has_qgl / has_z_tilt flags differ.
    auto a = make_baseline_discovery();

    PrinterDiscovery b;
    b.parse_objects(nlohmann::json::array(
        {"extruder", "heater_bed", "heater_generic chamber", "fan", "heater_fan hotend_fan",
         "controller_fan controller_fan", "temperature_sensor chamber_sensor", "neopixel case_led",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro CLEAN_NOZZLE", "bed_mesh", "probe",
         "z_tilt"}));
    b.set_hostname("voron-350");
    b.set_mcu("stm32f446");
    b.set_kinematics("corexy");

    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects hostname change", "[hardware_fingerprint]") {
    auto a = make_baseline_discovery();
    auto b = make_baseline_discovery();
    b.set_hostname("voron-500");
    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects kinematics change", "[hardware_fingerprint]") {
    auto a = make_baseline_discovery();
    auto b = make_baseline_discovery();
    b.set_kinematics("cartesian");
    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint ignores software_version bumps", "[hardware_fingerprint]") {
    // A firmware_restart causes Klipper to report a new software_version
    // (e.g. v0.12.0-108 → v0.12.0-109) without any hardware change. The
    // fingerprint must NOT depend on software_version — otherwise every
    // firmware_restart would trigger a full discovery pipeline re-run,
    // defeating the purpose of the gate.
    auto a = make_baseline_discovery();
    auto b = make_baseline_discovery();
    a.set_software_version("v0.12.0-108-gabc1234");
    b.set_software_version("v0.12.0-109-gdef5678");
    REQUIRE(compute_hardware_fingerprint(a) == compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint ignores build_volume changes", "[hardware_fingerprint]") {
    // build_volume is populated later in discovery from bed_mesh bounds and
    // can legitimately vary across reconnects (mesh loaded, then unloaded).
    auto a = make_baseline_discovery();
    auto b = make_baseline_discovery();
    BuildVolume big{};
    big.x_min = 0;
    big.x_max = 350;
    big.y_min = 0;
    big.y_max = 350;
    big.z_max = 350;
    BuildVolume small{};
    small.x_min = 0;
    small.x_max = 150;
    small.y_min = 0;
    small.y_max = 150;
    small.z_max = 150;
    a.set_build_volume(big);
    b.set_build_volume(small);
    REQUIRE(compute_hardware_fingerprint(a) == compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint ignores printer_objects list reorder", "[hardware_fingerprint]") {
    // set_printer_objects stores the raw object list (used for debugging).
    // Its iteration order isn't part of the fingerprint — only the parsed
    // component vectors (heaters_, fans_, etc.) are.
    auto a = make_baseline_discovery();
    auto b = make_baseline_discovery();
    a.set_printer_objects({"extruder", "heater_bed", "fan", "neopixel case_led"});
    b.set_printer_objects({"neopixel case_led", "fan", "heater_bed", "extruder"});
    REQUIRE(compute_hardware_fingerprint(a) == compute_hardware_fingerprint(b));
}

TEST_CASE("Hardware fingerprint detects MMU attach", "[hardware_fingerprint]") {
    // User adds an AFC/MMU between reconnects — a major hardware change that
    // must trigger the full discovery pipeline (AMS UI, lane mapping, etc.).
    auto a = make_baseline_discovery();

    PrinterDiscovery b;
    b.parse_objects(nlohmann::json::array(
        {"extruder", "heater_bed", "heater_generic chamber", "fan", "heater_fan hotend_fan",
         "controller_fan controller_fan", "temperature_sensor chamber_sensor", "neopixel case_led",
         "gcode_macro HELIX_BED_MESH_IF_NEEDED", "gcode_macro CLEAN_NOZZLE", "bed_mesh", "probe",
         "quad_gantry_level", "AFC", "AFC_stepper lane0", "AFC_stepper lane1", "AFC_hub AFC_hub"}));
    b.set_hostname("voron-350");
    b.set_mcu("stm32f446");
    b.set_kinematics("corexy");

    REQUIRE(compute_hardware_fingerprint(a) != compute_hardware_fingerprint(b));
    REQUIRE(b.has_mmu());
    REQUIRE(b.mmu_type() == AmsType::AFC);
}
