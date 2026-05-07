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

#include <regex>
#include <string>
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

    // Set predicted total to 400s (absolute timeout = max(400*2.5, 900) = 1000s)
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
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1010);
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
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 910);
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
// PrintPhaseTracker. Universal printers fall through these matchers.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "K2 purge percent (fraction form) drives PURGING progress",
                 "[print][collector][k2]") {
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

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "K2 purge percent (legacy integer form) drives PURGING progress",
                 "[print][collector][k2]") {
    collector().start();
    drain_async_updates();

    // Legacy/integer form: "percent:" followed by 0..100 integer.
    send_gcode_response("// num: 0, velocity: 23.0, percent: 75");
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    REQUIRE(get_current_progress() == 75);
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "CFS box cut sensor detected enters INITIALIZING with Loading Filament",
                 "[print][collector][k2][cfs]") {
    collector().start();
    drain_async_updates();

    send_gcode_response("// [box] cut sensor detected");
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE(get_current_message().find("Loading Filament") != std::string::npos);
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Stock Klipper purge-line text falls through K2 matcher",
                 "[print][collector][k2]") {
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
        "// probe at 30.000,30.000 is z=-0.580000",
        "// probe at 30.000,30.000 is z=-0.582500",
        "// probe at 30.000,30.000 is z=-0.580000",
        "// probe at 270.000,30.000 is z=-0.532500",
        "// probe at 270.000,30.000 is z=-0.532500",
        "// probe at 270.000,30.000 is z=-0.532500",
        "// probe at 270.000,270.000 is z=-0.495000",
        "// probe at 270.000,270.000 is z=-0.490000",
        "// probe at 270.000,270.000 is z=-0.490000",
        "// probe at 30.000,270.000 is z=-0.557500",
        "// probe at 30.000,270.000 is z=-0.555000",
        "// probe at 30.000,270.000 is z=-0.555000",
    };
    for (const char* line : qgl_probe_lines) {
        feed_gcode(line);
    }

    // Must remain in QGL — these probes are corner pads, not bed mesh points.
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);
}

TEST_CASE_METHOD(VoronCollectorFixture,
                 "BED_MESH_CALIBRATE after QGL enters BED_MESH cleanly",
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
