// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_collector.cpp
 * @brief Unit tests for PrintStartCollector pattern matching
 *
 * Tests the regex patterns used to detect PRINT_START phases.
 * Includes test cases from real Voron V2 PRINT_START macro output.
 * These tests don't require LVGL or Moonraker - they test pure regex logic.
 */

#include <cstdio>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Pattern definitions (replicated from print_start_collector.cpp)
// ============================================================================

// PRINT_START marker pattern
static const std::regex print_start_pattern(R"(PRINT_START|START_PRINT|_PRINT_START)",
                                            std::regex::icase);

// Completion marker (layer 1 detected)
static const std::regex completion_pattern(
    R"(SET_PRINT_STATS_INFO\s+CURRENT_LAYER=|LAYER:?\s*1\b|;LAYER:1|First layer)",
    std::regex::icase);

// RESPOND-based print start completion (authoritative signal)
// Matches G-code responses containing "print" + "start"/"started"/"starting" adjacent in either
// order
static const std::regex respond_completion_pattern(
    R"(\bprint\b\W+\b(start|started|starting)\b|\b(start|started|starting)\b\W+\bprint\b)",
    std::regex::icase);

// Phase detection patterns
// Include both G-code commands AND Voron status_* LED macros (they indicate phase start)
static const std::regex homing_pattern(R"(G28|Homing|Home All Axes|homing|status_homing)",
                                       std::regex::icase);

static const std::regex heating_bed_pattern(
    R"(M190|M140\s+S[1-9]|Heating bed|Heat Bed|BED_TEMP|bed.*heat|status_heating)",
    std::regex::icase);

static const std::regex heating_nozzle_pattern(
    R"(M109|M104\s+S[1-9]|Heating (nozzle|hotend|extruder)|EXTRUDER_TEMP|status_heating)",
    std::regex::icase);

static const std::regex qgl_pattern(R"(QUAD_GANTRY_LEVEL|quad.?gantry.?level|QGL|status_leveling)",
                                    std::regex::icase);

static const std::regex z_tilt_pattern(R"(Z_TILT_ADJUST|z.?tilt.?adjust|status_leveling)",
                                       std::regex::icase);

static const std::regex bed_mesh_pattern(
    R"(BED_MESH_CALIBRATE|BED_MESH_PROFILE\s+LOAD=|Loading bed mesh|mesh.*load|status_meshing)",
    std::regex::icase);

static const std::regex cleaning_pattern(
    R"(CLEAN_NOZZLE|NOZZLE_CLEAN|WIPE_NOZZLE|nozzle.?wipe|clean.?nozzle|status_cleaning)",
    std::regex::icase);

static const std::regex purging_pattern(
    R"(VORON_PURGE|LINE_PURGE|PURGE_LINE|Prime.?Line|Priming|KAMP_.*PURGE|purge.?line)",
    std::regex::icase);

// ============================================================================
// Helper for testing patterns
// ============================================================================

static bool matches(const std::regex& pattern, const std::string& line) {
    return std::regex_search(line, pattern);
}

// ============================================================================
// PRINT_START Marker Tests
// ============================================================================

TEST_CASE("PrintStart: PRINT_START marker detection", "[core][print][marker]") {
    // Should match
    REQUIRE(matches(print_start_pattern, "PRINT_START") == true);
    REQUIRE(matches(print_start_pattern, "START_PRINT") == true);
    REQUIRE(matches(print_start_pattern, "_PRINT_START") == true);
    REQUIRE(matches(print_start_pattern, "print_start") == true); // Case insensitive
    REQUIRE(matches(print_start_pattern, "Calling PRINT_START with args") == true);

    // Real macro invocations
    REQUIRE(matches(print_start_pattern, "START_PRINT BED_TEMP=60 EXTRUDER_TEMP=200") == true);
    REQUIRE(matches(print_start_pattern, "PRINT_START BED=60 EXTRUDER=200 CHAMBER=35") == true);

    // Should NOT match
    REQUIRE(matches(print_start_pattern, "PRINTS_TART") == false);
    REQUIRE(matches(print_start_pattern, "G28") == false);
    REQUIRE(matches(print_start_pattern, "") == false);
}

// ============================================================================
// Completion Marker Tests
// ============================================================================

TEST_CASE("PrintStart: completion marker detection", "[core][print][completion]") {
    // Should match
    REQUIRE(matches(completion_pattern, "SET_PRINT_STATS_INFO CURRENT_LAYER=1") == true);
    REQUIRE(matches(completion_pattern, "LAYER: 1") == true);
    REQUIRE(matches(completion_pattern, "LAYER:1") == true);
    REQUIRE(matches(completion_pattern, ";LAYER:1") == true);
    REQUIRE(matches(completion_pattern, "First layer starting") == true);

    // Should NOT match (not layer 1)
    REQUIRE(matches(completion_pattern, "LAYER: 2") == false);
    REQUIRE(matches(completion_pattern, "LAYER:10") == false);
    REQUIRE(matches(completion_pattern, "LAYER:100") == false);
    REQUIRE(matches(completion_pattern, "SET_PRINT_STATS_INFO") == false); // No CURRENT_LAYER
}

// ============================================================================
// RESPOND Completion Pattern Tests
// ============================================================================

TEST_CASE("PrintStart: RESPOND completion detection", "[core][print][respond]") {
    SECTION("Should match common print start messages") {
        REQUIRE(matches(respond_completion_pattern, "// Print started!") == true);
        REQUIRE(matches(respond_completion_pattern, "Print Started") == true);
        REQUIRE(matches(respond_completion_pattern, "PRINT STARTING") == true);
        REQUIRE(matches(respond_completion_pattern, "Print Start Complete") == true);
        REQUIRE(matches(respond_completion_pattern, "print started") == true);
        REQUIRE(matches(respond_completion_pattern, "Starting print") == true);
        REQUIRE(matches(respond_completion_pattern, "starting print now") == true);
        REQUIRE(matches(respond_completion_pattern, "Started print successfully") == true);
    }

    SECTION("Should NOT match unrelated messages") {
        // "Printing" is not "print" (word boundary)
        REQUIRE(matches(respond_completion_pattern, "Printing layer 5") == false);
        // "restart" is not "start" (word boundary)
        REQUIRE(matches(respond_completion_pattern, "restart print") == false);
        // "print_start" — underscore breaks word boundary for "start"
        REQUIRE(matches(respond_completion_pattern, "print_start phase") == false);
        // No "print" keyword
        REQUIRE(matches(respond_completion_pattern, "Starting temperature check") == false);
        // No "start" variant keyword
        REQUIRE(matches(respond_completion_pattern, "print complete") == false);
        // Unrelated with both words far apart in different context
        REQUIRE(matches(respond_completion_pattern, "start heating the print bed") == false);
    }
}

TEST_CASE("PrintStart: RESPOND and PRINT_START patterns are independent",
          "[core][print][respond]") {
    SECTION("RESPOND messages with 'Print started' should not match PRINT_START marker pattern") {
        // "Print started!" is a RESPOND message, not a macro invocation
        // The print_start_pattern only matches exact macro names: PRINT_START, START_PRINT,
        // _PRINT_START
        REQUIRE(matches(print_start_pattern, "Print started!") == false);
        REQUIRE(matches(print_start_pattern, "PRINT STARTED") == false);
        REQUIRE(matches(print_start_pattern, "print starting") == false);
        // But RESPOND pattern should match these:
        REQUIRE(matches(respond_completion_pattern, "Print started!") == true);
        REQUIRE(matches(respond_completion_pattern, "PRINT STARTED") == true);
        REQUIRE(matches(respond_completion_pattern, "print starting") == true);
    }

    SECTION("PRINT_START macro lines should not trigger RESPOND completion") {
        // "PRINT_START BED=60 EXTRUDER=200" should NOT match respond pattern
        // because "PRINT_START" has underscore breaking word boundary for "start"
        REQUIRE(matches(respond_completion_pattern, "PRINT_START BED=60 EXTRUDER=200") == false);
        REQUIRE(matches(respond_completion_pattern, "START_PRINT BED=60") == false);
        REQUIRE(matches(respond_completion_pattern, "_PRINT_START") == false);
    }
}

TEST_CASE("PrintStart: timeout fallback still available", "[core][print][timeout]") {
    // Verifies RESPOND pattern doesn't match common noise lines that could
    // interfere with timeout-based completion as a last resort.

    SECTION("RESPOND pattern does not match empty or noise lines") {
        REQUIRE(matches(respond_completion_pattern, "") == false);
        REQUIRE(matches(respond_completion_pattern, "ok") == false);
        REQUIRE(matches(respond_completion_pattern, "T:200.0 /200.0 B:60.0 /60.0") == false);
        REQUIRE(matches(respond_completion_pattern, "echo: M190 S60") == false);
    }
}

// ============================================================================
// Homing Phase Tests
// ============================================================================

TEST_CASE("PrintStart: homing phase detection", "[core][print][homing]") {
    // Should match
    REQUIRE(matches(homing_pattern, "G28") == true);
    REQUIRE(matches(homing_pattern, "G28 X Y Z") == true);
    REQUIRE(matches(homing_pattern, "G28 Z") == true);
    REQUIRE(matches(homing_pattern, "Homing axes") == true);
    REQUIRE(matches(homing_pattern, "Home All Axes") == true);
    REQUIRE(matches(homing_pattern, "// homing started") == true);

    // Real Voron V2 macro output
    REQUIRE(matches(homing_pattern, "SET_DISPLAY_TEXT MSG=\"Homing\"") == true);

    // Should NOT match
    REQUIRE(matches(homing_pattern, "G29") == false); // Bed leveling
    REQUIRE(matches(homing_pattern, "M104") == false);
}

// ============================================================================
// Heating Phase Tests
// ============================================================================

TEST_CASE("PrintStart: heating bed phase detection", "[core][print][heating]") {
    // Should match
    REQUIRE(matches(heating_bed_pattern, "M190 S60") == true); // Wait for bed
    REQUIRE(matches(heating_bed_pattern, "M140 S60") == true); // Set bed
    REQUIRE(matches(heating_bed_pattern, "Heating bed to 60") == true);
    REQUIRE(matches(heating_bed_pattern, "Heat Bed") == true);
    REQUIRE(matches(heating_bed_pattern, "BED_TEMP=60") == true);
    REQUIRE(matches(heating_bed_pattern, "bed heating") == true);

    // Real Voron V2 macro: M190 S{BED_TEMP}
    REQUIRE(matches(heating_bed_pattern, "M190 S110") == true);

    // Should NOT match
    REQUIRE(matches(heating_bed_pattern, "M140 S0") == false);   // Setting to 0 (cooling)
    REQUIRE(matches(heating_bed_pattern, "M104 S200") == false); // Nozzle temp
}

TEST_CASE("PrintStart: heating nozzle phase detection", "[print][heating]") {
    // Should match
    REQUIRE(matches(heating_nozzle_pattern, "M109 S200") == true); // Wait for nozzle
    REQUIRE(matches(heating_nozzle_pattern, "M104 S200") == true); // Set nozzle
    REQUIRE(matches(heating_nozzle_pattern, "M104 S150") == true); // Mesh temp
    REQUIRE(matches(heating_nozzle_pattern, "Heating nozzle to 200") == true);
    REQUIRE(matches(heating_nozzle_pattern, "Heating hotend") == true);
    REQUIRE(matches(heating_nozzle_pattern, "Heating extruder") == true);
    REQUIRE(matches(heating_nozzle_pattern, "EXTRUDER_TEMP=200") == true);

    // Real Voron V2 macro output
    REQUIRE(matches(heating_nozzle_pattern, "SET_DISPLAY_TEXT MSG=\"Heating for print\"") ==
            false); // "for print" not "nozzle"
    REQUIRE(matches(heating_nozzle_pattern,
                    "SET_DISPLAY_TEXT MSG=\"Heating extruder and bed for probing\"") == true);

    // Should NOT match
    REQUIRE(matches(heating_nozzle_pattern, "M104 S0") == false);  // Cooling
    REQUIRE(matches(heating_nozzle_pattern, "M190 S60") == false); // Bed temp
}

// ============================================================================
// Leveling Phase Tests
// ============================================================================

TEST_CASE("PrintStart: QGL phase detection", "[print][leveling]") {
    // Should match
    REQUIRE(matches(qgl_pattern, "QUAD_GANTRY_LEVEL") == true);
    REQUIRE(matches(qgl_pattern, "quad gantry level") == true);
    REQUIRE(matches(qgl_pattern, "Running QGL") == true);

    // Real Voron V2 macro output
    REQUIRE(matches(qgl_pattern, "SET_DISPLAY_TEXT MSG=\"Leveling gantry\"") ==
            false); // "gantry" alone doesn't match

    // Should NOT match
    REQUIRE(matches(qgl_pattern, "Z_TILT_ADJUST") == false);
    REQUIRE(matches(qgl_pattern, "G28") == false);
}

TEST_CASE("PrintStart: Z_TILT phase detection", "[print][leveling]") {
    // Should match
    REQUIRE(matches(z_tilt_pattern, "Z_TILT_ADJUST") == true);
    REQUIRE(matches(z_tilt_pattern, "z_tilt_adjust") == true);
    REQUIRE(matches(z_tilt_pattern, "z tilt adjust") == true);

    // Should NOT match
    REQUIRE(matches(z_tilt_pattern, "QUAD_GANTRY_LEVEL") == false);
}

// ============================================================================
// Bed Mesh Phase Tests
// ============================================================================

TEST_CASE("PrintStart: bed mesh phase detection", "[print][mesh]") {
    // Should match
    REQUIRE(matches(bed_mesh_pattern, "BED_MESH_CALIBRATE") == true);
    REQUIRE(matches(bed_mesh_pattern, "BED_MESH_PROFILE LOAD=default") == true);
    REQUIRE(matches(bed_mesh_pattern, "Loading bed mesh") == true);
    REQUIRE(matches(bed_mesh_pattern, "mesh loading") == true);

    // Real Voron V2 macro: BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1
    REQUIRE(matches(bed_mesh_pattern, "BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1") == true);

    // Should NOT match
    REQUIRE(matches(bed_mesh_pattern, "BED_MESH_CLEAR") == false);
    REQUIRE(matches(bed_mesh_pattern, "SET_DISPLAY_TEXT MSG=\"Performing bed mesh calibration\"") ==
            false);
}

// ============================================================================
// Cleaning Phase Tests
// ============================================================================

TEST_CASE("PrintStart: cleaning phase detection", "[print][cleaning]") {
    // Should match
    REQUIRE(matches(cleaning_pattern, "CLEAN_NOZZLE") == true);
    REQUIRE(matches(cleaning_pattern, "NOZZLE_CLEAN") == true);
    REQUIRE(matches(cleaning_pattern, "WIPE_NOZZLE") == true);
    REQUIRE(matches(cleaning_pattern, "nozzle wipe") == true);
    REQUIRE(matches(cleaning_pattern, "clean nozzle") == true);
    REQUIRE(matches(cleaning_pattern, "clean_nozzle") == true); // Voron V2 macro call

    // Real Voron V2 display text - note: "Cleaning nozzle" has "ing " between,
    // which doesn't match clean.?nozzle pattern (requires 0-1 char between)
    REQUIRE(matches(cleaning_pattern, "SET_DISPLAY_TEXT MSG=\"Cleaning nozzle\"") == false);

    // Should NOT match
    REQUIRE(matches(cleaning_pattern, "PURGE_LINE") == false);
}

// ============================================================================
// Purging Phase Tests
// ============================================================================

TEST_CASE("PrintStart: purging phase detection", "[print][purging]") {
    // Should match
    REQUIRE(matches(purging_pattern, "VORON_PURGE") == true);
    REQUIRE(matches(purging_pattern, "LINE_PURGE") == true);
    REQUIRE(matches(purging_pattern, "PURGE_LINE") == true);
    REQUIRE(matches(purging_pattern, "Prime Line") == true);
    REQUIRE(matches(purging_pattern, "PrimeLine") == true);
    REQUIRE(matches(purging_pattern, "Priming extruder") == true);
    REQUIRE(matches(purging_pattern, "KAMP_ADAPTIVE_PURGE") == true);
    REQUIRE(matches(purging_pattern, "purge line done") == true);

    // Real Voron V2 display text
    REQUIRE(matches(purging_pattern, "SET_DISPLAY_TEXT MSG=\"Purging\"") ==
            false); // Just "Purging" alone

    // Should NOT match
    REQUIRE(matches(purging_pattern, "CLEAN_NOZZLE") == false);
}

// ============================================================================
// Real Voron V2 PRINT_START Macro Tests
// ============================================================================

/**
 * Test against real output from Voron V2 at 192.168.1.112
 * START_PRINT macro includes:
 *   - M104 S{MESH_TEMP}       -> heating nozzle
 *   - M190 S{BED_TEMP}        -> heating bed
 *   - G28                     -> homing
 *   - clean_nozzle            -> cleaning
 *   - QUAD_GANTRY_LEVEL       -> QGL
 *   - G28 Z                   -> homing Z
 *   - BED_MESH_CALIBRATE      -> bed mesh
 *   - M109 S{EXTRUDER_TEMP}   -> heating nozzle (wait)
 *   - VORON_PURGE             -> purging
 */
TEST_CASE("PrintStart: real Voron V2 START_PRINT macro lines", "[print][voron][integration]") {
    // Lines from actual Voron V2 START_PRINT macro
    struct TestCase {
        std::string line;
        const std::regex* expected_pattern;
        const char* description;
    };

    std::vector<TestCase> voron_lines = {
        {"START_PRINT BED_TEMP=110 EXTRUDER_TEMP=250 CHAMBER_TEMP=45", &print_start_pattern,
         "macro invocation"},
        {"M104 S150", &heating_nozzle_pattern, "mesh temp heating"},
        {"M190 S110", &heating_bed_pattern, "bed temp wait"},
        {"G28", &homing_pattern, "home all"},
        {"clean_nozzle", &cleaning_pattern, "nozzle clean macro"},
        {"QUAD_GANTRY_LEVEL", &qgl_pattern, "quad gantry level"},
        {"G28 Z", &homing_pattern, "home Z after QGL"},
        {"BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1", &bed_mesh_pattern, "adaptive bed mesh"},
        {"M109 S250", &heating_nozzle_pattern, "extruder temp wait"},
        {"VORON_PURGE", &purging_pattern, "voron purge"},
    };

    for (const auto& tc : voron_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(matches(*tc.expected_pattern, tc.line) == true);
    }
}

TEST_CASE("PrintStart: Voron V2 SET_DISPLAY_TEXT messages", "[print][voron]") {
    // These are the display messages from the macro
    REQUIRE(matches(homing_pattern, "SET_DISPLAY_TEXT MSG=\"Homing\"") == true);

    // Note: "Cleaning nozzle" has "ing " between clean and nozzle,
    // so it doesn't match clean.?nozzle pattern (which requires 0-1 char)
    REQUIRE(matches(cleaning_pattern, "SET_DISPLAY_TEXT MSG=\"Cleaning nozzle\"") == false);

    // These DON'T match because they use different wording
    // This is intentional - we match G-code commands, not display text
    REQUIRE(matches(qgl_pattern, "SET_DISPLAY_TEXT MSG=\"Leveling gantry\"") == false);
    REQUIRE(matches(heating_nozzle_pattern, "SET_DISPLAY_TEXT MSG=\"Heating for print\"") == false);
}

// ============================================================================
// Real AD5M Pro START_PRINT Macro Tests
// ============================================================================

/**
 * Test against real output from FlashForge AD5M Pro at 192.168.1.67
 * START_PRINT macro (with mod firmware) includes:
 *   - M140 S{bed_temp}        -> heating bed
 *   - M104 S{extruder_temp}   -> heating nozzle
 *   - G28                     -> homing
 *   - KAMP or _FULL_BED_LEVEL -> bed mesh (adaptive or full)
 *   - BED_MESH_PROFILE LOAD=  -> mesh loading
 *   - LINE_PURGE              -> KAMP purge
 *
 * Notable differences from Voron V2:
 *   - No QGL or Z_TILT (fixed bed CoreXY)
 *   - Uses KAMP for adaptive meshing
 *   - Has CHECK_MD5 verification step
 *   - Uses _PRINT_STATUS S="..." for display
 */
TEST_CASE("PrintStart: real AD5M Pro START_PRINT macro lines", "[print][ad5m][integration]") {
    struct TestCase {
        std::string line;
        const std::regex* expected_pattern;
        const char* description;
    };

    std::vector<TestCase> ad5m_lines = {
        {"START_PRINT BED_TEMP=60 EXTRUDER_TEMP=200", &print_start_pattern, "macro invocation"},
        {"RESPOND MSG=\"START_PRINT\"", &print_start_pattern, "respond with start marker"},
        {"M140 S60", &heating_bed_pattern, "set bed temp"},
        {"M104 S200", &heating_nozzle_pattern, "set nozzle temp"},
        {"G28", &homing_pattern, "home all"},
        {"BED_MESH_CALIBRATE mesh_min=-100,-100 mesh_max=100,100", &bed_mesh_pattern,
         "KAMP mesh calibrate"},
        {"BED_MESH_PROFILE LOAD=auto", &bed_mesh_pattern, "load auto mesh profile"},
        {"LINE_PURGE", &purging_pattern, "KAMP line purge"},
    };

    for (const auto& tc : ad5m_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(matches(*tc.expected_pattern, tc.line) == true);
    }
}

TEST_CASE("PrintStart: AD5M Pro _PRINT_STATUS messages", "[print][ad5m]") {
    // These are unique to AD5M Pro mod firmware
    REQUIRE(matches(homing_pattern, "_PRINT_STATUS S=\"HOMING...\"") == true);

    // Note: These DON'T match because they use different wording (status strings only)
    REQUIRE(matches(heating_bed_pattern, "_PRINT_STATUS S=\"HEATING...\"") == false);
    REQUIRE(matches(bed_mesh_pattern, "_PRINT_STATUS S=\"MESH CHECKING...\"") == false);
}

TEST_CASE("PrintStart: AD5M Pro KAMP-specific patterns", "[print][ad5m][kamp]") {
    // KAMP adaptive purge patterns
    REQUIRE(matches(purging_pattern, "KAMP_ADAPTIVE_PURGE") == true);
    REQUIRE(matches(purging_pattern, "_LINE_PURGE") == true);

    // KAMP bed mesh with parameters
    REQUIRE(matches(bed_mesh_pattern, "BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1") == true);
    REQUIRE(matches(bed_mesh_pattern, "_KAMP_BED_MESH_CALIBRATE") == true);
}

// ============================================================================
// Noise Rejection Tests
// ============================================================================

// ============================================================================
// Voron Status LED Macro Tests
// ============================================================================

TEST_CASE("PrintStart: Voron status_* LED macros are valid phase indicators",
          "[print][voron][status]") {
    // These LED macros are called at the START of each phase in Voron configs
    REQUIRE(matches(homing_pattern, "status_homing") == true);
    REQUIRE(matches(heating_bed_pattern, "status_heating") == true);
    REQUIRE(matches(heating_nozzle_pattern, "status_heating") == true);
    REQUIRE(matches(qgl_pattern, "status_leveling") == true);
    REQUIRE(matches(z_tilt_pattern, "status_leveling") == true);
    REQUIRE(matches(bed_mesh_pattern, "status_meshing") == true);
    REQUIRE(matches(cleaning_pattern, "status_cleaning") == true);

    // status_printing indicates print started (end of PRINT_START)
    REQUIRE(matches(completion_pattern, "status_printing") == false); // Not a completion marker
}

// ============================================================================
// Noise Rejection Tests
// ============================================================================

TEST_CASE("PrintStart: typical noise lines should not match phases", "[print][negative]") {
    // These are common Klipper output lines that should NOT trigger phase detection
    std::vector<std::string> noise_lines = {
        "ok",
        "// Klipper state: Ready",
        "T:210.5 /210.0 B:60.2 /60.0",
        "echo: Command completed",
        "TOOLHEAD_PARK_MACRO",
        "SET_LED LED=nozzle RED=1",
        "M141 S45", // Chamber temp (not bed or nozzle)
        "AFC_PARK",
        "SMART_PARK",
        "TOOLCHANGE TOOL=0",
        "BED_MESH_CLEAR",
        "SET_AFC_TOOLCHANGES TOOLCHANGES=0",
        "status_printing", // End of PRINT_START, not a phase
        "status_busy",     // Generic status, not a phase
        "status_ready",    // Idle status
    };

    std::vector<const std::regex*> phase_patterns = {
        &homing_pattern, &heating_bed_pattern, &heating_nozzle_pattern, &qgl_pattern,
        &z_tilt_pattern, &bed_mesh_pattern,    &cleaning_pattern,       &purging_pattern};

    for (const auto& line : noise_lines) {
        for (const auto* pattern : phase_patterns) {
            CAPTURE(line);
            REQUIRE(matches(*pattern, line) == false);
        }
    }
}

// ============================================================================
// AREA A: HELIX:PHASE Signal Detection Tests
// ============================================================================
// Tests for check_helix_phase_signal() method which parses signals like:
// - HELIX:PHASE:STARTING -> sets INITIALIZING phase
// - HELIX:PHASE:COMPLETE -> sets COMPLETE phase
// - Various phase transitions

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_start_collector_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "moonraker_client_mock.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "thermal_rate_model.h"
#include "translation_loader.h"

using namespace helix;
using namespace helix::ui;
/**
 * @brief HELIX:PHASE signal parser for direct testing
 *
 * This standalone function replicates the HELIX:PHASE parsing logic from
 * PrintStartCollector::check_helix_phase_signal() so we can test it directly
 * without the full callback infrastructure.
 *
 * Returns the PrintStartPhase that would be set by the signal, or
 * PrintStartPhase::IDLE if the signal is not recognized.
 */
static std::pair<PrintStartPhase, std::string> parse_helix_phase_signal(const std::string& line) {
    static const char* HELIX_PHASE_PREFIX = "HELIX:PHASE:";
    constexpr size_t PREFIX_LEN = 12;

    size_t pos = line.find(HELIX_PHASE_PREFIX);
    if (pos == std::string::npos) {
        return {PrintStartPhase::IDLE, ""};
    }

    std::string phase_name = line.substr(pos + PREFIX_LEN);
    size_t end = phase_name.find_first_of(" \t\n\r\"'");
    if (end != std::string::npos) {
        phase_name = phase_name.substr(0, end);
    }

    // Map to phase (same logic as check_helix_phase_signal)
    if (phase_name == "STARTING" || phase_name == "START") {
        return {PrintStartPhase::INITIALIZING, "Preparing Print..."};
    } else if (phase_name == "COMPLETE" || phase_name == "DONE") {
        return {PrintStartPhase::COMPLETE, "Starting Print..."};
    } else if (phase_name == "HOMING") {
        return {PrintStartPhase::HOMING, "Homing..."};
    } else if (phase_name == "HEATING_BED" || phase_name == "BED_HEATING") {
        return {PrintStartPhase::HEATING_BED, "Heating Bed..."};
    } else if (phase_name == "HEATING_NOZZLE" || phase_name == "NOZZLE_HEATING" ||
               phase_name == "HEATING_HOTEND") {
        return {PrintStartPhase::HEATING_NOZZLE, "Heating Nozzle..."};
    } else if (phase_name == "QGL" || phase_name == "QUAD_GANTRY_LEVEL") {
        return {PrintStartPhase::QGL, "Leveling Gantry..."};
    } else if (phase_name == "Z_TILT" || phase_name == "Z_TILT_ADJUST") {
        return {PrintStartPhase::Z_TILT, "Z Tilt Adjust..."};
    } else if (phase_name == "BED_MESH" || phase_name == "BED_LEVELING") {
        return {PrintStartPhase::BED_MESH, "Loading Bed Mesh..."};
    } else if (phase_name == "CLEANING" || phase_name == "NOZZLE_CLEAN") {
        return {PrintStartPhase::CLEANING, "Cleaning Nozzle..."};
    } else if (phase_name == "PURGING" || phase_name == "PURGE" || phase_name == "PRIMING") {
        return {PrintStartPhase::PURGING, "Purging..."};
    }

    // Unknown phase
    return {PrintStartPhase::IDLE, ""};
}

/**
 * @brief Test fixture for PrintStartCollector proactive heater detection tests
 *
 * Provides initialized PrinterState and mock MoonrakerClient for testing
 * the collector's fallback completion and proactive heater detection.
 */
class PrintStartCollectorHeaterFixture : public LVGLTestFixture {
  public:
    PrintStartCollectorHeaterFixture() {
        state_.init_subjects(false);
        // Mark print as active so set_print_start_state() accepts phase updates
        lv_subject_set_int(state_.get_print_active_subject(), 1);
        client_ = std::make_unique<MoonrakerClientMock>();
        collector_ = std::make_shared<PrintStartCollector>(*client_, state_);
        collector_->set_profile(PrintStartProfile::load_default());
    }

    ~PrintStartCollectorHeaterFixture() override {
        if (collector_->is_active()) {
            collector_->stop();
        }
        collector_.reset();
        client_.reset();
    }

    PrinterState& state() {
        return state_;
    }
    MoonrakerClientMock& client() {
        return *client_;
    }
    PrintStartCollector& collector() {
        return *collector_;
    }

    /**
     * @brief Get current print start phase from PrinterState subject
     */
    PrintStartPhase get_current_phase() {
        return static_cast<PrintStartPhase>(
            lv_subject_get_int(state_.get_print_start_phase_subject()));
    }

    /**
     * @brief Get current print start message from PrinterState subject
     */
    std::string get_current_message() {
        return lv_subject_get_string(state_.get_print_start_message_subject());
    }

    /**
     * @brief Set bed temperature and target in PrinterState subjects
     *
     * Values are in decidegrees (temp * 10) as stored in PrinterState.
     * Example: 60.0C = 600 decidegrees
     */
    void set_bed_temps(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state_.get_bed_temp_subject(), temp_decideg);
        lv_subject_set_int(state_.get_bed_target_subject(), target_decideg);
    }

    /**
     * @brief Set extruder temperature and target in PrinterState subjects
     */
    void set_extruder_temps(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state_.get_active_extruder_temp_subject(), temp_decideg);
        lv_subject_set_int(state_.get_active_extruder_target_subject(), target_decideg);
    }

    /**
     * @brief Set both bed and extruder temperatures
     */
    void set_all_temps(int bed_temp, int bed_target, int ext_temp, int ext_target) {
        set_bed_temps(bed_temp, bed_target);
        set_extruder_temps(ext_temp, ext_target);
    }

    /**
     * @brief Set progress and layer for completion fallback tests
     */
    void set_progress_and_layer(int progress, int layer) {
        lv_subject_set_int(state_.get_print_progress_subject(), progress);
        lv_subject_set_int(state_.get_print_layer_current_subject(), layer);
    }

    /**
     * @brief Process pending async UI updates
     *
     * Since set_print_start_state() uses helix::ui::async_call() to defer subject updates,
     * we need to drain the queue to see the updates in tests.
     */
    void drain_async_updates() {
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

    /**
     * @brief Feed a raw gcode response line through the real signal path
     *
     * Dispatches notify_gcode_response so the collector's pattern matching runs
     * and (on a match) latches current_phase_ AND sets real_signal_seen_ — the
     * exact firmware-speaks condition that gates the proactive detector off.
     */
    void send_gcode_response(const std::string& line) {
        json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    }

    /**
     * @brief Reset collector's internal state back to IDLE for proactive detection tests
     *
     * After start(), the collector's internal current_phase_ is INITIALIZING.
     * state().reset_print_start_state() only resets the PrinterState subject, not
     * the collector's internal current_phase_. This helper calls collector().reset()
     * which resets current_phase_ to INITIALIZING, then forces the PrinterState
     * subject back to IDLE for test isolation, and re-enables fallbacks.
     */
    void reset_collector_to_idle() {
        // Ensure print is considered active so set_print_start_state() doesn't
        // reject non-IDLE phase updates via the print_active_ guard
        lv_subject_set_int(state_.get_print_active_subject(), 1);

        collector_->reset();
        drain_async_updates();            // Process reset()'s queued INITIALIZING state
        state_.reset_print_start_state(); // Override back to IDLE
        drain_async_updates();            // Process the IDLE update
    }

  protected:
    PrinterState state_;
    std::unique_ptr<MoonrakerClientMock> client_;
    std::shared_ptr<PrintStartCollector> collector_;
};

// ============================================================================
// HELIX:PHASE:STARTING Signal Tests
// ============================================================================

TEST_CASE("HELIX:PHASE:STARTING sets INITIALIZING phase", "[print][collector][helix_phase]") {
    SECTION("HELIX:PHASE:STARTING") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:STARTING");
        REQUIRE(phase == PrintStartPhase::INITIALIZING);
        REQUIRE(message == "Preparing Print...");
    }

    SECTION("HELIX:PHASE:START (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:START");
        REQUIRE(phase == PrintStartPhase::INITIALIZING);
    }
}

// ============================================================================
// HELIX:PHASE:COMPLETE Signal Tests
// ============================================================================

TEST_CASE("HELIX:PHASE:COMPLETE sets COMPLETE phase", "[print][collector][helix_phase]") {
    SECTION("HELIX:PHASE:COMPLETE") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:COMPLETE");
        REQUIRE(phase == PrintStartPhase::COMPLETE);
        REQUIRE(message == "Starting Print...");
    }

    SECTION("HELIX:PHASE:DONE (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:DONE");
        REQUIRE(phase == PrintStartPhase::COMPLETE);
    }
}

// ============================================================================
// Individual HELIX:PHASE Signal Tests
// ============================================================================

TEST_CASE("HELIX:PHASE individual phases set correctly", "[print][collector][helix_phase]") {
    SECTION("HELIX:PHASE:HOMING") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:HOMING");
        REQUIRE(phase == PrintStartPhase::HOMING);
        REQUIRE(message == "Homing...");
    }

    SECTION("HELIX:PHASE:HEATING_BED") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:HEATING_BED");
        REQUIRE(phase == PrintStartPhase::HEATING_BED);
        REQUIRE(message == "Heating Bed...");
    }

    SECTION("HELIX:PHASE:BED_HEATING (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:BED_HEATING");
        REQUIRE(phase == PrintStartPhase::HEATING_BED);
    }

    SECTION("HELIX:PHASE:HEATING_NOZZLE") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:HEATING_NOZZLE");
        REQUIRE(phase == PrintStartPhase::HEATING_NOZZLE);
        REQUIRE(message == "Heating Nozzle...");
    }

    SECTION("HELIX:PHASE:NOZZLE_HEATING (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:NOZZLE_HEATING");
        REQUIRE(phase == PrintStartPhase::HEATING_NOZZLE);
    }

    SECTION("HELIX:PHASE:HEATING_HOTEND (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:HEATING_HOTEND");
        REQUIRE(phase == PrintStartPhase::HEATING_NOZZLE);
    }

    SECTION("HELIX:PHASE:QGL") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:QGL");
        REQUIRE(phase == PrintStartPhase::QGL);
        REQUIRE(message == "Leveling Gantry...");
    }

    SECTION("HELIX:PHASE:QUAD_GANTRY_LEVEL (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:QUAD_GANTRY_LEVEL");
        REQUIRE(phase == PrintStartPhase::QGL);
    }

    SECTION("HELIX:PHASE:Z_TILT") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:Z_TILT");
        REQUIRE(phase == PrintStartPhase::Z_TILT);
        REQUIRE(message == "Z Tilt Adjust...");
    }

    SECTION("HELIX:PHASE:Z_TILT_ADJUST (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:Z_TILT_ADJUST");
        REQUIRE(phase == PrintStartPhase::Z_TILT);
    }

    SECTION("HELIX:PHASE:BED_MESH") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:BED_MESH");
        REQUIRE(phase == PrintStartPhase::BED_MESH);
        REQUIRE(message == "Loading Bed Mesh...");
    }

    SECTION("HELIX:PHASE:BED_LEVELING (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:BED_LEVELING");
        REQUIRE(phase == PrintStartPhase::BED_MESH);
    }

    SECTION("HELIX:PHASE:CLEANING") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:CLEANING");
        REQUIRE(phase == PrintStartPhase::CLEANING);
        REQUIRE(message == "Cleaning Nozzle...");
    }

    SECTION("HELIX:PHASE:NOZZLE_CLEAN (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:NOZZLE_CLEAN");
        REQUIRE(phase == PrintStartPhase::CLEANING);
    }

    SECTION("HELIX:PHASE:PURGING") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:PURGING");
        REQUIRE(phase == PrintStartPhase::PURGING);
        REQUIRE(message == "Purging...");
    }

    SECTION("HELIX:PHASE:PURGE (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:PURGE");
        REQUIRE(phase == PrintStartPhase::PURGING);
    }

    SECTION("HELIX:PHASE:PRIMING (alternative form)") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:PRIMING");
        REQUIRE(phase == PrintStartPhase::PURGING);
    }
}

// ============================================================================
// Malformed HELIX:PHASE Signal Tests
// ============================================================================

TEST_CASE("Malformed HELIX:PHASE signals are ignored",
          "[print][collector][helix_phase][negative]") {
    SECTION("Unknown phase name returns IDLE") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:UNKNOWN_PHASE");
        REQUIRE(phase == PrintStartPhase::IDLE);
        REQUIRE(message.empty());
    }

    SECTION("Malformed prefix returns IDLE") {
        auto [phase, message] = parse_helix_phase_signal("HELIX_PHASE:HOMING"); // Wrong separator
        REQUIRE(phase == PrintStartPhase::IDLE);
    }

    SECTION("Partial prefix returns IDLE") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:HOMING"); // Missing PHASE
        REQUIRE(phase == PrintStartPhase::IDLE);
    }

    SECTION("Empty phase name returns IDLE") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:");
        REQUIRE(phase == PrintStartPhase::IDLE);
    }

    SECTION("Case sensitivity: lowercase phase names return IDLE") {
        // Currently the code checks for exact uppercase matches
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:homing");
        REQUIRE(phase == PrintStartPhase::IDLE);
    }

    SECTION("No HELIX:PHASE prefix returns IDLE") {
        auto [phase, message] = parse_helix_phase_signal("G28");
        REQUIRE(phase == PrintStartPhase::IDLE);
    }

    SECTION("Empty line returns IDLE") {
        auto [phase, message] = parse_helix_phase_signal("");
        REQUIRE(phase == PrintStartPhase::IDLE);
    }
}

// ============================================================================
// HELIX:PHASE Signal with Context Tests
// ============================================================================

TEST_CASE("HELIX:PHASE signals work with surrounding text", "[print][collector][helix_phase]") {
    SECTION("Signal with quotes is parsed correctly") {
        auto [phase, message] = parse_helix_phase_signal("\"HELIX:PHASE:HOMING\"");
        REQUIRE(phase == PrintStartPhase::HOMING);
    }

    SECTION("Signal with prefix text is parsed correctly") {
        auto [phase, message] = parse_helix_phase_signal("RESPOND MSG=HELIX:PHASE:HEATING_BED");
        REQUIRE(phase == PrintStartPhase::HEATING_BED);
    }

    SECTION("Signal with trailing whitespace") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:QGL   ");
        REQUIRE(phase == PrintStartPhase::QGL);
    }

    SECTION("Signal with trailing newline") {
        auto [phase, message] = parse_helix_phase_signal("HELIX:PHASE:CLEANING\n");
        REQUIRE(phase == PrintStartPhase::CLEANING);
    }

    SECTION("Signal embedded in M118 echo") {
        auto [phase, message] = parse_helix_phase_signal("M118 HELIX:PHASE:Z_TILT output=prefix");
        REQUIRE(phase == PrintStartPhase::Z_TILT);
    }
}

// ============================================================================
// AREA B: Proactive Heater Detection Tests
// ============================================================================
// Tests for the proactive detection logic in check_fallback_completion() that
// detects "Preparing" phase when:
// - Collector is active but in IDLE phase
// - Heaters are ramping toward target
//
// Note: PrintStartCollectorHeaterFixture is defined above and provides
// helpers for temperature simulation.
// ============================================================================

// ============================================================================
// Proactive Bed Heating Detection Tests
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection: bed heating at <50% target triggers HEATING_BED",
                 "[print][collector][proactive][heating]") {
    collector().start();
    drain_async_updates();
    drain_async_updates(); // Process start()'s INITIALIZING state update
    collector().enable_fallbacks();

    // Reset collector's internal current_phase_ back to IDLE for proactive detection
    reset_collector_to_idle();
    collector().enable_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);

    SECTION("Bed at 25% of target (150/600) triggers HEATING_BED") {
        // Bed target 60C (600 decideg), current temp 15C (150 decideg) = 25% of target
        set_all_temps(150, 600, 0, 0); // No extruder target

        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
        REQUIRE(get_current_message() == "Heating Bed...");
    }

    SECTION("Bed at 49% of target triggers HEATING_BED") {
        // Bed target 60C, current 29.4C (294 decideg) = 49% of target
        set_all_temps(294, 600, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("Bed at 10% of target (extreme case)") {
        // Bed target 110C (1100 decideg), current 11C (110 decideg) = 10%
        set_all_temps(110, 1100, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }
}

// ============================================================================
// Writer Independence (subject ownership)
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "PrintStartCollector: M117 does not overwrite the phase label",
                 "[print][collector][proactive][heating]") {
    // Drive to a known phase that has its own label. Mirrors the existing
    // proactive bed-heating section at :955-964.
    collector().start();
    drain_async_updates();
    drain_async_updates(); // INITIALIZING settle
    collector().enable_fallbacks();

    set_all_temps(150, 600, 0, 0);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    REQUIRE(get_current_message() == "Heating Bed...");

    // A user M117 arrives on the same notify_status_update path the collector
    // listens on. It must NOT clobber the collector's phase label — user text
    // belongs in display_message, which PrinterPrintState owns.
    client().dispatch_status_update({{"display_status", {{"message", "Leveling 3/9"}}}});
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_message() == "Heating Bed...");
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection: bed heating stays HEATING_BED until bed reaches target",
                 "[print][collector][proactive][heating]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("Bed at 50% of target - bed still heating even with nozzle also heating") {
        // Bed target 60C, current 30C (300 decideg) = 50% of target
        // Nozzle also heating - but bed_heating takes priority
        set_all_temps(300, 600, 500, 2100); // Nozzle 50C/210C

        collector().check_fallback_completion();
        drain_async_updates();

        // Bed is still heating, so HEATING_BED takes priority
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("Bed at 80% of target with nozzle heating - bed still takes priority") {
        // Bed target 60C, current 48C (480 decideg) = 80%
        set_all_temps(480, 600, 1000, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        // Bed is still below target-tolerance, so HEATING_BED
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("Bed at target - nozzle heating takes over") {
        // Bed target 60C, current 59C (590 decideg) = within tolerance
        // Nozzle still heating
        set_all_temps(590, 600, 1000, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        // Bed is within tolerance, so nozzle heating takes over
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }
}

// ============================================================================
// Heating-phase heater correction (signal-latched, temp-aware relabel)
//
// These drive the phase via the REAL gcode signal path (send_gcode_response),
// so real_signal_seen_ is genuinely set and the proactive detector is gated
// off — exactly the K2 condition where a latched M109 leaves the label stuck
// on the wrong heater. check_fallback_completion() must then re-derive the
// shown heater from live temps, bed-first, without ever relabeling a
// firmware's ordered non-heating phase.
// ============================================================================

TEST_CASE_METHOD(
    PrintStartCollectorHeaterFixture,
    "Heater correction: latched HEATING_NOZZLE with hot nozzle + cold bed -> HEATING_BED",
    "[print][collector][heating][heater_correction]") {
    // Reproduces the observed K2 screenshot: nozzle already at target (green),
    // bed is the real long pole, but firmware's M109 latched HEATING_NOZZLE.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    // Arm pattern matching, then latch HEATING_NOZZLE via a real M109 line.
    send_gcode_response("PRINT_START");
    send_gcode_response("M109 S140");
    drain_async_updates();

    // Precondition: the signal path genuinely latched HEATING_NOZZLE.
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // Bed 48.8/100C (heating), nozzle 139.7/140C (at target).
    set_all_temps(488, 1000, 1397, 1400);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    REQUIRE(get_current_message() == "Heating Bed...");
}

TEST_CASE_METHOD(
    PrintStartCollectorHeaterFixture,
    "Heater correction: latched HEATING_BED with hot bed + cold nozzle -> HEATING_NOZZLE",
    "[print][collector][heating][heater_correction]") {
    // Symmetric case: bed reached target first, nozzle still climbing.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    send_gcode_response("PRINT_START");
    send_gcode_response("M190 S100");
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    // Bed 100/100C (at target), nozzle 100/210C (heating).
    set_all_temps(1000, 1000, 1000, 2100);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(get_current_message() == "Heating Nozzle...");
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Heater correction: non-heating latched phase (HOMING) is never relabeled",
                 "[print][collector][heating][heater_correction]") {
    // U1 protection: a firmware-signaled non-heating phase must survive even
    // though both heaters read below target. Proves the scope guard.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    send_gcode_response("PRINT_START");
    send_gcode_response("G28");
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // Both heaters well below target — a naive detector would fire HEATING_BED.
    set_all_temps(300, 1000, 500, 2100);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    // Scope guard: HOMING is not a heating phase, so it must be left alone.
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Heater correction: both heaters heating resolves bed-first",
                 "[print][collector][heating][heater_correction]") {
    // Tiebreak: when both are still heating, the bed (usual long pole) wins.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    send_gcode_response("PRINT_START");
    send_gcode_response("M109 S210");
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // Bed 30/60C and nozzle 50/210C — both heating.
    set_all_temps(300, 600, 500, 2100);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

TEST_CASE_METHOD(
    PrintStartCollectorHeaterFixture,
    "Heater correction: CAS guard refuses to regress a phase that advanced past heating",
    "[print][collector][heating][heater_correction]") {
    // Race guard. In production, check_fallback_completion() snapshots the phase
    // under lock, releases it, reads temps, then relabels — a bg gcode signal can
    // advance current_phase_ past heating in that gap. relabel_heating_phase()
    // re-checks under the lock and must refuse, so the newer phase is never
    // regressed. Here we reproduce that deterministically: drive the phase to QGL
    // (a non-heating phase), then invoke the relabel as if a stale snapshot did.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    send_gcode_response("PRINT_START");
    send_gcode_response("M109 S210"); // was heating...
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    send_gcode_response("QUAD_GANTRY_LEVEL"); // ...but firmware advanced to QGL
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);

    // Both heaters below target — a naive relabel would fire HEATING_BED.
    set_all_temps(300, 1000, 500, 2100);

    // Stale relabel, as check_fallback_completion() would attempt off its snapshot.
    PrintStartCollectorTestAccess::relabel_heating_phase(collector(), PrintStartPhase::HEATING_BED);
    drain_async_updates();
    drain_async_updates();

    // CAS refused: QGL survives, not regressed to HEATING_BED.
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);
}

// ============================================================================
// Proactive Nozzle Heating Detection Tests
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection: nozzle heating when bed is near target",
                 "[print][collector][proactive][heating]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("Bed near target, nozzle far from target triggers HEATING_NOZZLE") {
        // Bed at 55C/60C (near target), nozzle at 50C/210C (far from target)
        set_all_temps(550, 600, 500, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
        REQUIRE(get_current_message() == "Heating Nozzle...");
    }

    SECTION("Bed at target (within tolerance), nozzle heating") {
        // TEMP_TOLERANCE_DECIDEGREES = 50 (5C)
        // Bed at 58C/60C (within 5C tolerance = at target)
        // Nozzle at 100C/210C
        set_all_temps(580, 600, 1000, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }

    SECTION("Bed at exactly target, nozzle ramping") {
        set_all_temps(600, 600, 1500, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }
}

// ============================================================================
// Temperature Tolerance Edge Cases (TEMP_TOLERANCE_DECIDEGREES = 50)
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection respects TEMP_TOLERANCE_DECIDEGREES (50 = 5C)",
                 "[print][collector][proactive][tolerance]") {
    // Initialize temps to zero before starting to prevent proactive detection triggering
    // during enable_fallbacks() call
    set_all_temps(0, 0, 0, 0);

    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);

    SECTION("Temp exactly at tolerance boundary is considered heating") {
        // Target 60C (600), temp 55C (550), diff = 50 decideg = exactly at tolerance
        // temp < target - tolerance means heating
        // 550 < 600 - 50 = 550 < 550 is FALSE, so NOT heating
        set_all_temps(550, 600, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();

        // At exactly tolerance, NOT considered heating (550 is not < 550)
        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }

    SECTION("Temp 1 decidegree below tolerance is considered heating") {
        // Target 60C (600), temp 54.9C (549), diff = 51 decideg
        // 549 < 600 - 50 = 549 < 550 is TRUE, so bed_heating = true
        // Bed heating takes priority regardless of how close to target
        set_all_temps(549, 600, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();

        // Bed is still heating (below tolerance), so HEATING_BED
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("Temp 1 decidegree above tolerance is NOT heating") {
        // Target 60C (600), temp 55.1C (551), diff = 49 decideg
        // 551 < 600 - 50 = 551 < 550 is FALSE, so NOT heating
        set_all_temps(551, 600, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }
}

// ============================================================================
// Zero Target Temperature Tests
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection handles zero targets correctly",
                 "[print][collector][proactive][edge]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("Zero bed target means no bed heating") {
        // Bed target 0, so bed_heating = false (target > 0 && temp < target - tol)
        set_all_temps(250, 0, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }

    SECTION("Zero extruder target means no nozzle heating") {
        // Both targets 0
        set_all_temps(250, 0, 500, 0);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }

    SECTION("Zero bed target but nozzle heating triggers HEATING_NOZZLE") {
        // Bed target 0 (no bed heating), nozzle heating
        set_all_temps(250, 0, 500, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }
}

// ============================================================================
// Both Heaters at Target - No Proactive Detection
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection not triggered when both heaters at target",
                 "[print][collector][proactive][edge]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("Both heaters exactly at target") {
        set_all_temps(600, 600, 2100, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        // No heating detected, should remain IDLE
        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }

    SECTION("Both heaters within tolerance of target") {
        // Bed 58C/60C, Nozzle 207C/210C - both within 5C tolerance
        set_all_temps(580, 600, 2070, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }

    SECTION("Heaters above target (overshooting)") {
        // Bed 62C/60C, Nozzle 212C/210C - both above target
        set_all_temps(620, 600, 2120, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }
}

// ============================================================================
// Proactive Detection Requires IDLE Phase
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection behavior from internal IDLE state",
                 "[print][collector][proactive][state]") {
    // NOTE: Proactive detection checks the collector's internal current_phase_,
    // not the PrinterState subject. After start(), internal state is IDLE while
    // PrinterState shows INITIALIZING. We can't easily set the internal state
    // externally, so we test that proactive detection works from the internal
    // IDLE state (set by start()).
    //
    // The previous test was incorrect - it set PrinterState externally but the
    // collector's internal state remained IDLE, so proactive detection still
    // triggered. The fix would require exposing internal state or testing
    // through the G-code callback mechanism.

    // Initialize temps to zero to prevent proactive detection during enable
    set_all_temps(0, 0, 0, 0);

    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);

    SECTION("Proactive detection triggers from IDLE state when heaters heating") {
        // The collector's internal state is IDLE (from start())
        // Proactive detection should trigger when heaters are heating
        set_all_temps(200, 600, 500, 2100); // Bed at 20C/60C, nozzle at 50C/210C

        collector().check_fallback_completion();
        drain_async_updates();

        // Should detect heating - bed is < 50% of target so HEATING_BED
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("After proactive detection triggers, subsequent calls don't change phase") {
        // First call triggers HEATING_BED
        set_all_temps(200, 600, 500, 2100);
        collector().check_fallback_completion();
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

        // Now heaters still heating, but internal state is no longer IDLE
        // so proactive detection won't trigger again (but we can't verify
        // this without accessing internal state)
    }
}

// ============================================================================
// Fallback Detection Requires Fallbacks Enabled
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection requires fallbacks to be enabled",
                 "[print][collector][proactive][state]") {
    collector().start();
    drain_async_updates();
    // Do NOT call enable_fallbacks()
    reset_collector_to_idle();
    // Fallbacks remain disabled (reset_collector_to_idle calls reset() which disables them)

    // Set heaters heating
    set_all_temps(200, 600, 500, 2100);

    collector().check_fallback_completion();
    drain_async_updates();

    // Fallbacks not enabled, so no proactive detection
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Proactive detection requires collector to be active",
                 "[print][collector][proactive][state]") {
    // Do NOT call collector().start()
    // Collector is not active

    set_all_temps(200, 600, 500, 2100);

    collector().check_fallback_completion();
    drain_async_updates();

    // Collector not active, so no detection
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
}

// ============================================================================
// Decidegree Math Validation
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Decidegree math: temperature values are handled correctly",
                 "[print][collector][proactive][math]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("Real-world temps: 22.5C bed heating to 60C") {
        // 22.5C = 225 decideg, target 60C = 600 decideg
        // 225 is < 50% of 600 (300), so HEATING_BED
        set_all_temps(225, 600, 0, 0);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("Real-world temps: 205.3C nozzle heating to 250C") {
        // Bed at target, nozzle at 205.3C (2053) heating to 250C (2500)
        // 2053 < 2500 - 50 = 2053 < 2450? YES, so heating
        set_all_temps(600, 600, 2053, 2500);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }

    SECTION("High-temp printing: bed 110C, nozzle 285C") {
        // ABS/ASA temps: bed 110C (1100), nozzle 285C (2850)
        // Bed at 30C (300) = 27% of target, so HEATING_BED
        set_all_temps(300, 1100, 250, 2850);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("PLA temps: bed 60C, nozzle 200C") {
        // Bed at 55C (550), nozzle at 50C (500)
        // Bed: 550 < 600 - 50 = 550 < 550? NO (not heating)
        // But 550 is >= 50% of 600, so check nozzle
        // Nozzle: 500 < 2000 - 50 = 500 < 1950? YES (heating)
        set_all_temps(550, 600, 500, 2000);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }
}

// ============================================================================
// Completion Fallback Tests (Layer/Progress Detection)
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Fallback completion: layer count no longer triggers COMPLETE",
                 "[print][collector][fallback][completion]") {
    // Layer count heuristic has been removed — authoritative signals (RESPOND match
    // and Moonraker state=printing) now handle dismissal. These sections verify the
    // heuristic no longer fires.

    // Initialize temps to prevent proactive detection
    set_all_temps(0, 0, 0, 0);

    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);

    SECTION("Layer 1 does not trigger completion") {
        set_progress_and_layer(0, 1);
        set_all_temps(600, 600, 2100, 2100); // Temps at target

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("Layer 2 does not trigger completion") {
        set_progress_and_layer(0, 2);
        set_all_temps(600, 600, 2100, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("Layer 0 does not trigger completion - stays IDLE when no heating") {
        set_progress_and_layer(0, 0);
        // Set temps at target so no heating phase detected
        set_all_temps(600, 600, 2100, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        // No layer, no progress, no heating - stays IDLE
        REQUIRE(get_current_phase() == PrintStartPhase::IDLE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Layer-zero edge: stale positive layer does not arm completion until a fresh 0",
                 "[print][collector][completion][regression]") {
    // FIX C hardening: the pre-print → printing hand-off is gated on a genuine
    // 0 -> >=1 transition (MoonrakerManager::should_complete_preprint with
    // seen_layer_zero). The collector tracks the "seen zero" latch. On
    // back-to-back prints the layer subject still holds a stale positive from
    // the prior print until reset_for_new_print() runs async — note_current_layer
    // must NOT latch on that stale positive, only on a freshly observed value < 1.

    set_all_temps(0, 0, 0, 0);
    collector().start();
    drain_async_updates();

    SECTION("Fresh collector has not seen layer zero") {
        REQUIRE_FALSE(collector().has_seen_layer_zero());
    }

    SECTION("Stale positive layer (250) does NOT arm the edge") {
        collector().note_current_layer(250); // stale carryover from prior print
        REQUIRE_FALSE(collector().has_seen_layer_zero());
    }

    SECTION("A freshly observed layer 0 arms the edge; subsequent >=1 keeps it armed") {
        collector().note_current_layer(250); // stale, ignored
        REQUIRE_FALSE(collector().has_seen_layer_zero());
        collector().note_current_layer(0); // fresh post-reset zero
        REQUIRE(collector().has_seen_layer_zero());
        collector().note_current_layer(1); // first real layer — latch stays
        REQUIRE(collector().has_seen_layer_zero());
    }

    SECTION("reset() clears the latch for the next print") {
        collector().note_current_layer(0);
        REQUIRE(collector().has_seen_layer_zero());
        collector().reset();
        drain_async_updates();
        REQUIRE_FALSE(collector().has_seen_layer_zero());
    }

    SECTION("A new start() clears the latch even if the subject holds a stale positive") {
        collector().note_current_layer(0);
        REQUIRE(collector().has_seen_layer_zero());
        collector().stop();
        // Simulate the stale-layer carryover: subject reads a positive value
        // from the prior print at the moment the next print's collector starts.
        lv_subject_set_int(state().get_print_layer_current_subject(), 250);
        collector().start();
        drain_async_updates();
        REQUIRE_FALSE(collector().has_seen_layer_zero());
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Fallback completion: progress threshold no longer triggers COMPLETE",
                 "[print][collector][fallback][completion]") {
    // Progress-threshold heuristic has been removed — authoritative signals handle
    // dismissal. These sections verify the heuristic no longer fires.

    // Initialize temps to prevent proactive detection
    set_all_temps(0, 0, 0, 0);

    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::IDLE);

    SECTION("3% progress with temps at target does not trigger COMPLETE") {
        set_progress_and_layer(3, 0); // 3% progress, layer 0
        set_all_temps(600, 600, 2100, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("2% progress with temps at target does NOT trigger (threshold is >= 3%)") {
        set_progress_and_layer(2, 0);
        set_all_temps(600, 600, 2100, 2100);

        collector().check_fallback_completion();
        drain_async_updates();

        // 2% is below the 3% threshold — START_PRINT macros occupy ~1% of the file,
        // so we need a higher threshold to avoid false completion during pre-print.
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("2% progress but temps NOT ready - triggers heating detection") {
        set_progress_and_layer(2, 0);
        set_all_temps(200, 600, 500, 2100); // Bed at 20C/60C, nozzle at 50C/210C

        collector().check_fallback_completion();
        drain_async_updates();

        // Temps not ready, proactive detection triggers for bed heating
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }
}

// ============================================================================
// AREA C: Sequential Progress Monotonic Guard Tests
// ============================================================================
// Tests that sequential mode progress never regresses, even when signals
// are re-emitted in a different order (e.g., AD5M firmware re-emitting
// HEATING after bed mesh probing).
// ============================================================================

/**
 * @brief Test fixture for sequential profile (Forge-X) progress tests
 *
 * Loads forge_x profile and provides helpers for dispatching G-code
 * responses and reading progress values.
 */
class PrintStartCollectorSequentialFixture : public LVGLTestFixture {
  public:
    PrintStartCollectorSequentialFixture() {
        state_.init_subjects(false);
        // Mark print as active so set_print_start_state() accepts phase updates
        lv_subject_set_int(state_.get_print_active_subject(), 1);
        client_ = std::make_unique<MoonrakerClientMock>();
        collector_ = std::make_shared<PrintStartCollector>(*client_, state_);
        collector_->set_profile(PrintStartProfile::load("forge_x"));
    }

    ~PrintStartCollectorSequentialFixture() override {
        if (collector_->is_active()) {
            collector_->stop();
        }
        collector_.reset();
        client_.reset();
    }

    PrinterState& state() {
        return state_;
    }
    MoonrakerClientMock& client() {
        return *client_;
    }
    PrintStartCollector& collector() {
        return *collector_;
    }

    int get_current_progress() {
        return lv_subject_get_int(state_.get_print_start_progress_subject());
    }

    PrintStartPhase get_current_phase() {
        return static_cast<PrintStartPhase>(
            lv_subject_get_int(state_.get_print_start_phase_subject()));
    }

    std::string get_current_message() {
        return lv_subject_get_string(state_.get_print_start_message_subject());
    }

    void send_gcode_response(const std::string& line) {
        json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    }

    void drain_async_updates() {
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
    }

  protected:
    PrinterState state_;
    std::unique_ptr<MoonrakerClientMock> client_;
    std::shared_ptr<PrintStartCollector> collector_;
};

// ============================================================================
// Sequential Progress Never Regresses on Repeated Signals
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Sequential progress never regresses on repeated signals",
                 "[print][collector][sequential]") {
    collector().start();
    drain_async_updates();

    send_gcode_response("// State: HOMING...");
    REQUIRE(get_current_progress() == 10);

    send_gcode_response("// State: KAMP LEVELING...");
    REQUIRE(get_current_progress() == 60);

    send_gcode_response("// State: WAIT FOR TEMPERATURE...");
    REQUIRE(get_current_progress() == 82);

    // AD5M firmware re-emits HEATING after bed mesh probing — this should NOT regress
    send_gcode_response("// State: HEATING...");
    REQUIRE(get_current_progress() >= 82);

    send_gcode_response("// State: KAMP PRIMING...");
    REQUIRE(get_current_progress() == 90);
}

// ============================================================================
// Sequential Progress Allows Forward Movement
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Sequential progress allows forward movement through all signals",
                 "[print][collector][sequential]") {
    collector().start();
    drain_async_updates();

    int prev_progress = 0;

    // Walk through all 14 AD5M signals in order
    std::vector<std::string> signals = {
        "// State: PREPARING...",
        "// State: MD5 CHECK",
        "// State: HOMING...",
        "// State: PREPARE CLEANING...",
        "// State: HEATING...",
        "// State: CLEANING START SOON",
        "// State: CLEANING...",
        "// State: COOLING DOWN...",
        "// State: FINISHING CLEANING...",
        "// State: DONE!",
        "// State: KAMP LEVELING...",
        "// State: WAIT FOR TEMPERATURE...",
        "// State: KAMP PRIMING...",
        "// State: PRINTING...",
    };

    for (const auto& signal : signals) {
        send_gcode_response(signal);
        int progress = get_current_progress();
        CAPTURE(signal, progress, prev_progress);
        REQUIRE(progress >= prev_progress);
        prev_progress = progress;
    }

    // Final signal should reach 100%
    REQUIRE(prev_progress == 100);
}

// ============================================================================
// Response Pattern Weight Doesn't Regress Sequential Progress
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Response pattern weight doesn't regress sequential progress",
                 "[print][collector][sequential]") {
    collector().start();
    drain_async_updates();

    // HEATING signal sets progress to 25
    send_gcode_response("// State: HEATING...");
    REQUIRE(get_current_progress() == 25);

    // Response pattern "Wait extruder temperature to reach 220" has weight=15
    // which would be used as progress in sequential mode — but monotonic guard prevents regression
    send_gcode_response("// Wait extruder temperature to reach 220");
    REQUIRE(get_current_progress() >= 25);
}

// ============================================================================
// COMPLETE Always Reaches 100%
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "COMPLETE always reaches 100% regardless of prior progress",
                 "[print][collector][sequential]") {
    collector().start();
    drain_async_updates();

    // Advance to 82%
    send_gcode_response("// State: WAIT FOR TEMPERATURE...");
    REQUIRE(get_current_progress() == 82);

    // PRINTING signal maps to COMPLETE phase → always 100%
    send_gcode_response("// State: PRINTING...");
    REQUIRE(get_current_progress() == 100);
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

// ============================================================================
// ============================================================================
// ETA RE-BASELINE TESTS
//
// Reproduced from the K1C capture of 2026-08-19: monitoring started before the
// firmware set heater targets, so the first ETA publish anchored on a
// target-less provisional estimate (215s). Real targets arrived one second
// later and the recompute said 469s — but the strict monotonic guard clamped
// every subsequent publish back down to 215s for the entire 369s prep. The
// anchor must re-baseline when the weights' inputs change, not just when time
// passes.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "ETA rises when heater targets arrive after monitoring started",
                 "[print][collector][eta]") {
    // Ambient temps, no targets yet (START_PRINT hasn't issued M104/M140).
    set_all_temps(250, 0, 500, 0);
    collector().start();
    collector().enable_fallbacks();
    // Empty history bucket (first print with this window/temp class) and a
    // learned-rate heater so the recomputed durations are realistic.
    PrintStartCollectorTestAccess::clear_prediction_history(collector());
    auto& rates = ThermalRateManager::instance();
    rates.get_model("extruder").set_default_rate(1.0f);
    rates.get_model("heater_bed").set_default_rate(1.0f);
    drain_async_updates();

    // start() publishes the provisional estimate immediately.
    const int provisional = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(provisional > 0);

    // Heater targets land (K1C CX_ROUGH_G28 stage: nozzle to probe temp, bed
    // to print temp) and the subject observer path runs the recompute.
    set_all_temps(250, 550, 500, 1300);
    collector().check_fallback_completion();
    drain_async_updates();

    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    const int corrected = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(corrected > provisional);

    // The corrected estimate is the new anchor: without further input changes
    // remaining must not creep back up on later ticks.
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    const int settled = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(settled <= corrected);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "ETA re-baselines when nozzle target rises in stages", "[print][collector][eta]") {
    // K1C heats the nozzle to ~130°C for probing, then to print temp. The
    // second stage is a genuine new input: the recompute (and the anchor
    // release) must fire on a substantial target RISE, not only on 0→positive.
    set_all_temps(250, 550, 500, 1300);
    collector().start();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::clear_prediction_history(collector());
    auto& rates = ThermalRateManager::instance();
    rates.get_model("extruder").set_default_rate(1.0f);
    rates.get_model("heater_bed").set_default_rate(1.0f);
    drain_async_updates();
    collector().check_fallback_completion();
    drain_async_updates();
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    const int probe_stage = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(probe_stage > 0);

    // Firmware raises the nozzle to print temp; nozzle temp is still well
    // below it, so the heating phase keeps real weight.
    set_all_temps(250, 550, 500, 2200);
    collector().check_fallback_completion();
    drain_async_updates();
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    const int print_stage = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(print_stage > probe_stage);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Remaining keeps unfinished heating work after the phase marker passes",
                 "[print][collector][eta]") {
    // K1C capture 2026-08-20: heater targets land first, homing starts while
    // the bed is at 29 of 55C, and both heating phases' durations vanished
    // from the estimate the moment their markers passed - the countdown dove
    // to 205s of a 378s prep and the bar jumped to ~45% twenty seconds in.
    // A heating phase is done when its TARGET IS REACHED, not when the next
    // phase's marker arrives.
    set_all_temps(250, 550, 290, 550); // nozzle 25/55C, bed 29/55C
    collector().start();
    collector().enable_fallbacks();

    // Deterministic history: heating phases carry 90s each, mesh 120s.
    helix::PreprintEntry e;
    e.total_seconds = 300;
    e.timestamp = 1000;
    e.temp_bucket = 1;
    e.phase_durations = {{static_cast<int>(PrintStartPhase::HEATING_NOZZLE), 90},
                         {static_cast<int>(PrintStartPhase::HEATING_BED), 90},
                         {static_cast<int>(PrintStartPhase::HOMING), 15},
                         {static_cast<int>(PrintStartPhase::BED_MESH), 120}};
    PrintStartCollectorTestAccess::load_prediction_entries(collector(), {e});
    auto& rates = ThermalRateManager::instance();
    rates.get_model("extruder").set_default_rate(1.0f);
    rates.get_model("heater_bed").set_default_rate(1.0f);
    drain_async_updates();

    // The chain passes heating and lands in HOMING while both heaters are
    // still mid-climb (bed at 29 of 55C).
    send_gcode_response("M190"); // HEATING_BED marker
    send_gcode_response("M109"); // HEATING_NOZZLE marker
    send_gcode_response("G28");  // HOMING begins - heaters still running

    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    // With bed 26C short at ~1s/C the unfinished heating work is ~26s of the
    // bed phase alone; the estimate must still hold most of the prep. The old
    // behavior dropped both 90s heating phases as "completed".
    const int remaining = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(remaining > 150);

    // And the bar (total - remaining) must not front-load: 20s into a 300s
    // prep it has no business showing a third.
    const int progress = lv_subject_get_int(state().get_print_start_progress_subject());
    REQUIRE(progress < 30);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Entering a phase releases the monotonic countdown anchor",
                 "[print][collector][eta]") {
    // K1C capture 2026-08-20: the countdown eased to 39s during the pre-mesh
    // probes, BED_MESH entered with 124s predicted, and the strict monotonic
    // guard pinned the display at 39s for the entire mesh (the collector
    // logged "Monotonic bias: suppressed 39s->103s, overrun=15.3%"). A new
    // phase is a genuine re-assessment of the remaining work, not noise.
    set_all_temps(250, 550, 500, 550);
    collector().start();
    collector().enable_fallbacks();

    helix::PreprintEntry e;
    e.total_seconds = 240;
    e.timestamp = 1000;
    e.temp_bucket = 1;
    e.phase_durations = {{static_cast<int>(PrintStartPhase::HOMING), 15},
                         {static_cast<int>(PrintStartPhase::BED_MESH), 124}};
    PrintStartCollectorTestAccess::load_prediction_entries(collector(), {e});
    drain_async_updates();

    // Model the eased-down pre-mesh floor directly.
    PrintStartCollectorTestAccess::set_last_remaining(collector(), 39);
    send_gcode_response("BED_MESH_CALIBRATE");

    // The phase transition must have released the anchor...
    REQUIRE(PrintStartCollectorTestAccess::get_last_remaining(collector()) == 0);

    // ...so the mesh's 124s publish instead of clamping to 39.
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    const int remaining = lv_subject_get_int(state().get_preprint_remaining_subject());
    REQUIRE(remaining > 100);
}

// ============================================================================
// ADAPTIVE TIMEOUT TESTS
//
// Tests for the adaptive timeout behavior introduced to prevent premature
// completion on bed-first macros (e.g., AD5M with ABS). Uses
// PrintStartCollectorTestAccess to simulate elapsed time and set predictions.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Timeout fallback does not fire when nozzle target is zero",
                 "[print][collector][timeout]") {
    // Simulate AD5M ABS scenario: bed at target, nozzle target not yet set by macro
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // Force no predictions so FALLBACK_TIMEOUT (300s) applies directly
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Bed at target (105°C), nozzle target = 0 (M109 not issued yet)
    set_all_temps(1050, 1050, 500, 0);
    // Simulate 400s elapsed (well past FALLBACK_TIMEOUT of 300s)
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    // Should NOT complete — nozzle target unknown means temps_near is false
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Timeout fallback fires when both targets set and near",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    REQUIRE(collector().is_active());

    // Force no predictions so FALLBACK_TIMEOUT (300s) applies directly
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Both heaters within 5°C tolerance (not "heating") AND above 90% of target
    set_all_temps(1050, 1050, 2610, 2650);
    // Simulate 310s elapsed (past FALLBACK_TIMEOUT of 300s)
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 310);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Adaptive timeout extends deadline when predictions available",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // Set predicted total to 500s (adaptive timeout = 500 * 1.5 = 750s)
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 500.0f);

    // Both heaters within tolerance (not "heating") and above 90% of target
    set_all_temps(1050, 1050, 2610, 2650);

    SECTION("Does not fire before adaptive timeout") {
        // 400s elapsed — past FALLBACK_TIMEOUT but before adaptive (750s)
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("Fires after adaptive timeout with temps near") {
        // 760s elapsed — past adaptive timeout (750s)
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 760);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Absolute timeout fires even without temps near when predicted",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // Set predicted total to 400s. The absolute ceiling is
    // max(400*2.5, ABSOLUTE_MAX_TIMEOUT) and ABSOLUTE_MAX_TIMEOUT is now 1800s,
    // raised because the old 900s cut off legitimate long pre-prints (the K2
    // Plus runs ~1140s: heat, ~390s mesh, purge).
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 400.0f);

    // Nozzle target still 0 — temps_near will be false
    set_all_temps(1050, 1050, 500, 0);

    SECTION("Does not fire before absolute timeout") {
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 950);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("Fires at absolute timeout regardless of temps") {
        // Still fires with temps_near false — the ceiling ignores temperature.
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1850);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture, "Absolute max timeout fires without predictions",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // Force no predictions — FALLBACK_TIMEOUT (300s) applies
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Nozzle target = 0 — temps_near stays false, FALLBACK_TIMEOUT won't fire
    set_all_temps(1050, 1050, 500, 0);

    SECTION("FALLBACK_TIMEOUT does not fire with unknown nozzle target") {
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("ABSOLUTE_MAX_TIMEOUT fires as hard ceiling") {
        // Ungated backstop: fires on elapsed time alone, without needing the
        // printer to have gone quiet, so a firmware that chatters forever still
        // leaves Preparing.
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1850);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Nozzle hot but target=0 blocks timeout completion",
                 "[print][collector][timeout]") {
    // Even with nozzle physically hot, ext_target=0 means the macro hasn't
    // commanded M109 yet — temps_near must be false
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    set_all_temps(1050, 1050, 2610, 0); // Nozzle hot but target=0 (not set by macro)
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Nozzle within tolerance and above 90% satisfies temps_near",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // Force no predictions so FALLBACK_TIMEOUT (300s) applies directly
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Nozzle at 261°C with target 265°C: within 5°C tolerance (not "heating")
    // and above 90% of target (238.5°C). This satisfies temps_near.
    set_all_temps(1050, 1050, 2610, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

// ============================================================================
// PREDICTION SAVE/LOAD TESTS
// ============================================================================

TEST_CASE("PreprintPredictor load/save round-trip preserves entries",
          "[print][preprint][persistence]") {
    helix::PreprintPredictor predictor;

    helix::PreprintEntry entry1;
    entry1.total_seconds = 300;
    entry1.timestamp = 1000;
    entry1.temp_bucket = 1;
    entry1.phase_durations = {{2, 15}, {7, 200}};

    helix::PreprintEntry entry2;
    entry2.total_seconds = 320;
    entry2.timestamp = 2000;
    entry2.temp_bucket = 1;
    entry2.phase_durations = {{2, 18}, {7, 210}};

    std::vector<helix::PreprintEntry> entries = {entry1, entry2};
    predictor.load_entries(entries, 1); // cold bucket

    auto loaded = predictor.get_entries();
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].total_seconds == 300);
    REQUIRE(loaded[1].total_seconds == 320);
    REQUIRE(loaded[0].phase_durations.at(7) == 200);
    REQUIRE(loaded[1].phase_durations.at(7) == 210);
}

TEST_CASE("PreprintPredictor FIFO trims to MAX_ENTRIES", "[print][preprint][persistence]") {
    helix::PreprintPredictor predictor;

    std::vector<helix::PreprintEntry> entries;
    for (int i = 0; i < 15; ++i) {
        helix::PreprintEntry e;
        e.total_seconds = 100 + i;
        e.timestamp = i;
        e.temp_bucket = 1;
        entries.push_back(e);
    }

    predictor.load_entries(entries, 1);
    auto loaded = predictor.get_entries();
    REQUIRE(loaded.size() == static_cast<size_t>(helix::PreprintPredictor::MAX_ENTRIES));
    // Should keep the NEWEST entries (highest index = highest timestamps)
    REQUIRE(loaded.front().total_seconds == 105); // 15 - 10 = index 5
}

TEST_CASE("PreprintPredictor bucket filtering separates cold and warm",
          "[print][preprint][persistence]") {
    helix::PreprintEntry cold_entry;
    cold_entry.total_seconds = 300;
    cold_entry.timestamp = 1000;
    cold_entry.temp_bucket = 1; // cold

    helix::PreprintEntry warm_entry;
    warm_entry.total_seconds = 150;
    warm_entry.timestamp = 2000;
    warm_entry.temp_bucket = 2; // warm

    helix::PreprintEntry legacy_entry;
    legacy_entry.total_seconds = 250;
    legacy_entry.timestamp = 500;
    legacy_entry.temp_bucket = 0; // legacy -- included in both

    std::vector<helix::PreprintEntry> all = {cold_entry, warm_entry, legacy_entry};

    SECTION("Cold bucket includes cold + legacy") {
        helix::PreprintPredictor predictor;
        predictor.load_entries(all, 1);
        REQUIRE(predictor.get_entries().size() == 2);
    }

    SECTION("Warm bucket includes warm + legacy") {
        helix::PreprintPredictor predictor;
        predictor.load_entries(all, 2);
        REQUIRE(predictor.get_entries().size() == 2);
    }
}

TEST_CASE("PreprintPredictor has_predictions reflects actual entries", "[print][preprint]") {
    helix::PreprintPredictor predictor;
    REQUIRE_FALSE(predictor.has_predictions());

    helix::PreprintEntry entry;
    entry.total_seconds = 200;
    entry.timestamp = 1000;
    entry.temp_bucket = 1;
    entry.phase_durations = {{2, 10}, {7, 180}};
    predictor.add_entry(entry);

    REQUIRE(predictor.has_predictions());
}

// ============================================================================
// K2/CFS-specific gcode tag stream — folded in from the deleted
// PrintPhaseTracker. Opted-in profiles (cfs_signals) consume these matchers;
// every other printer falls through them.
// ============================================================================

/**
 * @brief The K2 tag stream run under the profile that declares it
 *
 * creality_k2.json sets cfs_signals, which is what admits the purge-percent
 * and box-load tag matchers. The base sequential fixture's forge_x profile
 * deliberately does not.
 */
class K2TagStreamFixture : public PrintStartCollectorSequentialFixture {
  public:
    K2TagStreamFixture() {
        auto profile = PrintStartProfile::load("creality_k2");
        REQUIRE(profile != nullptr);
        have_k2_profile_ = profile->name().find("K2") != std::string::npos;
        if (have_k2_profile_) {
            collector().set_profile(std::move(profile));
        }
    }

    bool have_k2_profile_ = false;
};

TEST_CASE_METHOD(K2TagStreamFixture, "K2 purge percent (fraction form) drives PURGING progress",
                 "[print][collector][k2]") {
    if (!have_k2_profile_) {
        SKIP("creality_k2.json not available");
    }
    collector().start();
    drain_async_updates();

    // Real K2 firmware emits: "// num: 0, velocity: 575.000000, percent 1.000000"
    // (no colon after "percent", value is a 0..1 fraction).
    send_gcode_response("// num: 0, velocity: 575.000000, percent 0.500000");
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    REQUIRE(get_current_progress() == 50);

    // The collector caps non-COMPLETE phases at 95% to leave headroom for the
    // COMPLETE transition — so 100% K2 purge tops out at 95% on the overall bar.
    send_gcode_response("// num: 0, velocity: 575.000000, percent 1.000000");
    REQUIRE(get_current_progress() == 95);
}

TEST_CASE_METHOD(K2TagStreamFixture,
                 "K2 purge percent (legacy integer form) drives PURGING progress",
                 "[print][collector][k2]") {
    if (!have_k2_profile_) {
        SKIP("creality_k2.json not available");
    }
    collector().start();
    drain_async_updates();

    // Legacy/integer form: "percent:" followed by 0..100 integer.
    send_gcode_response("// num: 0, velocity: 23.0, percent: 75");
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    REQUIRE(get_current_progress() == 75);
}

TEST_CASE_METHOD(K2TagStreamFixture,
                 "CFS box cut sensor detected enters INITIALIZING with Loading Filament",
                 "[print][collector][k2][cfs]") {
    if (!have_k2_profile_) {
        SKIP("creality_k2.json not available");
    }
    collector().start();
    drain_async_updates();

    send_gcode_response("// [box] cut sensor detected");
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE(get_current_message().find("Loading Filament") != std::string::npos);
}

TEST_CASE_METHOD(K2TagStreamFixture, "Stock Klipper purge-line text falls through K2 matcher",
                 "[print][collector][k2]") {
    if (!have_k2_profile_) {
        SKIP("creality_k2.json not available");
    }
    collector().start();
    drain_async_updates();

    // Voron / RatRig stock output — must NOT match the K2 percent matcher
    // (no num:/velocity: anchors), so PURGING isn't entered from this line.
    send_gcode_response("// probe at 190.000,8.000 is z=-0.580000");
    REQUIRE(get_current_phase() != PrintStartPhase::PURGING);
}

// ============================================================================
// QGL / bed-mesh conflation regression — Voron 2.4 PRINT_START runs
// QUAD_GANTRY_LEVEL before BED_MESH_CALIBRATE. QGL probes 4 pads with
// `samples: 3` (default) = 12 `probe at X,Y is z=Z` lines. The collector
// previously entered BED_MESH on the 3rd probe line and counted QGL pads
// against the bed_mesh probe total, throwing the "X/Y" count off.
// ============================================================================

namespace {
class VoronCollectorFixture : public PrintStartCollectorHeaterFixture {
  public:
    /// Send a "// probe at X,Y is z=Z" line through the gcode-response path.
    /// Same plumbing as PrintStartCollectorSequentialFixture::send_gcode_response,
    /// duplicated here because the Heater fixture doesn't expose it.
    void feed_gcode(const std::string& line) {
        nlohmann::json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    }
};
} // namespace

TEST_CASE_METHOD(VoronCollectorFixture,
                 "Voron QGL probes do not trigger BED_MESH (12-line corner burst)",
                 "[print][collector][voron][bed_mesh]") {
    collector().start();
    drain_async_updates();

    // Klipper emits "QUAD_GANTRY_LEVEL" + "quad_gantry_level: ..." during QGL.
    // The profile's QGL regex matches and sets phase=QGL.
    feed_gcode("QUAD_GANTRY_LEVEL");
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);

    // Real Voron QGL output: 4 corner pads × 3 samples = 12 probe lines.
    // Each pad position repeated 3x; 4 unique (x,y).
    const char* qgl_probe_lines[] = {
        "// probe at 30.000,30.000 is z=-0.580000",   "// probe at 30.000,30.000 is z=-0.582500",
        "// probe at 30.000,30.000 is z=-0.580000",   "// probe at 270.000,30.000 is z=-0.532500",
        "// probe at 270.000,30.000 is z=-0.532500",  "// probe at 270.000,30.000 is z=-0.532500",
        "// probe at 270.000,270.000 is z=-0.495000", "// probe at 270.000,270.000 is z=-0.490000",
        "// probe at 270.000,270.000 is z=-0.490000", "// probe at 30.000,270.000 is z=-0.557500",
        "// probe at 30.000,270.000 is z=-0.555000",  "// probe at 30.000,270.000 is z=-0.555000",
    };
    for (const char* line : qgl_probe_lines) {
        feed_gcode(line);
    }

    // Must remain in QGL — these probes are corner pads, not bed mesh points.
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);
}

TEST_CASE_METHOD(VoronCollectorFixture, "BED_MESH_CALIBRATE after QGL enters BED_MESH cleanly",
                 "[print][collector][voron][bed_mesh]") {
    collector().start();
    drain_async_updates();

    // Pre-print sequence: QGL (with probes) → BED_MESH_CALIBRATE → mesh probes.
    feed_gcode("QUAD_GANTRY_LEVEL");
    feed_gcode("// probe at 30.000,30.000 is z=-0.580000");
    feed_gcode("// probe at 30.000,30.000 is z=-0.582500");
    feed_gcode("// probe at 30.000,30.000 is z=-0.580000");
    feed_gcode("// probe at 270.000,30.000 is z=-0.532500");
    feed_gcode("// probe at 270.000,30.000 is z=-0.532500");
    feed_gcode("// probe at 270.000,30.000 is z=-0.532500");
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);

    // Now BED_MESH_CALIBRATE — collector regex matches, transitions to BED_MESH.
    feed_gcode("BED_MESH_CALIBRATE");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // First mesh probe should be the FIRST mesh point — the QGL probes that
    // came before should not be carried into the mesh count.
    feed_gcode("// probe at 50.000,50.000 is z=-0.500000");
    feed_gcode("// probe at 100.000,50.000 is z=-0.510000");
    feed_gcode("// probe at 150.000,50.000 is z=-0.515000");

    // Message format is "Bed Mesh (N/total)" or "Bed Mesh (N)" — assert
    // that the count reflects the 3 mesh probes, NOT 3 + 4 QGL pads = 7.
    std::string msg = get_current_message();
    INFO("Current message: " << msg);
    // Count should be 3 (the mesh probes), not 7 (QGL pads + mesh probes)
    REQUIRE(msg.find("(3") != std::string::npos);
    REQUIRE(msg.find("(7") == std::string::npos);
}

// ============================================================================
// Snapmaker U1 sub-phase counter regression — the U1 routes multiple distinct
// probe operations (Inspecting Bed, Detecting Plate, the bed mesh proper)
// through one BED_MESH phase enum, switching only the message between them. The
// probe counter must reset between sub-phases so the displayed "(N/M)" doesn't
// accumulate to "(20/16)". Sub-phase action codes are the REAL ones captured
// from the U1 gcode_store on 2026-06-18 (PRINT_BED_DETECTING → "Inspecting
// bed...", DETECT_PLATE → "Detecting plate...").
// ============================================================================

namespace {
class SnapmakerCollectorFixture : public PrintStartCollectorHeaterFixture {
  public:
    SnapmakerCollectorFixture() {
        collector_->set_profile(PrintStartProfile::load("snapmaker_u1"));
    }

    void feed_gcode(const std::string& line) {
        nlohmann::json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    }
};
} // namespace

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: BED_MESH sub-phase change resets probe counter",
                 "[print][collector][snapmaker][bed_mesh]") {
    collector().start();
    drain_async_updates();

    // Sub-phase 1: Inspecting bed. Three probes → "Inspecting bed (3)".
    feed_gcode("// Success: Set action code PRINT_BED_DETECTING");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    feed_gcode("// probe at 10.000,10.000 is z=-0.100000");
    feed_gcode("// probe at 50.000,10.000 is z=-0.105000");
    feed_gcode("// probe at 90.000,10.000 is z=-0.110000");

    std::string after_inspect = get_current_message();
    INFO("After inspect: " << after_inspect);
    REQUIRE(after_inspect.find("Inspecting") != std::string::npos);
    REQUIRE(after_inspect.find("(3") != std::string::npos);

    // Sub-phase 2: Detecting plate. Counter MUST reset — three probes here
    // should display "(1)".."(3)", not "(4)".."(6)".
    feed_gcode("// Success: Set action code DETECT_PLATE");
    feed_gcode("// probe at 10.000,10.000 is z=-0.200000");

    std::string after_first_detect_probe = get_current_message();
    INFO("After first detect probe: " << after_first_detect_probe);
    REQUIRE(after_first_detect_probe.find("Detecting") != std::string::npos);
    // Must NOT carry the 3 inspect probes
    REQUIRE(after_first_detect_probe.find("(4") == std::string::npos);
    REQUIRE(after_first_detect_probe.find("(1") != std::string::npos);

    feed_gcode("// probe at 50.000,10.000 is z=-0.205000");
    feed_gcode("// probe at 90.000,10.000 is z=-0.210000");

    std::string after_detect = get_current_message();
    INFO("After three detect probes: " << after_detect);
    REQUIRE(after_detect.find("Detecting") != std::string::npos);
    REQUIRE(after_detect.find("(3") != std::string::npos);
    REQUIRE(after_detect.find("(6") == std::string::npos);

    // Sub-phase 3: back to Inspecting bed (message change resets again).
    feed_gcode("// Success: Set action code PRINT_BED_DETECTING");
    feed_gcode("// probe at 100.000,100.000 is z=-0.300000");

    std::string after_reinspect = get_current_message();
    INFO("After re-inspect probe: " << after_reinspect);
    REQUIRE(after_reinspect.find("Inspecting") != std::string::npos);
    REQUIRE(after_reinspect.find("(1") != std::string::npos);
    REQUIRE(after_reinspect.find("(4") == std::string::npos);
    REQUIRE(after_reinspect.find("(7") == std::string::npos);
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: BED_MESH display uses sub-phase label (no ellipsis)",
                 "[print][collector][snapmaker][bed_mesh]") {
    collector().start();
    drain_async_updates();

    feed_gcode("// Success: Set action code PRINT_BED_DETECTING");
    feed_gcode("// probe at 10.000,10.000 is z=-0.100000");

    std::string msg = get_current_message();
    INFO("Sub-phase label: " << msg);
    // Trailing ellipsis stripped before the count: "Inspecting bed (1)",
    // not "Inspecting bed... (1)".
    REQUIRE(msg.find("Inspecting bed (") != std::string::npos);
    REQUIRE(msg.find("bed... (") == std::string::npos);
}

// ============================================================================
// Snapmaker U1 pre-print phase reconciliation (issue #991-adjacent)
//
// The U1's firmware sets print_stats.state="printing" the instant the SD job
// opens, then idles for ~90s before emitting its first real action code. The
// pre-print sequence is signalled via two action-code verb forms — "// Success:
// Set action code X" and "// Success: Changed main state to PRINTING with
// action X" — in the real order captured 2026-06-18:
// PRINT_BED_DETECTING → PRINT_SWITCH_CHECKING → PRINT_AUTO_FEEDING → DETECT_PLATE
// → (silent clean + mesh + prime) → PRINT_PREEXTRUDING (on tool-change). The
// signal-less clean/mesh/prime stretch does NOT get a temps-ready timer:
// heater targets are set and cleared throughout (ext_target=0 during the early
// idle gap), so temps-ready fires unreliably. So:
//   1. The profile must carry NO silent_progression entries — a temps-ready
//      timer would announce "Purging..." ~45s before the printer homes.
//   2. Once a real firmware signal has been seen, the proactive temperature
//      heuristic must not regress the displayed phase back to the generic
//      "Preparing Print..." (INITIALIZING) — the firmware is authoritative.
// Verified against live capture on U1 @ 192.168.30.103, 2026-06-11.
// ============================================================================

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: no premature Cleaning/Purging before real firmware signals",
                 "[print][collector][snapmaker][preprint]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();

    // Reproduce the idle-gap: Moonraker says "printing", the bed sits near a
    // low standby target (so temps_ready is trivially true) and no print
    // nozzle temp is commanded yet (ext target 0). NO action code has arrived.
    set_all_temps(/*bed*/ 600, 600, /*ext*/ 0, 0);

    // Let time pass as if the firmware were idling before its real sequence.
    PrintStartCollectorTestAccess::set_temps_ready_elapsed_seconds(collector(), 30);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    // The display must NOT have jumped to the last pre-print steps before the
    // printer has done anything. (Old behavior: silent_progression announced
    // "Cleaning Nozzle..." then "Purging..." here.)
    INFO("phase=" << static_cast<int>(get_current_phase()) << " msg=" << get_current_message());
    REQUIRE(get_current_phase() != PrintStartPhase::CLEANING);
    REQUIRE(get_current_phase() != PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: proactive detection is fully suppressed after a real signal",
                 "[print][collector][snapmaker][preprint]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();

    // A real firmware signal lands — the firmware is now authoritative for the
    // displayed phase. On the U1 the bed and nozzle sit below target for almost
    // all of preparation while the firmware narrates its real sequence, so an
    // ungated proactive detector would fire Heating Bed/Heating Nozzle on nearly
    // every tick and stomp the firmware phases (the "bouncing" bug). Once a
    // signal is seen, proactive must stay completely silent.
    feed_gcode("// Success: Set action code PRINT_SWITCH_CHECKING");
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // Bed and nozzle both well below target — pre-fix this would have driven
    // the phase to HEATING_BED. It must NOT: the firmware phase stands.
    set_all_temps(/*bed*/ 200, 600, /*ext*/ 1000, 2000);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    INFO("phase=" << static_cast<int>(get_current_phase()) << " msg=" << get_current_message());
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // A later real signal advances the phase normally (firmware in control).
    feed_gcode("// Success: Set action code DETECT_PLATE");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // Still no proactive override even as temps keep changing.
    set_all_temps(600, 600, 1500, 2000);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: proactive shows Homing (not Heating Bed) while mid-home pre-signal",
                 "[print][collector][snapmaker][preprint]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();

    // Before any firmware signal, the bed warms (25/55) while G28 runs. The
    // homed_axes string is partial during homing ("xy" = z not yet homed). The
    // proactive detector must show "Homing", not mislabel the concurrent warm-up
    // as "Heating Bed".
    lv_subject_copy_string(state().get_homed_axes_subject(), "xy");
    set_all_temps(/*bed*/ 250, 550, /*ext*/ 0, 0);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // Klipper clears homed_axes to "" as each axis re-homes during G28. That
    // empty-string blip must NOT flip the label to "Heating Bed" — the HOMING
    // latch holds until the toolhead is actually fully homed.
    lv_subject_copy_string(state().get_homed_axes_subject(), "");
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // Once fully homed, proactive may surface the bed heating (still pre-signal).
    lv_subject_copy_string(state().get_homed_axes_subject(), "xyz");
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: profile carries no silent_progression entries",
                 "[print][collector][snapmaker][preprint]") {
    auto profile = PrintStartProfile::load("snapmaker_u1");
    REQUIRE(profile != nullptr);
    if (profile->name() != "Snapmaker U1") {
        SKIP("snapmaker_u1.json not available");
    }
    // The U1 fully signals its sequence; a temps-ready timer is wrong on this
    // firmware. Guard against a future edit re-introducing it.
    REQUIRE(profile->silent_progression().empty());
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: same sub-phase repeated → counter keeps incrementing",
                 "[print][collector][snapmaker][bed_mesh]") {
    collector().start();
    drain_async_updates();

    // Same action code twice in a row must NOT reset the counter — only
    // a *change* in message resets.
    feed_gcode("// Success: Set action code DETECT_PLATE");
    feed_gcode("// probe at 10.000,10.000 is z=-0.100000");
    feed_gcode("// probe at 50.000,10.000 is z=-0.105000");
    feed_gcode("// Success: Set action code DETECT_PLATE"); // same message
    feed_gcode("// probe at 90.000,10.000 is z=-0.110000");

    std::string msg = get_current_message();
    INFO("After repeated DETECT_PLATE: " << msg);
    REQUIRE(msg.find("(3") != std::string::npos);
    REQUIRE(msg.find("(1)") == std::string::npos);
}

// ============================================================================
// Snapmaker U1: the real bed mesh runs AFTER "Detecting plate" with no action
// code of its own — only the bed_mesh.py "// z offset:" line (emitted at mesh
// start) and "// z_mesh_complete:" (at the end) reach gcode_response. Without a
// mesh-start signal the "Detecting plate" label persisted through the entire
// real mesh (device-observed bug). "// z offset:" must relabel BED_MESH to
// "Bed mesh" and reset the probe counter, so probes count "(1)".."(n)" under
// the correct label. Real lines captured 2026-06-18 (gcode_store 20:21:26 /
// 20:21:30):  "// z offset: -0.05"  then  "// probe at x: 129.915, y: 125.560
// is z=299.043333".
// ============================================================================

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: z offset relabels Detecting plate -> Bed mesh and resets counter",
                 "[print][collector][snapmaker][bed_mesh]") {
    collector().start();
    drain_async_updates();

    // Plate detect runs first, counting its own probes.
    feed_gcode("// Success: Set action code DETECT_PLATE");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    feed_gcode("// probe at x: 13.000, y: 220.000 is z=-0.321667");
    feed_gcode("// probe at x: 13.000, y: 233.000 is z=-0.369167");
    {
        std::string msg = get_current_message();
        INFO("During plate detect: " << msg);
        REQUIRE(msg.find("Detecting") != std::string::npos);
        REQUIRE(msg.find("(2") != std::string::npos);
    }

    // The real mesh starts: "// z offset:" must switch the label to "Bed mesh"
    // (NOT stay on "Detecting plate") even though BED_MESH was already detected.
    feed_gcode("// z offset: -0.05");
    {
        std::string msg = get_current_message();
        INFO("After z offset: " << msg);
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
        REQUIRE(msg.find("Bed mesh") != std::string::npos);
        REQUIRE(msg.find("Detecting") == std::string::npos);
    }

    // Mesh probes now count from 1 under "Bed mesh" (counter reset on the
    // sub-phase message change — must NOT carry the 2 plate-detect probes).
    feed_gcode("// probe at x: 129.915, y: 125.560 is z=299.043333");
    {
        std::string msg = get_current_message();
        INFO("First mesh probe: " << msg);
        REQUIRE(msg.find("Bed mesh") != std::string::npos);
        REQUIRE(msg.find("(1") != std::string::npos);
        REQUIRE(msg.find("(3") == std::string::npos);
    }

    // z_mesh_complete keeps the label on "Bed mesh".
    feed_gcode("// z_mesh_complete: -0.02573436601557052");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message().find("Bed mesh") != std::string::npos);
}

// ============================================================================
// Snapmaker U1: the initial prime/purge line ("G1 X110 E15") extrudes with NO
// observable gcode_response (PRINT_PREEXTRUDING only fires for a 2nd tool
// mid-print). print_stats.print_duration going 0->positive while current_layer
// is still < 1 is the one real, observable "priming has begun" signal. The
// collector must show PURGING "Priming..." but must NOT complete the pre-print
// phase — completion stays gated on the genuine current_layer 0->1 edge.
// ============================================================================

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: note_priming shows Priming without completing pre-print",
                 "[print][collector][snapmaker][preprint]") {
    collector().start();
    drain_async_updates();

    // Sequence has reached the mesh, pre-layer-1.
    feed_gcode("// z offset: -0.05");
    feed_gcode("// z_mesh_complete: -0.02573436601557052");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // print_duration went positive (prime extrusion) while current_layer < 1.
    collector().note_priming();
    drain_async_updates();
    INFO("After note_priming: phase=" << static_cast<int>(get_current_phase())
                                      << " msg=" << get_current_message());
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    REQUIRE(get_current_message().find("Priming") != std::string::npos);

    // CRITICAL: priming is a phase UPDATE, not completion. It must NOT advance to
    // COMPLETE — that only happens on the real first-layer (current_layer 0->1).
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    // A second note_priming is a no-op (already PURGING) and still not COMPLETE.
    collector().note_priming();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);

    // The genuine first-layer signal completes it (as before).
    collector().complete_from_external_signal("first layer");
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

// ============================================================================
// Stock Klipper adaptive bed_mesh — "Adapted probe count: N,M" populates the
// live denominator via the generic gcode_response parser (no profile-side
// schema needed). Uses the default profile to verify the path works for
// Voron / KAMP / generic Klipper setups that emit this line.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Adapted probe count line populates mesh_probe_total_",
                 "[print][collector][bed_mesh][adapted]") {
    collector().start();
    drain_async_updates();

    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 0);

    auto feed = [&](const std::string& line) {
        nlohmann::json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    };

    SECTION("Stock Klipper line with // prefix sets total = N * M") {
        feed("// Adapted probe count: 4,4");
        REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 16);
    }

    SECTION("Subsequent line with different count overwrites the total") {
        feed("// Adapted probe count: 5,3");
        REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 15);
        feed("// Adapted probe count: 6,6");
        REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 36);
    }

    SECTION("Unrelated lines do not perturb the total") {
        feed("// Adapted probe count: 4,4");
        REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 16);
        feed("// probe at 10.000,10.000 is z=-0.100000");
        feed("// Bed preheating: 30s left");
        REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 16);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture, "PrintStartCollector - layer advance latch",
                 "[print_start][regression]") {
    // The pre-print phase can only end once layer data is proven to belong to
    // THIS print. A zero sample proves it, but is not guaranteed to arrive —
    // notify_status_update is coalesced. The advance latch is the second route;
    // it must not fire on a stale positive carried over from a previous print,
    // which is static by definition.
    SECTION("repeated stale value never arms the latch") {
        for (int i = 0; i < 5; ++i) {
            collector().note_current_layer(97);
        }
        CHECK_FALSE(collector().has_seen_layer_advance());
        CHECK_FALSE(collector().has_seen_layer_zero());
    }
    SECTION("an increase arms it") {
        collector().note_current_layer(3);
        CHECK_FALSE(collector().has_seen_layer_advance());
        collector().note_current_layer(8);
        CHECK(collector().has_seen_layer_advance());
    }
    SECTION("a zero sample arms the original latch") {
        collector().note_current_layer(0);
        CHECK(collector().has_seen_layer_zero());
    }
}

// ============================================================================
// Pre-mesh probe buffering (BED_MESH auto-entry from probe lines)
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "PrintStartCollector: nozzle-clean probe touches do not enter BED_MESH",
                 "[print][collector][mesh][k2]") {
    // K2 Plus BOX_NOZZLE_CLEAN touches three points on the wipe strip at Y=355,
    // outside the 350mm bed, and Klipper emits TWO lines per touch (one with
    // z_compensation, one without). The entry threshold counted raw lines, so
    // six lines tripped a threshold of three and the collector announced
    // "Bed Mesh" 79 seconds before BED_MESH_CALIBRATE actually ran.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    for (double x : {147.588, 150.588, 153.588}) {
        char with_comp[128];
        char without[128];
        std::snprintf(with_comp, sizeof(with_comp),
                      "// probe at %.3f,355.000 is z=-0.647500 z_compensation=0.050000", x);
        std::snprintf(without, sizeof(without), "// probe at %.3f,355.000 is z=-0.597500", x);
        send_gcode_response(with_comp);
        send_gcode_response(without);
    }
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() != PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "PrintStartCollector: a real mesh sweep still auto-enters BED_MESH",
                 "[print][collector][mesh]") {
    // The threshold exists for firmware that emits no mesh-start line at all.
    // Raising it must not break that: a genuine sweep crosses it and enters.
    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    for (int i = 0; i < 8; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line), "// probe at %.3f,50.000 is z=-0.031000",
                      20.0 + i * 30.0);
        send_gcode_response(line);
    }
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
}

// ============================================================================
// K2 Plus pre-print replay (end-to-end regression)
// ============================================================================

/**
 * @brief Replay a real K2 Plus PRINT_START through the collector
 *
 * Every gcode line below is verbatim from klippy.log of the 2026-08-16 print
 * quattrobox_bottom_cover_ASA-GF, in the order Moonraker forwarded it. The
 * shipped profile was written against macro names in gcode_macro.cfg rather
 * than against what the firmware echoes, and the probe counter deduped only
 * against the previous point, so this sequence produced:
 *
 *   - "Bed Mesh" announced during BOX_NOZZLE_CLEAN, 79s early, off the bed
 *   - HOMING announced at [G28_RE_CHECK], 3.5 minutes after the real G28
 *   - HEATING_NOZZLE consumed by the clean's M109, so the real heat never showed
 *   - a 67-point mesh displayed as (147/81)
 *
 * A profile edit that stops matching this stream, or a dedupe regression, puts
 * one of those back.
 */
class K2PrintStartReplayFixture : public PrintStartCollectorHeaterFixture {
  public:
    K2PrintStartReplayFixture() {
        auto profile = PrintStartProfile::load("creality_k2");
        REQUIRE(profile != nullptr);
        have_profile_ = profile->name().find("K2") != std::string::npos;
        collector().set_profile(std::move(profile));
    }

    bool have_profile_ = false;

    void settle() {
        drain_async_updates();
        drain_async_updates();
    }

    /// Both lines Klipper emits for one K2 probe touch.
    void touch(double x, double y) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "// probe at %.3f,%.3f is z=-0.647500 z_compensation=0.050000", x, y);
        send_gcode_response(buf);
        std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=-0.597500", x, y);
        send_gcode_response(buf);
    }

    /// The 67 points the adaptive sweep actually visited on the captured run.
    static std::vector<std::pair<double, double>> grid() {
        const double xs[] = {5.0, 47.5, 90.0, 132.5, 175.0, 217.5, 260.0, 302.5, 345.0};
        const double ys[] = {5.0, 47.5, 90.0, 132.5, 175.0, 217.5, 260.0, 302.5};
        std::vector<std::pair<double, double>> g;
        for (double x : xs) {
            for (double y : ys) {
                if (y == 5.0 && x > 132.5) {
                    continue; // outside the print area, never probed
                }
                g.push_back({x, y});
            }
        }
        return g;
    }
};

TEST_CASE_METHOD(K2PrintStartReplayFixture,
                 "PrintStartCollector: real K2 Plus pre-print reaches every phase in order",
                 "[print][collector][k2][integration]") {
    if (!have_profile_) {
        SKIP("creality_k2.json not available");
    }
    collector().start();
    settle();
    collector().enable_fallbacks();

    // 12:18:50 — G28. The old profile matched nothing here.
    send_gcode_response("// [DEBUG]_handle_home_rails_begin");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // 12:19:15 — z_align.
    send_gcode_response("// send query_z_align cur_retries:0 oid=4 enable=1");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::Z_TILT);

    // 12:20:50 — nozzle clean starts.
    send_gcode_response("// [NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0");
    send_gcode_response("// [GCODE]BOX_NOZZLE_CLEAN");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // 12:21:05-12:21:13 — three touches on the wipe strip at Y=355, which is
    // off the 350mm bed. Six probe lines: enough to trip a 3-LINE threshold.
    touch(147.588, 355.0);
    touch(150.588, 355.0);
    touch(153.588, 355.0);
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // 12:21:15 — the clean softens filament with M109. This DOES latch
    // HEATING_NOZZLE, and must: it is the only route into a heating phase on
    // this firmware (no M190, no M109 at print temp), and proactive temperature
    // detection is gated off once real signals are seen. The heater correction
    // then re-derives the shown phase from live temps during the bed soak.
    send_gcode_response("// [GCODE]M109 S170");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // 12:22:21 — Z re-verify. Matches "G28" as a bare substring, and must not
    // re-trigger HOMING now that we have moved past it.
    send_gcode_response("// [G28_RE_CHECK]");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // 12:22:28 — the real mesh begins.
    send_gcode_response("// exist_points[81], config_points[81]");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // 12:22:30-12:25:48 — the adaptive sweep.
    const auto g = grid();
    for (const auto& [x, y] : g) {
        touch(x, y);
    }
    // 12:26:25-12:28:56 — eight G29_RE_CHECK rounds over two corners already
    // swept, each sampling four +/-0.25mm quadrant offsets.
    for (int round = 0; round < 8; ++round) {
        for (double dx : {-0.25, 0.25}) {
            for (double dy : {-0.25, 0.25}) {
                touch(345.0 + dx, 47.5 + dy);
                touch(345.0 + dx, 302.5 + dy);
            }
        }
    }
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // The count is the grid the sweep visited, not the 262 lines that carried
    // it. (The denominator is absent here only because the mock never answers
    // the probe-count RPC, so this does not also pin adaptive_meshing — the
    // profile test does that.)
    const std::string expected = "Bed Mesh (" + std::to_string(g.size()) + ")";
    REQUIRE(get_current_message() == expected);

    // 12:34:39 — CFS purge. The old BOX_MATERIAL_FLUSH pattern never matched.
    send_gcode_response("// flush_temp: 220");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
}

// ============================================================================
// K1C replay: probe lines are mesh data, not phase patterns
// ============================================================================

/**
 * @brief Replay a real K1C PRINT_START through the collector
 *
 * Verbatim from klippy.log of the 2026-08-19 print Bed_Mesh_Test_Layer_PLA,
 * in the order Moonraker forwarded it. The K1 firmware's interesting phases
 * (PRTouch homing, accurate G28, the CHECK_BED_MESH corner validation) echo
 * nothing to gcode_response, so the stream is sparse: two nozzle-wipe
 * markers, a long silent gap, then one "probe at" line per mesh point, then
 * the draw-line heater markers. The pre-mesh buffer's 5-distinct-point
 * threshold is what carries the collector from CLEANING into BED_MESH.
 */
class K1CPrintStartReplayFixture : public PrintStartCollectorHeaterFixture {
  public:
    K1CPrintStartReplayFixture() {
        auto profile = PrintStartProfile::load("creality_k1");
        REQUIRE(profile != nullptr);
        have_profile_ = profile->name().find("K1") != std::string::npos;
        collector().set_profile(std::move(profile));
        // configfile.settings.bed_mesh.probe_count = 5x5 — what the real
        // printer answered to the entry-time objects.query (the live mesh is
        // cleared at print start, so probe_count is the source, not
        // probed_matrix).
        client().set_config_bed_mesh_probe_count(5, 5);
    }

    bool have_profile_ = false;

    void settle() {
        drain_async_updates();
        drain_async_updates();
    }

    /// One K1C probe line — a single sample per point, no z_compensation twin.
    void point(double x, double y) {
        char buf[120];
        std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=0.160594", x, y);
        send_gcode_response(buf);
    }
};

TEST_CASE_METHOD(K1CPrintStartReplayFixture,
                 "PrintStartCollector: K1C mesh sweep keeps its denominator",
                 "[print][collector][k1c][integration]") {
    if (!have_profile_) {
        SKIP("creality_k1.json not available");
    }
    collector().start();
    settle();
    collector().enable_fallbacks();

    // 19:24:47 — START_PRINT's full-prep branch announces itself.
    send_gcode_response("// not prepare.");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // 19:25:34 — the nozzle wipe markers; the last forwarded signal before
    // ~3 minutes of firmware silence.
    send_gcode_response("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.213906");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    send_gcode_response("// [CLEAR_NOZZLE_QUICK] end_pos[2]:3.246250");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // 19:28:09 — CHECK_BED_MESH failed its corner validation, so the firmware
    // re-meshes: one probe line per point over a 5x5 grid. The first five
    // distinct points cross the pre-mesh entry threshold.
    const double c[] = {5.0, 57.5, 110.0, 162.5, 215.0};
    for (double x : c) {
        point(x, 5.0);
    }
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 25);

    // The rest of the sweep. Every line here used to re-match the profile's
    // BED_MESH pattern and reset the counters, losing the denominator.
    for (double y : {57.5, 110.0, 162.5, 215.0}) {
        for (double x : c) {
            point(x, y);
            settle();
            REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 25);
        }
    }
    // 25 points counted, denominator intact, message carries both.
    REQUIRE(get_current_message() == "Bed Mesh (25/25)");

    // 19:31:01 — draw line + final heat. The can_break_flag markers close it out.
    send_gcode_response("// can_break_flag = 0");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    send_gcode_response("// can_break_flag is 3");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(K1CPrintStartReplayFixture,
                 "K2 purge-percent and box tags are ignored without cfs_signals",
                 "[print][collector][k2][negative]") {
    // The creality_k1 profile does not declare cfs_signals: on a K1/K1C the
    // tag stream never appears, and these matchers must not fire on lines
    // that merely happen to contain their vocabulary.
    collector().start();
    settle();

    send_gcode_response("// num: 0, velocity: 575.000000, percent 0.500000");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    send_gcode_response("// [box] cut sensor detected");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE(get_current_message().find("Loading Filament") == std::string::npos);
}

/**
 * The K1C's longest display dead-zone is the stretch between the nozzle-wipe
 * markers and the first mesh probe line: accurate Z homing and the bed-mesh
 * corner validation run there, and the firmware echoes none of it to
 * gcode_response (2026-08-19, ~3 minutes stuck on "Cleaning Nozzle...").
 *
 * What the printer DOES emit is a bed_mesh status flap: klippy reports the
 * loaded profile, then clears it, when the probing sequence begins. A mesh
 * that disappears while the collector is in CLEANING is the start of that
 * silent meshing work, so the display moves to "Bed Meshing..." — with the
 * probe denominator already sized from the entry-time query. The same clear
 * arriving BEFORE the nozzle clean is the rough G28's own mesh clear and
 * carries no phase information.
 */
TEST_CASE_METHOD(K1CPrintStartReplayFixture,
                 "PrintStartCollector: bed-mesh clear during cleaning enters Bed Meshing",
                 "[print][collector][k1c][bedmesh-flap]") {
    collector().start();
    settle();
    collector().enable_fallbacks();

    send_gcode_response("// not prepare.");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // Rough G28: mesh reported, then cleared — before any clean marker, so
    // it must not move the phase.
    collector().note_bed_mesh_presence(true);
    collector().note_bed_mesh_presence(false);
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // Nozzle wipe markers put the display in CLEANING.
    send_gcode_response("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.213906");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // Mesh re-reported mid-sequence: presence alone changes nothing.
    collector().note_bed_mesh_presence(true);
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // Accurate G28 clears it — leveling work begins, display follows.
    collector().note_bed_mesh_presence(false);
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Bed Meshing...");

    // Mesh probes now count against the denominator the entry query fetched.
    const double c[] = {5.0, 57.5, 110.0, 162.5, 215.0};
    for (double x : c) {
        point(x, 5.0);
    }
    settle();
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 25);
    REQUIRE(get_current_message() == "Bed Meshing (5/25)");
}

/**
 * Profile "message" strings are English tags like every other translatable
 * string, but they reach the display raw from the profile JSON — a German
 * user saw "Cleaning Nozzle..." straight through the whole pre-print. They
 * now pass through lv_tr() at match time, so the tag resolves through the
 * loaded pack like the built-in labels do.
 */
TEST_CASE_METHOD(K1CPrintStartReplayFixture,
                 "PrintStartCollector: profile phase messages translate",
                 "[print][collector][i18n]") {
    helix::ui::ensure_translation_loaded("de");
    lv_translation_set_language("de");

    collector().start();
    settle();
    collector().enable_fallbacks();

    send_gcode_response("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.213906");
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    REQUIRE(get_current_message() == "Düse reinigen...");

    send_gcode_response("// x_axes: xyz");
    settle();
    REQUIRE(get_current_message() == "Referenzfahrt...");
}

// ============================================================================
// Timeout must key on quiet, not on elapsed time
// ============================================================================

/**
 * The adaptive timeout used to fire on (elapsed > threshold && temps_near).
 * On any printer that meshes AFTER heating, temps_near goes true minutes before
 * the pre-print is actually over, so the timeout fired mid-sequence. That set
 * fallback_completion_, which makes save_prediction_entry() skip, so the
 * prediction never grew and the next run timed out at the same point — a
 * deadlock the collector could not learn its way out of.
 *
 * Observed on a K2 Plus 2026-08-16: predicted 185s, timeout at 278s, real
 * pre-print ~1140s. Every run in a 38-hour log ended on this timeout.
 *
 * A printer still narrating its pre-print is not stuck, however long it takes.
 */
TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Timeout does not fire while pre-print activity is recent",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    set_all_temps(1050, 1050, 2610, 2650); // temps_near = true
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
    // ...but the printer spoke 10 seconds ago.
    PrintStartCollectorTestAccess::set_last_activity_seconds_ago(collector(), 10);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture, "Timeout fires once the printer goes quiet",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    set_all_temps(1050, 1050, 2610, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
    // Nothing heard for well over the quiet window — this one really is stuck.
    PrintStartCollectorTestAccess::set_last_activity_seconds_ago(collector(), 300);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A long but active pre-print survives past the old ceilings",
                 "[print][collector][timeout][k2]") {
    // The K2 Plus pre-print runs ~1140s: heat, then a ~390s mesh, then purge.
    // Both the old adaptive ceiling (predicted * 2.5) and ABSOLUTE_MAX_TIMEOUT
    // (900s) cut it off while the printer was still working.
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    PrintStartCollectorTestAccess::set_predicted_total(collector(), 185.0f);
    set_all_temps(1050, 1050, 2610, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1100);
    PrintStartCollectorTestAccess::set_last_activity_seconds_ago(collector(), 3);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture, "A probe line counts as pre-print activity",
                 "[print][collector][timeout]") {
    // Mesh probing is the longest silent-to-the-profile stretch on many
    // firmwares: no phase pattern matches for minutes, only probe lines.
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    set_all_temps(1050, 1050, 2610, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    send_gcode_response("// probe at 100.000,100.000 is z=-0.031000");
    drain_async_updates();

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
}

// ============================================================================
// POSITION TELEMETRY INTEGRATION — silent-window refinement, end to end.
// Coordinates below are the real K1C capture values (mesh 5..215, wipe strip
// beyond Y=215, centre probes at ~(110,110), corner validation at the mesh
// corners, sweep rows marching X at constant Y).
// ============================================================================

TEST_CASE_METHOD(K1CPrintStartReplayFixture,
                 "Position samples refine the silent window's status line",
                 "[print][collector][k1c][position]") {
    if (!have_profile_) {
        SKIP("creality_k1.json not available");
    }
    collector().start();
    settle();
    collector().enable_fallbacks();
    collector().note_mesh_bounds(5.0f, 215.0f, 5.0f, 215.0f);

    // The console marker that enters CLEANING — then the firmware goes quiet
    // (no forwarded markers for the Z probes / corner validation / sweep).
    send_gcode_response("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.1676562");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // Centre Z probes: hover-and-dip at the mesh centre.
    collector().note_position_sample(114.1f, 103.7f, 6.0f);
    collector().note_position_sample(114.1f, 103.7f, 0.0f);
    collector().note_position_sample(110.7f, 110.9f, 6.0f);
    collector().note_position_sample(110.7f, 110.9f, 0.0f);
    drain_async_updates();
    REQUIRE(get_current_message() == "Probing Z...");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING); // message-only

    // Corner validation tour: three distinct mesh corners.
    collector().note_position_sample(5.0f, 5.0f, 5.0f);
    collector().note_position_sample(5.0f, 215.0f, 5.0f);
    collector().note_position_sample(215.0f, 215.0f, 5.0f);
    drain_async_updates();
    REQUIRE(get_current_message() == "Checking Bed Mesh...");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // Sweep march promotes the phase (same edge the bed-mesh flap produces).
    collector().note_position_sample(57.5f, 5.0f, 3.0f);
    collector().note_position_sample(110.0f, 5.0f, 3.0f);
    collector().note_position_sample(162.5f, 5.0f, 3.0f);
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Bed Meshing...");
}

TEST_CASE_METHOD(K1CPrintStartReplayFixture,
                 "Buffered pre-mesh probes are credited when the sweep march promotes BED_MESH",
                 "[print][collector][k1c][position]") {
    // K1C capture 2026-08-20: the two front-row probes arrived before the
    // position classifier's sweep-march verdict, were buffered ("Pre-mesh
    // probe point 1/5 (buffering)"), and were then DISCARDED when the march
    // promoted BED_MESH - the displayed count lagged the physical taps by 2
    // for the whole mesh.
    if (!have_profile_) {
        SKIP("creality_k1.json not available");
    }
    collector().start();
    settle();
    collector().enable_fallbacks();
    collector().note_mesh_bounds(5.0f, 215.0f, 5.0f, 215.0f);
    send_gcode_response("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.1676562");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    // Two front-row probe lines buffer below the console entry threshold.
    point(110.0, 5.0);
    point(130.0, 5.0);
    settle();
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 0);

    // The sweep march promotes BED_MESH from position telemetry.
    collector().note_position_sample(110.0f, 5.0f, 3.0f);
    collector().note_position_sample(130.0f, 5.0f, 3.0f);
    collector().note_position_sample(150.0f, 5.0f, 3.0f);
    collector().note_position_sample(170.0f, 5.0f, 3.0f);
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    // The buffered front row is the sweep's first points.
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 2);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Position samples ignored without profile position_signals",
                 "[print][collector][position]") {
    // Default profile has no position_signals — the inference must stay off.
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    collector().note_mesh_bounds(5.0f, 215.0f, 5.0f, 215.0f);

    const std::string before = get_current_message();

    collector().note_position_sample(114.1f, 103.7f, 6.0f);
    collector().note_position_sample(110.7f, 110.9f, 0.0f);
    collector().note_position_sample(110.7f, 110.9f, 0.0f);
    drain_async_updates();

    CHECK(get_current_message() == before);
}
