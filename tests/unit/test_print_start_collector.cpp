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

#include "../test_helpers/print_start_collector_test_access.h"
#include "moonraker_manager.h"
#include "print_start_profile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintStartPhase;

// ============================================================================
// Line classifiers under test: the collector's universal markers and the
// response patterns of the shipped default profile.
// ============================================================================

namespace {

bool is_print_start(const std::string& line) {
    return PrintStartCollectorTestAccess::is_print_start_marker(line);
}

bool is_completion(const std::string& line) {
    return PrintStartCollectorTestAccess::is_completion_marker(line);
}

bool is_respond_completion(const std::string& line) {
    return PrintStartCollectorTestAccess::is_respond_completion(line);
}

/// The phase the default profile reads out of a G-code response, IDLE if none.
PrintStartPhase default_phase(const std::string& line) {
    static const auto profile = PrintStartProfile::load_default();
    PrintStartProfile::MatchResult result;
    return profile->try_match_pattern(line, result) ? result.phase : PrintStartPhase::IDLE;
}

} // namespace

// ============================================================================
// PRINT_START Marker Tests
// ============================================================================

TEST_CASE("PrintStart: PRINT_START marker detection", "[core][print][marker]") {
    // Should match
    REQUIRE(is_print_start("PRINT_START"));
    REQUIRE(is_print_start("START_PRINT"));
    REQUIRE(is_print_start("_PRINT_START"));
    REQUIRE(is_print_start("print_start")); // Case insensitive
    REQUIRE(is_print_start("Calling PRINT_START with args"));

    // Real macro invocations
    REQUIRE(is_print_start("START_PRINT BED_TEMP=60 EXTRUDER_TEMP=200"));
    REQUIRE(is_print_start("PRINT_START BED=60 EXTRUDER=200 CHAMBER=35"));

    // Should NOT match
    REQUIRE(!is_print_start("PRINTS_TART"));
    REQUIRE(!is_print_start("G28"));
    REQUIRE(!is_print_start(""));
}

// ============================================================================
// Completion Marker Tests
// ============================================================================

TEST_CASE("PrintStart: completion marker detection", "[core][print][completion]") {
    // Should match
    REQUIRE(is_completion("SET_PRINT_STATS_INFO CURRENT_LAYER=1"));
    REQUIRE(is_completion("LAYER: 1"));
    REQUIRE(is_completion("LAYER:1"));
    REQUIRE(is_completion(";LAYER:1"));
    REQUIRE(is_completion("First layer starting"));

    // Should NOT match (not layer 1)
    REQUIRE(!is_completion("LAYER: 2"));
    REQUIRE(!is_completion("LAYER:10"));
    REQUIRE(!is_completion("LAYER:100"));
    REQUIRE(!is_completion("SET_PRINT_STATS_INFO")); // No CURRENT_LAYER
    REQUIRE(!is_completion("status_printing"));
}

// ============================================================================
// RESPOND Completion Pattern Tests
// ============================================================================

TEST_CASE("PrintStart: RESPOND completion detection", "[core][print][respond]") {
    SECTION("Should match common print start messages") {
        REQUIRE(is_respond_completion("// Print started!"));
        REQUIRE(is_respond_completion("Print Started"));
        REQUIRE(is_respond_completion("PRINT STARTING"));
        REQUIRE(is_respond_completion("Print Start Complete"));
        REQUIRE(is_respond_completion("print started"));
        REQUIRE(is_respond_completion("Starting print"));
        REQUIRE(is_respond_completion("starting print now"));
        REQUIRE(is_respond_completion("Started print successfully"));
    }

    SECTION("Should NOT match unrelated messages") {
        // "Printing" is not "print" (word boundary)
        REQUIRE(!is_respond_completion("Printing layer 5"));
        // "restart" is not "start" (word boundary)
        REQUIRE(!is_respond_completion("restart print"));
        // "print_start" — underscore breaks word boundary for "start"
        REQUIRE(!is_respond_completion("print_start phase"));
        // No "print" keyword
        REQUIRE(!is_respond_completion("Starting temperature check"));
        // No "start" variant keyword
        REQUIRE(!is_respond_completion("print complete"));
        // Unrelated with both words far apart in different context
        REQUIRE(!is_respond_completion("start heating the print bed"));
    }

    SECTION("A host prompt asking to start the print is not a print that started") {
        // Z-Mod's material picker footer, emitted before any print exists
        REQUIRE(!is_respond_completion(
            "// action:prompt_footer_button Start print|PRINT_ZCOLOR LEVELING=0 "
            "FILENAME=\"Button_PETG_6m32s.gcode\" ALLOWED_TOOL_COUNT=1"));
        REQUIRE(!is_respond_completion("// action:prompt_button Start print|START_PRINT"));
        REQUIRE(!is_respond_completion("// action:prompt_text Print starting soon"));
        REQUIRE(!is_respond_completion("  // action:prompt_begin Start print?"));
        // An action line that is not a prompt still carries a message
        REQUIRE(is_respond_completion("// action:notification Print started"));
    }
}

TEST_CASE("PrintStart: RESPOND and PRINT_START patterns are independent",
          "[core][print][respond]") {
    SECTION("RESPOND messages with 'Print started' should not match PRINT_START marker pattern") {
        // "Print started!" is a RESPOND message, not a macro invocation
        // The print_start_pattern only matches exact macro names: PRINT_START, START_PRINT,
        // _PRINT_START
        REQUIRE(!is_print_start("Print started!"));
        REQUIRE(!is_print_start("PRINT STARTED"));
        REQUIRE(!is_print_start("print starting"));
        // But RESPOND pattern should match these:
        REQUIRE(is_respond_completion("Print started!"));
        REQUIRE(is_respond_completion("PRINT STARTED"));
        REQUIRE(is_respond_completion("print starting"));
    }

    SECTION("PRINT_START macro lines should not trigger RESPOND completion") {
        // "PRINT_START BED=60 EXTRUDER=200" should NOT match respond pattern
        // because "PRINT_START" has underscore breaking word boundary for "start"
        REQUIRE(!is_respond_completion("PRINT_START BED=60 EXTRUDER=200"));
        REQUIRE(!is_respond_completion("START_PRINT BED=60"));
        REQUIRE(!is_respond_completion("_PRINT_START"));
    }
}

TEST_CASE("PrintStart: timeout fallback still available", "[core][print][timeout]") {
    // Verifies RESPOND pattern doesn't match common noise lines that could
    // interfere with timeout-based completion as a last resort.

    SECTION("RESPOND pattern does not match empty or noise lines") {
        REQUIRE(!is_respond_completion(""));
        REQUIRE(!is_respond_completion("ok"));
        REQUIRE(!is_respond_completion("T:200.0 /200.0 B:60.0 /60.0"));
        REQUIRE(!is_respond_completion("echo: M190 S60"));
    }
}

// ============================================================================
// Homing Phase Tests
// ============================================================================

TEST_CASE("PrintStart: homing phase detection", "[core][print][homing]") {
    // Should match
    REQUIRE(default_phase("G28") == PrintStartPhase::HOMING);
    REQUIRE(default_phase("G28 X Y Z") == PrintStartPhase::HOMING);
    REQUIRE(default_phase("G28 Z") == PrintStartPhase::HOMING);
    REQUIRE(default_phase("Homing axes") == PrintStartPhase::HOMING);
    REQUIRE(default_phase("Home All Axes") == PrintStartPhase::HOMING);
    REQUIRE(default_phase("// homing started") == PrintStartPhase::HOMING);

    // Real Voron V2 macro output
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Homing\"") == PrintStartPhase::HOMING);

    // Should NOT match
    REQUIRE(default_phase("G29") == PrintStartPhase::IDLE); // Bed leveling
    REQUIRE(default_phase("M104") == PrintStartPhase::IDLE);
}

// ============================================================================
// Heating Phase Tests
// ============================================================================

TEST_CASE("PrintStart: heating bed phase detection", "[core][print][heating]") {
    // Should match
    REQUIRE(default_phase("M190 S60") == PrintStartPhase::HEATING_BED); // Wait for bed
    REQUIRE(default_phase("M140 S60") == PrintStartPhase::HEATING_BED); // Set bed
    REQUIRE(default_phase("Heating bed to 60") == PrintStartPhase::HEATING_BED);
    REQUIRE(default_phase("Heat Bed") == PrintStartPhase::HEATING_BED);
    REQUIRE(default_phase("BED_TEMP=60") == PrintStartPhase::HEATING_BED);
    REQUIRE(default_phase("bed heating") == PrintStartPhase::HEATING_BED);

    // Real Voron V2 macro: M190 S{BED_TEMP}
    REQUIRE(default_phase("M190 S110") == PrintStartPhase::HEATING_BED);

    // Should NOT match
    REQUIRE(default_phase("M140 S0") == PrintStartPhase::IDLE); // Setting to 0 (cooling)
    REQUIRE(default_phase("M104 S200") == PrintStartPhase::HEATING_NOZZLE); // Nozzle temp
}

TEST_CASE("PrintStart: heating nozzle phase detection", "[print][heating]") {
    // Should match
    REQUIRE(default_phase("M109 S200") == PrintStartPhase::HEATING_NOZZLE); // Wait for nozzle
    REQUIRE(default_phase("M104 S200") == PrintStartPhase::HEATING_NOZZLE); // Set nozzle
    REQUIRE(default_phase("M104 S150") == PrintStartPhase::HEATING_NOZZLE); // Mesh temp
    REQUIRE(default_phase("Heating nozzle to 200") == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(default_phase("Heating hotend") == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(default_phase("Heating extruder") == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(default_phase("EXTRUDER_TEMP=200") == PrintStartPhase::HEATING_NOZZLE);

    // Real Voron V2 macro output
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Heating for print\"") ==
            PrintStartPhase::IDLE); // "for print" not "nozzle"
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Heating extruder and bed for probing\"") ==
            PrintStartPhase::HEATING_NOZZLE);

    // Should NOT match
    REQUIRE(default_phase("M104 S0") == PrintStartPhase::IDLE);         // Cooling
    REQUIRE(default_phase("M190 S60") == PrintStartPhase::HEATING_BED); // Bed temp
}

// ============================================================================
// Leveling Phase Tests
// ============================================================================

TEST_CASE("PrintStart: QGL phase detection", "[print][leveling]") {
    // Should match
    REQUIRE(default_phase("QUAD_GANTRY_LEVEL") == PrintStartPhase::QGL);
    REQUIRE(default_phase("quad gantry level") == PrintStartPhase::QGL);
    REQUIRE(default_phase("Running QGL") == PrintStartPhase::QGL);

    // Klipper does not echo a macro's commands, so a macro-driven QGL is legible
    // only through its display text and QGL's own console report.
    REQUIRE(default_phase("Leveling gantry") == PrintStartPhase::QGL);
    REQUIRE(default_phase("Levelling gantry...") == PrintStartPhase::QGL);
    REQUIRE(default_phase("Gantry leveling") == PrintStartPhase::QGL);
    REQUIRE(default_phase("// Gantry-relative probe points:\n// 0: 6.137500 1: 5.125000") ==
            PrintStartPhase::QGL);

    // QGL and Z_TILT_ADJUST share Klipper's adjust/retry report, so it names neither.
    REQUIRE(default_phase("// Making the following Z adjustments:\n// stepper_z = -1.002280") ==
            PrintStartPhase::IDLE);
    REQUIRE(default_phase("// Retries: 0/5 Probed points range: 1.742500 tolerance: 0.010000") ==
            PrintStartPhase::IDLE);
    REQUIRE(default_phase("Leveling 3/9") == PrintStartPhase::IDLE);

    // Should NOT match
    REQUIRE(default_phase("Z_TILT_ADJUST") == PrintStartPhase::Z_TILT);
    REQUIRE(default_phase("G28") == PrintStartPhase::HOMING);
}

TEST_CASE("PrintStart: Z_TILT phase detection", "[print][leveling]") {
    // Should match
    REQUIRE(default_phase("Z_TILT_ADJUST") == PrintStartPhase::Z_TILT);
    REQUIRE(default_phase("z_tilt_adjust") == PrintStartPhase::Z_TILT);
    REQUIRE(default_phase("z tilt adjust") == PrintStartPhase::Z_TILT);
    REQUIRE(default_phase("Z-tilt") == PrintStartPhase::Z_TILT);
    REQUIRE(default_phase("Running Z tilt...") == PrintStartPhase::Z_TILT);

    // Should NOT match
    REQUIRE(default_phase("QUAD_GANTRY_LEVEL") == PrintStartPhase::QGL);
}

// ============================================================================
// Bed Mesh Phase Tests
// ============================================================================

TEST_CASE("PrintStart: bed mesh phase detection", "[print][mesh]") {
    // Should match
    REQUIRE(default_phase("BED_MESH_CALIBRATE") == PrintStartPhase::BED_MESH);
    REQUIRE(default_phase("BED_MESH_PROFILE LOAD=default") == PrintStartPhase::BED_MESH);
    REQUIRE(default_phase("Loading bed mesh") == PrintStartPhase::BED_MESH);
    REQUIRE(default_phase("mesh loading") == PrintStartPhase::BED_MESH);

    // Real Voron V2 macro: BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1
    REQUIRE(default_phase("BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1") ==
            PrintStartPhase::BED_MESH);

    // Display text naming the phase counts too
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Performing bed mesh calibration\"") ==
            PrintStartPhase::BED_MESH);

    // Should NOT match
    REQUIRE(default_phase("BED_MESH_CLEAR") == PrintStartPhase::IDLE);
}

// ============================================================================
// Cleaning Phase Tests
// ============================================================================

TEST_CASE("PrintStart: cleaning phase detection", "[print][cleaning]") {
    // Should match
    REQUIRE(default_phase("CLEAN_NOZZLE") == PrintStartPhase::CLEANING);
    REQUIRE(default_phase("NOZZLE_CLEAN") == PrintStartPhase::CLEANING);
    REQUIRE(default_phase("WIPE_NOZZLE") == PrintStartPhase::CLEANING);
    REQUIRE(default_phase("nozzle wipe") == PrintStartPhase::CLEANING);
    REQUIRE(default_phase("clean nozzle") == PrintStartPhase::CLEANING);
    REQUIRE(default_phase("clean_nozzle") == PrintStartPhase::CLEANING); // Voron V2 macro call

    // Real Voron V2 display text
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Cleaning nozzle\"") == PrintStartPhase::CLEANING);

    // Should NOT match
    REQUIRE(default_phase("PURGE_LINE") == PrintStartPhase::PURGING);
}

// ============================================================================
// Purging Phase Tests
// ============================================================================

TEST_CASE("PrintStart: purging phase detection", "[print][purging]") {
    // Should match
    REQUIRE(default_phase("VORON_PURGE") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("LINE_PURGE") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("PURGE_LINE") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("Prime Line") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("PrimeLine") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("Priming extruder") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("KAMP_ADAPTIVE_PURGE") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("purge line done") == PrintStartPhase::PURGING);

    // Real Voron V2 display text
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Purging\"") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("AFC_Poop: purge_length=100") == PrintStartPhase::IDLE);

    // Should NOT match
    REQUIRE(default_phase("CLEAN_NOZZLE") == PrintStartPhase::CLEANING);
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
        PrintStartPhase expected; ///< INITIALIZING = the PRINT_START marker
        const char* description;
    };

    std::vector<TestCase> voron_lines = {
        {"START_PRINT BED_TEMP=110 EXTRUDER_TEMP=250 CHAMBER_TEMP=45",
         PrintStartPhase::INITIALIZING, "macro invocation"},
        {"M104 S150", PrintStartPhase::HEATING_NOZZLE, "mesh temp heating"},
        {"M190 S110", PrintStartPhase::HEATING_BED, "bed temp wait"},
        {"G28", PrintStartPhase::HOMING, "home all"},
        {"clean_nozzle", PrintStartPhase::CLEANING, "nozzle clean macro"},
        {"QUAD_GANTRY_LEVEL", PrintStartPhase::QGL, "quad gantry level"},
        {"G28 Z", PrintStartPhase::HOMING, "home Z after QGL"},
        {"BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1", PrintStartPhase::BED_MESH,
         "adaptive bed mesh"},
        {"M109 S250", PrintStartPhase::HEATING_NOZZLE, "extruder temp wait"},
        {"VORON_PURGE", PrintStartPhase::PURGING, "voron purge"},
    };

    for (const auto& tc : voron_lines) {
        CAPTURE(tc.description, tc.line);
        if (tc.expected == PrintStartPhase::INITIALIZING) {
            REQUIRE(is_print_start(tc.line));
        } else {
            REQUIRE(default_phase(tc.line) == tc.expected);
        }
    }
}

TEST_CASE("PrintStart: Voron V2 SET_DISPLAY_TEXT messages", "[print][voron]") {
    // These are the display messages from the macro
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Homing\"") == PrintStartPhase::HOMING);

    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Cleaning nozzle\"") == PrintStartPhase::CLEANING);

    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Leveling gantry\"") == PrintStartPhase::QGL);
    REQUIRE(default_phase("Performing bed mesh calibration") == PrintStartPhase::BED_MESH);
    REQUIRE(default_phase("Purging") == PrintStartPhase::PURGING);

    // Wording that names no phase the patterns know does not match
    REQUIRE(default_phase("SET_DISPLAY_TEXT MSG=\"Heating for print\"") == PrintStartPhase::IDLE);
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
        PrintStartPhase expected; ///< INITIALIZING = the PRINT_START marker
        const char* description;
    };

    std::vector<TestCase> ad5m_lines = {
        {"START_PRINT BED_TEMP=60 EXTRUDER_TEMP=200", PrintStartPhase::INITIALIZING,
         "macro invocation"},
        {"RESPOND MSG=\"START_PRINT\"", PrintStartPhase::INITIALIZING, "respond with start marker"},
        {"M140 S60", PrintStartPhase::HEATING_BED, "set bed temp"},
        {"M104 S200", PrintStartPhase::HEATING_NOZZLE, "set nozzle temp"},
        {"G28", PrintStartPhase::HOMING, "home all"},
        {"BED_MESH_CALIBRATE mesh_min=-100,-100 mesh_max=100,100", PrintStartPhase::BED_MESH,
         "KAMP mesh calibrate"},
        {"BED_MESH_PROFILE LOAD=auto", PrintStartPhase::BED_MESH, "load auto mesh profile"},
        {"LINE_PURGE", PrintStartPhase::PURGING, "KAMP line purge"},
    };

    for (const auto& tc : ad5m_lines) {
        CAPTURE(tc.description, tc.line);
        if (tc.expected == PrintStartPhase::INITIALIZING) {
            REQUIRE(is_print_start(tc.line));
        } else {
            REQUIRE(default_phase(tc.line) == tc.expected);
        }
    }
}

TEST_CASE("PrintStart: AD5M Pro _PRINT_STATUS messages", "[print][ad5m]") {
    // These are unique to AD5M Pro mod firmware
    REQUIRE(default_phase("_PRINT_STATUS S=\"HOMING...\"") == PrintStartPhase::HOMING);

    // Note: These DON'T match because they use different wording (status strings only)
    REQUIRE(default_phase("_PRINT_STATUS S=\"HEATING...\"") == PrintStartPhase::IDLE);
    REQUIRE(default_phase("_PRINT_STATUS S=\"MESH CHECKING...\"") == PrintStartPhase::IDLE);
}

TEST_CASE("PrintStart: AD5M Pro KAMP-specific patterns", "[print][ad5m][kamp]") {
    // KAMP adaptive purge patterns
    REQUIRE(default_phase("KAMP_ADAPTIVE_PURGE") == PrintStartPhase::PURGING);
    REQUIRE(default_phase("_LINE_PURGE") == PrintStartPhase::PURGING);

    // KAMP bed mesh with parameters
    REQUIRE(default_phase("BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1") ==
            PrintStartPhase::BED_MESH);
    REQUIRE(default_phase("_KAMP_BED_MESH_CALIBRATE") == PrintStartPhase::BED_MESH);
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

    for (const auto& line : noise_lines) {
        CAPTURE(line);
        REQUIRE(default_phase(line) == PrintStartPhase::IDLE);
    }
}

// ============================================================================
// AREA A: HELIX:PHASE Signal Detection Tests
// ============================================================================
// Tests for check_helix_phase_signal() method which parses signals like:
// - HELIX:PHASE:STARTING -> sets INITIALIZING phase
// - HELIX:PHASE:COMPLETE -> sets COMPLETE phase
// - Various phase transitions

#include "ui_print_preparation_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/archive_transfers.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/preprint_config_scope.h"
#include "../test_helpers/print_start_collector_test_access.h"
#include "../test_helpers/print_start_profile_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"
#include "print_start_analyzer.h"
#include "print_start_collector.h"
#include "print_start_profile.h"
#include "simulated_clock.h"
#include "thermal_rate_model.h"
#include "translation_loader.h"

using namespace helix;
using namespace helix::ui;
/**
 * @brief HELIX:PHASE signal parser for direct testing
 *
 * Mirrors the prefix handling in PrintStartCollector::check_helix_phase_signal()
 * so the signal format can be exercised without the callback infrastructure.
 * The name lookup itself is the shipping one, so an alias that stops resolving
 * shows up here.
 *
 * Returns the PrintStartPhase the signal would set, or PrintStartPhase::IDLE
 * when the signal is not recognized.
 */
static std::pair<PrintStartPhase, std::string> parse_helix_phase_signal(const std::string& line) {
    static const char* kHelixPhasePrefix = "HELIX:PHASE:";
    constexpr size_t PREFIX_LEN = 12;

    size_t pos = line.find(kHelixPhasePrefix);
    if (pos == std::string::npos) {
        return {PrintStartPhase::IDLE, ""};
    }

    std::string phase_name = line.substr(pos + PREFIX_LEN);
    size_t end = phase_name.find_first_of(" \t\n\r\"'");
    if (end != std::string::npos) {
        phase_name = phase_name.substr(0, end);
    }

    const auto phase = helix::print_start_phase_from_name(phase_name);
    if (!phase || *phase == PrintStartPhase::IDLE) {
        return {PrintStartPhase::IDLE, ""};
    }

    static const std::map<PrintStartPhase, std::string> kMessages = {
        {PrintStartPhase::INITIALIZING, "Preparing Print..."},
        {PrintStartPhase::HOMING, "Homing..."},
        {PrintStartPhase::HEATING_BED, "Heating Bed..."},
        {PrintStartPhase::SOAKING, "Heat Soaking..."},
        {PrintStartPhase::HEATING_NOZZLE, "Heating Nozzle..."},
        {PrintStartPhase::QGL, "Leveling Gantry..."},
        {PrintStartPhase::Z_TILT, "Z Tilt Adjust..."},
        {PrintStartPhase::BED_MESH, "Loading Bed Mesh..."},
        {PrintStartPhase::CLEANING, "Cleaning Nozzle..."},
        {PrintStartPhase::PURGING, "Purging..."},
        {PrintStartPhase::COMPLETE, "Starting Print..."},
    };
    auto it = kMessages.find(*phase);
    return {*phase, it != kMessages.end() ? it->second : ""};
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
        lv_subject_set_int(state_.print_state().get_print_active_subject(), 1);
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
            lv_subject_get_int(state_.print_state().get_print_start_phase_subject()));
    }

    /**
     * @brief Get current print start message from PrinterState subject
     */
    std::string get_current_message() {
        return lv_subject_get_string(state_.print_state().get_print_start_message_subject());
    }

    /**
     * @brief Set bed temperature and target in PrinterState subjects
     *
     * Values are in decidegrees (temp * 10) as stored in PrinterState.
     * Example: 60.0C = 600 decidegrees
     */
    void set_bed_temps(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state_.temperature_state().get_bed_temp_subject(), temp_decideg);
        lv_subject_set_int(state_.temperature_state().get_bed_target_subject(), target_decideg);
    }

    /**
     * @brief Set extruder temperature and target in PrinterState subjects
     */
    void set_extruder_temps(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state_.temperature_state().get_active_extruder_temp_subject(),
                           temp_decideg);
        lv_subject_set_int(state_.temperature_state().get_active_extruder_target_subject(),
                           target_decideg);
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
        lv_subject_set_int(state_.print_state().get_print_progress_subject(), progress);
        lv_subject_set_int(state_.print_state().get_print_layer_current_subject(), layer);
    }

    /**
     * @brief Process pending async UI updates
     *
     * Since set_print_start_state() defers subject updates through the UpdateQueue,
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
        lv_subject_set_int(state_.print_state().get_print_active_subject(), 1);

        collector_->reset();
        drain_async_updates(); // Process reset()'s queued INITIALIZING state
        state_.print_state().reset_print_start_state(); // Override back to IDLE
        drain_async_updates();                          // Process the IDLE update
    }

    /// One fallback check as the ETA timer runs it, with the updates it queues applied.
    void tick_fallbacks() {
        collector_->check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
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

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "PrintStartCollector: each HELIX:PHASE signal shows its phase's banner",
                 "[print][collector][helix_phase]") {
    collector().start();
    drain_async_updates();
    drain_async_updates();

    const std::pair<const char*, const char*> signals[] = {
        {"HOMING", "Homing..."},
        {"HEATING_BED", "Heating Bed..."},
        {"SOAKING", "Heat Soaking..."},
        {"HEATING_NOZZLE", "Heating Nozzle..."},
        {"QGL", "Leveling Gantry..."},
        {"Z_TILT", "Z Tilt Adjust..."},
        {"BED_MESH", "Loading Bed Mesh..."},
        {"CLEANING", "Cleaning Nozzle..."},
        {"PURGING", "Purging..."},
        {"COMPLETE", "Starting Print..."},
    };
    for (const auto& [name, banner] : signals) {
        CAPTURE(name);
        send_gcode_response(std::string("HELIX:PHASE:") + name);
        drain_async_updates();
        drain_async_updates();
        CHECK(get_current_message() == banner);
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
                 "PrintStartCollector: Voron display text names QGL after the first clean",
                 "[print][collector][voron]") {
    collector().start();
    drain_async_updates();
    drain_async_updates();

    // The Voron V2.4 START_PRINT's SET_DISPLAY_TEXT sequence: it cleans the
    // nozzle before and after leveling the gantry.
    const auto display = [&](const char* message) {
        client().dispatch_status_update({{"display_status", {{"message", message}}}});
        drain_async_updates();
        drain_async_updates();
    };

    display("Homing");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    display("Cleaning nozzle");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    display("Leveling gantry");
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);
    REQUIRE(get_current_message() == "Leveling Gantry...");
    display("Cleaning nozzle");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    display("Performing bed mesh calibration");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    display("Purging");
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
}

// ============================================================================
// Host-Side Pre-Start Echo Suppression
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Pre-start echo: the dispatched block's console echo claims no phase",
                 "[print][collector][1571]") {
    collector().start();
    drain_async_updates();
    drain_async_updates(); // Process start()'s INITIALIZING state update

    // The K1C bed_mesh pre-start option from printer_database.json, values
    // rendered. Its echo collides with the default profile three times:
    // CX_ROUGH_G28 contains G28 (HOMING), CX_NOZZLE_CLEAR (CLEANING), and
    // BED_MESH_CALIBRATE (BED_MESH) — none of which the printer has run.
    const std::string block = "CX_ROUGH_G28 EXTRUDER_TEMP=240 BED_TEMP=60\n"
                              "CX_NOZZLE_CLEAR\n"
                              "ACCURATE_G28\n"
                              "BED_MESH_CALIBRATE\n"
                              "PRINT_PREPARED";
    collector().note_host_side_pre_start(block);

    SECTION("verbatim whole-block echo") {
        send_gcode_response(block);
        drain_async_updates();
        CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
    }

    SECTION("per-line echo of a multi-line block") {
        send_gcode_response("CX_ROUGH_G28 EXTRUDER_TEMP=240 BED_TEMP=60");
        drain_async_updates();
        CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
        send_gcode_response("CX_NOZZLE_CLEAR");
        drain_async_updates();
        CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
        send_gcode_response("BED_MESH_CALIBRATE");
        drain_async_updates();
        CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
    }

    // Positive control: the printer's own narration in the same window still
    // claims its phase through the same collector, so the checks above cannot
    // pass merely because nothing matched at all.
    send_gcode_response("M190 S60");
    drain_async_updates();
    CHECK(get_current_phase() == PrintStartPhase::HEATING_BED);
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
        // kTempToleranceDecidegrees = 50 (5C)
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
// Temperature Tolerance Edge Cases (kTempToleranceDecidegrees = 50)
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
                 "Fallback completion: layer count does not trigger COMPLETE",
                 "[print][collector][fallback][completion]") {
    // Layer count is not a completion signal: authoritative signals (a RESPOND
    // match and Moonraker state=printing) handle dismissal. These sections
    // verify a layer count alone never completes the pre-print.

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
        lv_subject_set_int(state().print_state().get_print_layer_current_subject(), 250);
        collector().start();
        drain_async_updates();
        REQUIRE_FALSE(collector().has_seen_layer_zero());
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Fallback completion: progress threshold does not trigger COMPLETE",
                 "[print][collector][fallback][completion]") {
    // Print progress is not a completion signal: authoritative signals handle
    // dismissal. These sections verify a progress threshold alone never
    // completes the pre-print.

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

/// A sequential profile narrated by "// State:" lines, with a phase object
/// matched on its bare state and two position rules. The engine's
/// sequential-progress, unqualified phase-object and status-signal behaviour
/// is pinned against it.
static const char* const kSequentialStateProfile = R"JSON(
{
  "name": "Sequential states",
  "description": "Sequential test profile",
  "progress_mode": "sequential",
  "signal_formats": [
    {
      "prefix": "// State: ",
      "mappings": {
        "PREPARING...":         { "phase": "INITIALIZING",    "message": "Preparing...",             "progress": 3 },
        "MD5 CHECK":            { "phase": "INITIALIZING",    "message": "Verifying file...",        "progress": 5 },
        "HOMING...":            { "phase": "HOMING",          "message": "Homing axes...",           "progress": 10 },
        "PREPARE CLEANING...":  { "phase": "CLEANING",        "message": "Preparing to clean...",    "progress": 15 },
        "HEATING...":           { "phase": "HEATING_BED",     "message": "Heating...",               "progress": 25 },
        "CLEANING START SOON":  { "phase": "CLEANING",        "message": "Starting nozzle clean...", "progress": 30 },
        "CLEANING...":          { "phase": "CLEANING",        "message": "Cleaning nozzle...",       "progress": 38 },
        "COOLING DOWN...":      { "phase": "CLEANING",        "message": "Cooling for probing...",   "progress": 45 },
        "FINISHING CLEANING...":{ "phase": "CLEANING",        "message": "Finishing clean...",        "progress": 55 },
        "DONE!":                { "phase": "CLEANING",        "message": "Nozzle clean complete",    "progress": 57 },
        "KAMP LEVELING...":     { "phase": "BED_MESH",        "message": "Creating bed mesh...",     "progress": 60 },
        "WAIT FOR TEMPERATURE...": { "phase": "HEATING_NOZZLE", "message": "Reheating...",           "progress": 82 },
        "KAMP PRIMING...":      { "phase": "PURGING",         "message": "Priming nozzle...",        "progress": 90 },
        "PRINTING...":          { "phase": "COMPLETE",         "message": "Starting print...",        "progress": 100 }
      }
    }
  ],
  "response_patterns": [
    {
      "pattern": "// Wait bed temperature to reach (\\d+)",
      "phase": "HEATING_BED",
      "message": "Heating bed to $1\u00b0C...",
      "weight": 15
    },
    {
      "pattern": "// Wait extruder temperature to reach (\\d+)",
      "phase": "HEATING_NOZZLE",
      "message": "Heating nozzle to $1\u00b0C...",
      "weight": 15
    }
  ],
  "phase_object": {
    "object": "operation_context",
    "field": "current_state"
  },
  "state_patterns": [
    {
      "pattern": "HOMING",
      "phase": "HOMING",
      "message": "Homing...",
      "weight": 5
    },
    {
      "pattern": "HEATING",
      "phase": "HEATING_BED",
      "message": "Heating Bed...",
      "weight": 15
    },
    {
      "pattern": "CLEANING",
      "phase": "CLEANING",
      "message": "Cleaning Nozzle...",
      "weight": 20
    },
    {
      "pattern": "LEVELING",
      "phase": "BED_MESH",
      "message": "Bed Mesh...",
      "weight": 25
    },
    {
      "pattern": "MESHING",
      "phase": "BED_MESH",
      "message": "Bed Mesh...",
      "weight": 25
    },
    {
      "pattern": "PARKING",
      "phase": "INITIALIZING",
      "message": "Preparing...",
      "weight": 5
    },
    {
      "pattern": "PRIMING",
      "phase": "PURGING",
      "message": "Priming nozzle...",
      "weight": 10
    },
    {
      "pattern": "PURGING",
      "phase": "PURGING",
      "message": "Priming nozzle...",
      "weight": 10
    },
    {
      "pattern": "PRINTING",
      "phase": "COMPLETE",
      "message": "Starting print...",
      "weight": 100
    },
    {
      "pattern": "FINISHING",
      "phase": "COMPLETE",
      "message": "Starting print...",
      "weight": 100
    }
  ],
  "status_signals": [
    {
      "name": "at_cutter",
      "object": "toolhead",
      "when": [
        { "field": "position", "index": 0, "op": "lt", "value": 0.0 },
        { "field": "position", "index": 1, "op": "lt", "value": 0.0 }
      ],
      "phase": "PURGING",
      "message": "Changing filament...",
      "weight": 30
    },
    {
      "name": "at_chute",
      "object": "toolhead",
      "when": [
        { "field": "position", "index": 1, "op": "gt", "value": 225.0 }
      ],
      "phase": "PURGING",
      "message": "Purging...",
      "weight": 20
    }
  ]
}
)JSON";

/**
 * @brief Test fixture for sequential-profile progress tests
 *
 * Loads kSequentialStateProfile and provides helpers for dispatching G-code
 * responses and reading progress values.
 */
class PrintStartCollectorSequentialFixture : public LVGLTestFixture {
  public:
    PrintStartCollectorSequentialFixture() {
        state_.init_subjects(false);
        // Mark print as active so set_print_start_state() accepts phase updates
        lv_subject_set_int(state_.print_state().get_print_active_subject(), 1);
        client_ = std::make_unique<MoonrakerClientMock>();
        collector_ = std::make_shared<PrintStartCollector>(*client_, state_);
        auto profile =
            PrintStartProfileTestAccess::parse(nlohmann::json::parse(kSequentialStateProfile));
        REQUIRE(profile != nullptr);
        collector_->set_profile(std::move(profile));
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
        return lv_subject_get_int(state_.print_state().get_print_start_progress_subject());
    }

    PrintStartPhase get_current_phase() {
        return static_cast<PrintStartPhase>(
            lv_subject_get_int(state_.print_state().get_print_start_phase_subject()));
    }

    std::string get_current_message() {
        return lv_subject_get_string(state_.print_state().get_print_start_message_subject());
    }

    void send_gcode_response(const std::string& line) {
        json msg = {{"method", "notify_gcode_response"}, {"params", {line}}};
        client().dispatch_method_callback("notify_gcode_response", msg);
        drain_async_updates();
    }

    /**
     * @brief Feed a status frame through the real notify_status_update path
     *
     * dispatch_status_update wraps the object exactly as the live WebSocket
     * does, so the collector's status subscription runs its phase-object
     * handling on it.
     */
    void send_status_update(const json& object) {
        client().dispatch_status_update(object);
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
    const int provisional =
        lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    REQUIRE(provisional > 0);

    // Heater targets land (K1C CX_ROUGH_G28 stage: nozzle to probe temp, bed
    // to print temp) and the subject observer path runs the recompute.
    set_all_temps(250, 550, 500, 1300);
    collector().check_fallback_completion();
    drain_async_updates();

    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    const int corrected =
        lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    REQUIRE(corrected > provisional);

    // The corrected estimate is the new anchor: without further input changes
    // remaining must not creep back up on later ticks.
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    const int settled = lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    REQUIRE(settled <= corrected);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Print details estimate and the countdown total agree",
                 "[print][collector][eta][estimate]") {
    // Same printer, same temperatures, same targets: the number the details
    // screen shows before Print is the total the countdown starts from.
    PreprintConfigScope config;
    auto& rates = ThermalRateManager::instance();
    rates.reset();
    rates.get_model("extruder").set_default_rate(0.5f);
    rates.get_model("heater_bed").set_default_rate(2.0f);

    set_all_temps(250, 600, 250, 2200); // bed 25/60C, nozzle 25/220C
    PrintPreparationManager details;
    details.set_dependencies(nullptr, &state());
    FileMetadata file;
    file.first_layer_bed_temp = 60.0;
    file.first_layer_extr_temp = 220.0;
    details.recalculate_estimate(&file, "countdown.gcode");
    const int shown = lv_subject_get_int(details.get_preprint_estimate_subject());

    collector().start();
    const float countdown_total = PrintStartCollectorTestAccess::get_predicted_total(collector());

    REQUIRE(shown > 300);
    REQUIRE(shown == static_cast<int>(countdown_total));
    rates.reset();
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "With a phase toggled off, print details and the countdown still agree",
                 "[print][collector][eta][estimate]") {
    PreprintConfigScope config;
    const int mesh = static_cast<int>(PrintStartPhase::BED_MESH);
    for (int64_t stamp : {1000, 2000}) {
        helix::PreprintEntry e;
        e.total_seconds = 400;
        e.timestamp = stamp;
        e.temp_bucket = 1;
        e.window = helix::PreprintWindow::PrinterEdge;
        e.phase_durations = {{static_cast<int>(PrintStartPhase::HOMING), 20}, {mesh, 150}};
        helix::PreprintPredictor::append_to_config(e);
    }

    set_all_temps(250, 600, 250, 2200); // bed 25/60C, nozzle 25/220C
    PrintPreparationManager details;
    details.set_dependencies(nullptr, &state());
    details.set_option_state_provider(
        [](const std::string& id) { return id == "bed_mesh" ? 0 : -1; });
    FileMetadata file;
    file.first_layer_bed_temp = 60.0;
    file.first_layer_extr_temp = 220.0;
    details.recalculate_estimate(&file, "countdown.gcode");
    const int shown = lv_subject_get_int(details.get_preprint_estimate_subject());
    REQUIRE(shown == 400 - 150);

    collector().start();
    REQUIRE(PrintStartCollectorTestAccess::get_predicted_total(collector()) == 400.0f);
    collector().note_phase_overrides({{mesh, false}});
    CHECK(static_cast<int>(PrintStartCollectorTestAccess::get_predicted_total(collector())) ==
          shown);
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
    const int probe_stage =
        lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    REQUIRE(probe_stage > 0);

    // Firmware raises the nozzle to print temp; nozzle temp is still well
    // below it, so the heating phase keeps real weight.
    set_all_temps(250, 550, 500, 2200);
    collector().check_fallback_completion();
    drain_async_updates();
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    const int print_stage =
        lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
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
    const int remaining =
        lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    REQUIRE(remaining > 150);

    // And the bar (total - remaining) must not front-load: 20s into a 300s
    // prep it has no business showing a third.
    const int progress =
        lv_subject_get_int(state().print_state().get_print_start_progress_subject());
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
    const int remaining =
        lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    REQUIRE(remaining > 100);
}

// ============================================================================
// MESH ETA EXTRAPOLATION TESTS
//
// AD5X capture in bundle SQJ8SAL7: predictor history never recorded a
// BED_MESH phase, so the weights map carried no mesh key — the mesh under
// way contributed nothing to the estimate and the countdown stayed pinned at
// the pre-telemetry seed (~6s) for a 160s mesh. A phase the printer runs but
// history never mapped still owes its live per-probe extrapolation.
// ============================================================================

namespace {

/// Start monitoring with deterministic predictor history. Temps sit at
/// target when the heater targets arrive, so the thermal model adds no
/// duration and `durations` become the weights verbatim; the 0→set target
/// rise triggers the recompute that picks the loaded history up.
void start_with_mesh_eta_history(PrintStartCollectorHeaterFixture& f, std::map<int, int> durations,
                                 int total_seconds) {
    f.set_all_temps(250, 0, 500, 0); // no heater targets yet
    f.collector().start();
    f.collector().enable_fallbacks();
    PrintStartCollectorTestAccess::clear_prediction_history(f.collector());

    helix::PreprintEntry e;
    e.total_seconds = total_seconds;
    e.timestamp = 1000;
    e.temp_bucket = 1;
    e.phase_durations = std::move(durations);
    PrintStartCollectorTestAccess::load_prediction_entries(f.collector(), {e});

    auto& rates = ThermalRateManager::instance();
    rates.get_model("extruder").set_default_rate(1.0f);
    rates.get_model("heater_bed").set_default_rate(1.0f);
    f.drain_async_updates();

    // Targets arrive at the current temperatures: zero degrees of climb.
    f.set_all_temps(250, 250, 500, 500);
    f.tick_fallbacks();
}

/// Home, then enter the mesh phase — the markers a stock macro echoes.
void enter_mesh_via_markers(PrintStartCollectorHeaterFixture& f) {
    f.send_gcode_response("G28");
    f.send_gcode_response("BED_MESH_CALIBRATE");
}

/// Two "Probing point N/25" lines `interval` seconds apart: probe #2 arms the
/// per-probe timing telemetry at interval s/probe with 23 probes left.
void arm_probe_telemetry(PrintStartCollectorHeaterFixture& f,
                         helix::sim::SimulatedClock::ManualScope& clock, int interval_s = 6) {
    f.send_gcode_response("Probing point 1/25");
    clock.advance(std::chrono::seconds(interval_s));
    f.send_gcode_response("Probing point 2/25");
}

int published_remaining(PrintStartCollectorHeaterFixture& f) {
    return lv_subject_get_int(f.state().print_state().get_preprint_remaining_subject());
}

} // namespace

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Mesh phase absent from predictor history still counts down via probe "
                 "extrapolation",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    // History maps homing and purging but never BED_MESH. Durations 32+32
    // make each weight exactly 0.5 of the 256s recorded total.
    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    enter_mesh_via_markers(*this);
    arm_probe_telemetry(*this, clock);

    // Assert the setup armed the telemetry before asserting on its output.
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 25);
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 2);

    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    // 23 probes left at 6s each (138s) plus the future purge phase (128s).
    // A mesh key missing from the weights must still owe its extrapolation;
    // only the purge term publishes otherwise.
    REQUIRE(published_remaining(*this) == 266);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Mesh extrapolation overrides the pre-telemetry seed once telemetry arms",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    enter_mesh_via_markers(*this);

    // Two ticks before probe #2 arms the telemetry: the mesh contributes
    // nothing yet, so both publish the future-purge term alone and the second
    // seeds the monotonic anchor with it.
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 128);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 128);
    REQUIRE(PrintStartCollectorTestAccess::get_last_remaining(collector()) == 128);

    arm_probe_telemetry(*this, clock);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    // The seed a pre-telemetry publish left must not pin the first honest
    // extrapolation: 138s of mesh + 128s purge publishes raw, not clamped
    // back to 128.
    REQUIRE(published_remaining(*this) == 266);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Mesh extrapolation with the phase in weights counts once and eases the seed",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    // History DOES map BED_MESH: durations 32/64/32 sum to 128 and the
    // recorded total equals that sum, so each weight*total reproduces its
    // recorded duration exactly.
    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::BED_MESH), 64},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                128);
    enter_mesh_via_markers(*this);

    SECTION("no telemetry: the historical mesh duration still applies") {
        PrintStartCollectorTestAccess::run_eta_update(collector());
        drain_async_updates();
        // 64s of predicted mesh (no probes seen yet) + 32s purge.
        REQUIRE(published_remaining(*this) == 96);
    }

    SECTION("telemetry armed: extrapolation replaces the historical duration once") {
        arm_probe_telemetry(*this, clock);
        PrintStartCollectorTestAccess::run_eta_update(collector());
        drain_async_updates();
        // 23 probes at 6s replace the 64s prediction; counting the
        // extrapolation on top of the weight would publish 308.
        REQUIRE(published_remaining(*this) == 170);
    }

    SECTION("telemetry armed after a seed: the anchor releases here too") {
        PrintStartCollectorTestAccess::run_eta_update(collector());
        drain_async_updates();
        REQUIRE(published_remaining(*this) == 96);
        arm_probe_telemetry(*this, clock);
        PrintStartCollectorTestAccess::run_eta_update(collector());
        drain_async_updates();
        // The anchor release must fire for the in-weights extrapolation
        // branch as well, not only the unrecorded-phase one.
        REQUIRE(published_remaining(*this) == 170);
    }
}

// ============================================================================
// ANCHOR-RELEASE RE-ARMING — every mesh segment gets its own release
//
// The extrapolation anchor release is one-shot per mesh SEGMENT: the flag is
// cleared wherever the probe telemetry is zeroed (phase change, sub-phase
// message change, >30s probe gap, per run), so the second segment's first
// honest extrapolation publishes raw instead of being clamped to whatever
// anchor the first segment (or a provisional seed) left behind. Every case
// below arms a fast first segment (6s/probe -> 266s), then re-arms a slower
// second segment (12s/probe -> 404s honest) and requires the raw 404: with
// the flag left armed the monotonic clamp would freeze the countdown at the
// first segment's value.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "mesh extrapolation anchor releases again on BED_MESH re-entry",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    enter_mesh_via_markers(*this);
    arm_probe_telemetry(*this, clock, 6);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 266);

    // Leave the mesh for a heating hop (a new phase always updates, even
    // backwards), then re-enter BED_MESH: the re-entry must clear the
    // extrapolation flag along with the probe telemetry. The seed publish
    // below (telemetry empty, purge term only) re-arms the monotonic anchor
    // at 128 — a flag left armed would clamp the second segment's honest 404
    // back to that seed.
    send_gcode_response("M190 S60");
    send_gcode_response("BED_MESH_CALIBRATE");
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 128);

    arm_probe_telemetry(*this, clock, 12);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    // 23 probes at 12s (276s) + the 128s purge, published raw.
    REQUIRE(published_remaining(*this) == 404);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "mesh extrapolation anchor releases again on sub-phase message change",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    // A profile with two BED_MESH markers carrying different messages, the way
    // multi-segment mesh firmware (Snapmaker U1's inspect -> level hops) does.
    // G28 is included so homing registers as completed and stops owing its
    // weight from the first publish on.
    collector().set_profile(PrintStartProfileTestAccess::parse(nlohmann::json{
        {"name", "mesh-subphase-test"},
        {"response_patterns",
         nlohmann::json::array({
             nlohmann::json{{"pattern", "G28"}, {"phase", "HOMING"}, {"message", "Homing..."}},
             nlohmann::json{{"pattern", "MESH_SEGMENT_A"},
                            {"phase", "BED_MESH"},
                            {"message", "First mesh segment..."}},
             nlohmann::json{{"pattern", "MESH_SEGMENT_B"},
                            {"phase", "BED_MESH"},
                            {"message", "Second mesh segment..."}},
         })},
    }));

    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    send_gcode_response("G28");
    send_gcode_response("MESH_SEGMENT_A");
    arm_probe_telemetry(*this, clock, 6);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 266);

    // Same phase, new message: the sub-phase reset zeroes the telemetry and
    // must re-arm the release along with it.
    send_gcode_response("MESH_SEGMENT_B");
    arm_probe_telemetry(*this, clock, 12);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    REQUIRE(published_remaining(*this) == 404);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "mesh extrapolation anchor releases again after a probe gap reset",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    enter_mesh_via_markers(*this);
    arm_probe_telemetry(*this, clock, 6);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 266);

    // >30s of silence, then a fresh probe burst: the gap reset treats the
    // earlier probes as a different operation and must re-arm the release.
    clock.advance(std::chrono::seconds(31));
    arm_probe_telemetry(*this, clock, 12);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    REQUIRE(published_remaining(*this) == 404);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "mesh extrapolation anchor releases again after a probe-at gap reset",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    enter_mesh_via_markers(*this);
    // The "probe at X,Y" fallback arm: no per-probe progress lines, so the
    // total comes from the adaptive-mesh echo and the timing from two unique
    // points 6s apart (2 points -> 23 of 25 left at 6s = 138s mesh + 128s).
    send_gcode_response("// Adapted probe count: 5,5");
    send_gcode_response("probe at 10.0,10.0 is z=0.100");
    clock.advance(std::chrono::seconds(6));
    send_gcode_response("probe at 20.0,10.0 is z=0.110");
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 25);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 266);

    // >30s of silence, then a new pair of unique points: the dedupe-path gap
    // reset clears the counted points and must re-arm the release.
    clock.advance(std::chrono::seconds(31));
    send_gcode_response("probe at 30.0,30.0 is z=0.090");
    clock.advance(std::chrono::seconds(12));
    send_gcode_response("probe at 40.0,30.0 is z=0.095");
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();

    REQUIRE(published_remaining(*this) == 404);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "mesh extrapolation anchor releases on a second collector run",
                 "[print][collector][eta]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    // Run 1 arms the one-shot release; run 2 must start with it cleared, or a
    // seeded anchor from the new run clamps its first extrapolation forever.
    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    enter_mesh_via_markers(*this);
    arm_probe_telemetry(*this, clock, 6);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 266);

    collector().stop();
    // The helper re-zeros the heater targets before its own start(), so the
    // restarted collector's weight compute sees no targets and the target
    // rise inside the helper re-triggers the recompute onto this history.
    start_with_mesh_eta_history(*this,
                                {{static_cast<int>(PrintStartPhase::HOMING), 32},
                                 {static_cast<int>(PrintStartPhase::PURGING), 32}},
                                256);
    // The per-run reset must leave the release disarmed: entering the next
    // mesh clears it too, so only reading the latch here pins the reset
    // itself rather than the entry clear.
    REQUIRE_FALSE(PrintStartCollectorTestAccess::is_mesh_extrapolation_armed(collector()));

    // Seed the anchor the way a quiet pre-mesh stretch does (purge term only),
    // then arm a slower mesh segment: the fresh run's release must let the
    // honest 404 through instead of clamping to the 128s seed.
    enter_mesh_via_markers(*this);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 128);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(PrintStartCollectorTestAccess::get_last_remaining(collector()) == 128);

    arm_probe_telemetry(*this, clock, 12);
    PrintStartCollectorTestAccess::run_eta_update(collector());
    drain_async_updates();
    REQUIRE(published_remaining(*this) == 404);
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

    // Force no predictions so kFallbackTimeout (300s) applies directly
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Bed at target (105°C), nozzle target = 0 (M109 not issued yet)
    set_all_temps(1050, 1050, 500, 0);
    // Simulate 400s elapsed (well past kFallbackTimeout of 300s)
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

    // Force no predictions so kFallbackTimeout (300s) applies directly
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Both heaters at their targets (within 2°C)
    set_all_temps(1050, 1050, 2640, 2650);
    // Simulate 310s elapsed (past kFallbackTimeout of 300s)
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

    // Both heaters at their targets
    set_all_temps(1050, 1050, 2650, 2650);

    SECTION("Does not fire before adaptive timeout") {
        // 400s elapsed — past kFallbackTimeout but before adaptive (750s)
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

    // Predicted total 400s: the ceiling is max(400*2.5, 1800s), above the
    // longest legitimate pre-print (the K2 Plus runs ~1140s: heat, ~390s mesh,
    // purge).
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

    // Force no predictions — kFallbackTimeout (300s) applies
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Nozzle target = 0 — temps_near stays false, kFallbackTimeout won't fire
    set_all_temps(1050, 1050, 500, 0);

    SECTION("FALLBACK_TIMEOUT does not fire with unknown nozzle target") {
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("ABSOLUTE_MAX_TIMEOUT fires as hard ceiling") {
        // Past the 1800s ceiling with nothing said for as long, whatever the
        // temperatures read.
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
                 "Nozzle within 2C of its target counts as at target",
                 "[print][collector][timeout]") {
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // Force no predictions so kFallbackTimeout (300s) applies directly
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    // Nozzle at 264°C with target 265°C: inside the 2°C at-target band.
    set_all_temps(1050, 1050, 2640, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

/**
 * A heater short of its target is still heating, however far past the
 * prediction the clock has run, so the timeout waits for the target itself
 * rather than a fraction of it. Numbers are a Centauri Carbon cold start
 * predicted at 180s: 390s in, its 105C bed reads 94.6 with most of a ~620s
 * pre-print still to run.
 */
TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Timeout fallback waits for the heaters to reach their targets",
                 "[print][collector][timeout]") {
    const auto tick_at = [this](int bed_decideg, int elapsed_s) {
        set_all_temps(bed_decideg, 1050, 1400, 1400);
        collector().start();
        drain_async_updates();
        reset_collector_to_idle();
        collector().enable_fallbacks();
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), elapsed_s);
        // A heating bed takes the proactive branch on the first tick, which
        // returns before the completion checks; the second tick reaches them.
        tick_fallbacks();
        tick_fallbacks();
    };

    SECTION("A bed at 94.6 of 105 is still heating") {
        tick_at(946, 390);
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    }

    SECTION("A bed within 2C of its target completes") {
        tick_at(1040, 390);
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }

    SECTION("A bed that never settles within 2C is not at target") {
        // 100 of 105 is inside the 5C band the proactive detector calls
        // heating, and outside the 2C band that counts as at target.
        tick_at(1000, 1000);
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("A bed that never settles within 2C completes at the ceiling") {
        tick_at(1000, 1850);
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A heater still closing on its target counts as activity",
                 "[print][collector][timeout]") {
    // M190 waits out the last degree or two with nothing on the console, and
    // that stretch is already inside the at-target band.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1020, 1050, 1400, 1400);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    tick_fallbacks(); // 102.0: short of the band
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    set_all_temps(1035, 1050, 1400, 1400); // 103.5: in the band and still rising
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    // Once it stops rising and the printer stays quiet, the timeout is back.
    clock.advance(std::chrono::seconds(91));
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A heater cycling under its target is not climbing",
                 "[print][collector][timeout]") {
    // A bed held near its target swings a degree or so for as long as it is
    // held. Only a reading above the highest yet under the current target is
    // progress toward it.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(950, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    const auto tick_after = [&](int seconds, int bed, int bed_target) {
        clock.advance(std::chrono::seconds(seconds));
        set_all_temps(bed, bed_target, 2650, 2650);
        tick_fallbacks();
    };

    // The approach, each reading a new high: 95.0, 99.0, 103.0 of 105.
    tick_after(0, 950, 1050);
    tick_after(30, 990, 1050);
    tick_after(30, 1030, 1050);
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    SECTION("a swing back up to the highest reading is not a climb") {
        tick_after(60, 1020, 1050);
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
        tick_after(60, 1030, 1050);
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }

    SECTION("a new high under the target is still a climb") {
        tick_after(60, 1020, 1050);
        tick_after(60, 1040, 1050);
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
        tick_after(91, 1040, 1050);
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }

    SECTION("a new target starts its own highest reading") {
        // Held at 104.0, set to cool toward 60, then set back to 105.
        tick_after(40, 1040, 1050);
        tick_after(40, 1000, 600);
        tick_after(40, 1000, 1050);
        tick_after(40, 1010, 1050);
        tick_after(40, 1030, 1050);
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "The ceiling ends a pre-print whose heater never settles",
                 "[print][collector][timeout][ceiling]") {
    // A heater that keeps climbing without reaching its target holds every
    // other timeout open. The ceiling ignores it.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1000, 1050, 1400, 1400);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("with a prediction") {
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    }
    SECTION("without a prediction") {
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    }

    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1790);
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    // Past the 1800s ceiling, a degree warmer than the last reading.
    clock.advance(std::chrono::seconds(20));
    set_all_temps(1010, 1050, 1400, 1400);
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "The ceiling waits for a narrating printer to go quiet",
                 "[print][collector][timeout][ceiling]") {
    // A long chamber soak says so more often than every 90s. The bed never
    // comes within 2C of its target, so only the ceiling can end this
    // pre-print, and a printer still narrating is not stuck.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1000, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    SECTION("with a prediction") {
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    }
    SECTION("without a prediction") {
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    }
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1700);

    const auto tick_after = [&](int seconds, const char* line) {
        clock.advance(std::chrono::seconds(seconds));
        if (line != nullptr) {
            send_gcode_response(line);
        }
        tick_fallbacks();
    };

    for (int elapsed_s = 1760; elapsed_s <= 2060; elapsed_s += 60) {
        CAPTURE(elapsed_s);
        tick_after(60, "// waiting for chamber");
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    tick_after(60, nullptr);
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    tick_after(31, nullptr);
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A printer that never stops narrating ends at the backstop",
                 "[print][collector][timeout][ceiling]") {
    // At twice the 1800s ceiling the collector stops waiting for quiet too: a
    // firmware that chatters forever still leaves Preparing.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1000, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 3590);

    send_gcode_response("// waiting for chamber");
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    clock.advance(std::chrono::seconds(20));
    send_gcode_response("// waiting for chamber");
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture, "The ceiling stretches for a long prediction",
                 "[print][collector][timeout][ceiling]") {
    // 2.5x a 1000s prediction is 2500s, so a quiet printer whose bed is still
    // creeping up is not cut at 1800s.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1000, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 1000.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1840);
    tick_fallbacks();

    clock.advance(std::chrono::seconds(10));
    set_all_temps(1010, 1050, 2650, 2650);
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 2510);
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A status-signal rule is not a line the ceiling waits out",
                 "[print][collector][timeout][ceiling]") {
    // A rule infers from the same heater frames the collector already reads;
    // the printer said nothing. Past the ceiling it does not hold Preparing
    // open the way a matched line does.
    set_all_temps(1000, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1850);

    // The generic profile's heating_bed rule: a target, and the bed more than 2C short.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 100.0}, {"target", 105.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A status-signal rule holds the timeout like a climbing heater",
                 "[print][collector][timeout]") {
    // The deadline timeouts wait for 90s past any sign of work, and a rule
    // firing is one.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1050, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    client().dispatch_status_update({{"heater_bed", {{"temperature", 23.5}, {"target", 60.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    clock.advance(std::chrono::seconds(91));
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "The generic profile moves a soak out of bed heating and keeps it there",
                 "[print][collector][soak]") {
    REQUIRE(PrintStartProfile::load_default()->name() == "Generic");
    set_all_temps(300, 1050, 2650, 2650);
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    // The generic heating_bed rule enters bed heating on the climb.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 30.0}, {"target", 105.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    SECTION("a soak narrated once the bed arrives") {
        set_bed_temps(1050, 1050);
        client().dispatch_status_update(
            {{"heater_bed", {{"temperature", 105.0}, {"target", 105.0}}}});
        client().dispatch_status_update(
            {{"display_status", {{"message", "Bed soak - 2 minutes remaining"}}}});
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
        REQUIRE(get_current_message() == "Heat Soaking...");

        // The bed sags past the 2C band during the dwell: the heating_bed rule
        // re-arms and holds again, and the temperature tick sees a heating bed.
        set_bed_temps(1020, 1050);
        client().dispatch_status_update(
            {{"heater_bed", {{"temperature", 102.0}, {"target", 105.0}}}});
        drain_async_updates();
        tick_fallbacks();
        CHECK(get_current_phase() == PrintStartPhase::SOAKING);
        CHECK(get_current_message() == "Heat Soaking...");
    }

    SECTION("a chamber wait while the bed is still climbing") {
        send_gcode_response("// waiting for chamber");
        REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);

        // Heater correction relabels only a heating phase, so the live bed
        // shortfall leaves the wait alone.
        set_bed_temps(600, 1050);
        tick_fallbacks();
        CHECK(get_current_phase() == PrintStartPhase::SOAKING);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A timeout completion keeps the heating rates it measured",
                 "[print][collector][timeout][thermal_rate]") {
    PreprintConfigScope config;
    struct ManagerReset {
        ManagerReset() {
            ThermalRateManager::instance().reset();
        }
        ~ManagerReset() {
            ThermalRateManager::instance().reset();
        }
    } manager_reset;

    set_all_temps(300, 1050, 1400, 1400);
    collector().start();
    drain_async_updates();

    // A bed that climbed 60C in 360s measured 6 s/C along the way.
    ThermalRateModel& bed = ThermalRateManager::instance().get_model("heater_bed");
    bed.record_sample(30.0f, 1000);
    bed.record_sample(90.0f, 361000);
    REQUIRE(bed.measured_rate().has_value());

    reset_collector_to_idle();
    collector().enable_fallbacks();
    set_all_temps(1050, 1050, 1400, 1400);
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);

    helix::Config* cfg = helix::Config::get_instance();
    REQUIRE(cfg->get<float>("/thermal/rates/heater_bed/heat_rate", 0.0f) == Catch::Approx(6.0f));
    // Phases a timeout cut short are not history.
    REQUIRE(helix::PreprintPredictor::load_entries_from_config().empty());
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
// K2 gcode tag stream, matched by the K2 profile's tag_patterns; every
// other printer's profile lacks them and falls through.
// ============================================================================

/**
 * @brief The K2 tag stream run under the profile that declares it
 *
 * creality_k2.json carries the purge-percent and box-load tag_patterns.
 * The base sequential fixture's profile deliberately does not.
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
// QGL / bed-mesh conflation — Voron 2.4 PRINT_START runs QUAD_GANTRY_LEVEL
// before BED_MESH_CALIBRATE. QGL probes 4 pads with `samples: 3` (default) =
// 12 `probe at X,Y is z=Z` lines, which must neither enter BED_MESH nor count
// against the bed_mesh probe total and throw the "X/Y" count off.
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

    // Bed and nozzle both well below target, which alone reads as HEATING_BED.
    // The firmware phase stands.
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
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xy");
    set_all_temps(/*bed*/ 250, 550, /*ext*/ 0, 0);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // Klipper clears homed_axes to "" as each axis re-homes during G28. That
    // empty-string blip must NOT flip the label to "Heating Bed" — the HOMING
    // latch holds until the toolhead is actually fully homed.
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "");
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // Once fully homed, proactive may surface the bed heating (still pre-signal).
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
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
// Snapmaker U1: the mesh announces itself in three action codes, a 60 s bed
// soak (BED_PREHEATING), four corner probes (BED_PRESCANNING), then the mesh
// (BED_LEVELING). G28 Z prints "// z offset:" 1 ms after BED_LEVELING; that
// Z touch must not drag the display back to homing, and the corner probes must
// not count as mesh points. Lines from the console of a U1 print on 2026-10-16
// (gcode_store 21:54:52.876 .. 21:55:39.041).
// ============================================================================

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: BED_LEVELING restarts the count the corner pre-scan used",
                 "[print][collector][snapmaker][bed_mesh]") {
    collector().start();
    drain_async_updates();

    feed_gcode("// trigger_mcu_pos: {'stepper_x': -34502, 'stepper_y': -35786}");
    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message() == "Probing Z...");

    feed_gcode("// Success: Set action code BED_PREHEATING");
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
    REQUIRE(get_current_message() == "Heat Soaking...");
    feed_gcode("// Bed preheating: 59s left");
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);

    feed_gcode("// Success: Set action code BED_PRESCANNING");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    for (const char* line : {"// probe at x: 14.999, y: 15.001 is z=-0.053333",
                             "// probe at x: 14.999, y: 15.001 is z=-0.055833",
                             "// probe at x: 254.999, y: 15.001 is z=-0.648333",
                             "// probe at x: 254.999, y: 15.001 is z=-0.649167",
                             "// probe at x: 254.999, y: 255.001 is z=-0.152500",
                             "// probe at x: 254.999, y: 255.001 is z=-0.157500",
                             "// probe at x: 14.999, y: 255.001 is z=-0.100000",
                             "// probe at x: 14.999, y: 255.001 is z=-0.097500"}) {
        feed_gcode(line);
    }
    REQUIRE(get_current_message() == "Validating bed mesh (4)");

    feed_gcode("// Success: Set action code BED_LEVELING");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Bed mesh...");
    REQUIRE(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 0);

    // G28 Z re-bases the mesh; the label stays on the mesh.
    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Bed mesh...");

    feed_gcode("// probe at x: 33.742, y: 28.487 is z=-0.085000");
    feed_gcode("// probe at x: 33.742, y: 28.487 is z=-0.080000");
    REQUIRE(get_current_message() == "Bed mesh (1)");
    feed_gcode("// probe at x: 74.442, y: 28.487 is z=-0.063333");
    REQUIRE(get_current_message() == "Bed mesh (2)");
}

// ============================================================================
// Snapmaker U1: M109/M190 run after DETECT_PLATE with no action code of their
// own, and the bed warms concurrently with the whole announced sequence. The
// heater only owns the label while a wait reports ("B:45.2 /100.0 T0:...",
// once a second), since that wait blocks everything else. A phase signal ends
// the wait at once; otherwise the first fallback tick after the reports stop
// gives the announced phase back. A stray line mid-wait changes nothing.
// ============================================================================

namespace {
/// The print's file behind a ranged-read transfers API, as the wait-line
/// reads see it, for as long as this lives.
class ServedPrint {
  public:
    ServedPrint(MoonrakerClientMock& client, PrinterState& state, const std::string& filename)
        : transfers{client, ""}, api_{client, state, transfers}, previous_(get_moonraker_api()) {
        set_moonraker_api(&api_);
        lv_subject_copy_string(state.print_state().get_print_filename_subject(), filename.c_str());
    }
    ~ServedPrint() {
        set_moonraker_api(previous_);
    }
    ServedPrint(const ServedPrint&) = delete;
    ServedPrint& operator=(const ServedPrint&) = delete;

    ArchiveTransfers transfers;

  private:
    ArchiveMoonrakerAPIMock api_;
    IMoonrakerAPI* previous_;
};

class SnapmakerHeaterWaitFixture : public SnapmakerCollectorFixture {
  public:
    SnapmakerHeaterWaitFixture() {
        collector().start();
        drain_async_updates();
        collector().enable_fallbacks();
        feed_gcode("// Success: Set action code DETECT_PLATE");
    }

    void tick() {
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
    }

    void set_chamber(int temp_decideg, int target_decideg) {
        lv_subject_set_int(state().temperature_state().get_chamber_temp_subject(), temp_decideg);
        lv_subject_set_int(state().temperature_state().get_chamber_target_subject(),
                           target_decideg);
    }

    /// A chamber heater whose backend reports refusals and its own waits.
    void use_appliance_chamber() {
        auto& temps = state().temperature_state();
        temps.set_chamber_heater_name("heater_generic dragonbreath");
        temps.set_chamber_diagnostics_source("dragonbreath", "dragonbreath", "");
    }
    void refuse_chamber_target() {
        state().temperature_state().update_from_status({{"dragonbreath",
                                                         {{"protocol_error_code", "countermanded"},
                                                          {"protocol_error_seq", 1},
                                                          {"protocol_error_target", 60.0}}}});
    }
    void report_for(int seconds) {
        for (int t = 0; t < seconds; ++t) {
            clock_.advance(std::chrono::seconds(1));
            feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
            tick();
        }
    }

    int chamber_wait_flag() {
        return lv_subject_get_int(state().print_state().get_print_start_chamber_wait_subject());
    }

    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};
} // namespace

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a bed wait after plate detect shows Heating Bed until it ends",
                 "[print][collector][snapmaker][heater_wait]") {
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2100, 2100);

    // A bed warming behind the announced phase leaves that phase alone.
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    feed_gcode("B:45.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    REQUIRE(get_current_message() == "Heating Bed...");

    // The last few degrees are inside the tolerance band; the wait still holds.
    clock_.advance(std::chrono::seconds(1));
    set_all_temps(980, 1000, 2100, 2100);
    feed_gcode("B:98.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    SECTION("a Z touch after the wait takes the label") {
        feed_gcode("// z offset: -0.05");
        REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
        REQUIRE(get_current_message() == "Probing Z...");
    }

    SECTION("a stray line keeps the wait; the first tick after the reports stop ends it") {
        feed_gcode("// unrelated firmware chatter");
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:98.5 /100.0 T0:210.0 /210.0");
        tick();
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

        clock_.advance(std::chrono::seconds(4));
        tick();
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
        REQUIRE(get_current_message() == "Detecting plate...");
    }

    SECTION("an action code arriving after the wait wins") {
        feed_gcode("// Success: Set action code PRINT_PREEXTRUDING");
        REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
        // A later wait does not displace priming.
        feed_gcode("B:98.0 /100.0 T0:150.0 /210.0");
        set_all_temps(980, 1000, 1500, 2100);
        tick();
        REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    }
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a wait shows the nozzle while it is short, then the bed",
                 "[print][collector][snapmaker][heater_wait]") {
    // M109 with the bed warming behind it, then M190 straight after.
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 1700, 2100);
    feed_gcode("B:45.0 /100.0 T0:170.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // The bed-first correction for firmware heating phases leaves it alone.
    clock_.advance(std::chrono::seconds(1));
    set_all_temps(460, 1000, 1800, 2100);
    feed_gcode("B:46.0 /100.0 T0:180.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // The report showing the nozzle arrive is the M109's last; only a report
    // after it (the M190's first) moves the label to the bed.
    clock_.advance(std::chrono::seconds(1));
    set_all_temps(480, 1000, 2100, 2100);
    feed_gcode("B:48.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    clock_.advance(std::chrono::seconds(1));
    feed_gcode("B:48.5 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    // A cooling nozzle is not short of its target.
    clock_.advance(std::chrono::seconds(1));
    set_all_temps(500, 1000, 1947, 1700);
    feed_gcode("B:50.0 /100.0 T0:194.7 /170.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a nozzle wait holds its label until the nozzle is within 2C",
                 "[print][collector][snapmaker][heater_wait]") {
    // M109 keeps blocking while the nozzle settles its last few degrees, with
    // the bed far below its target behind it.
    const auto report_for = [&](int nozzle, int seconds) {
        for (int s = 0; s < seconds; ++s) {
            clock_.advance(std::chrono::seconds(1));
            set_all_temps(/*bed*/ 450, 1000, nozzle, 2100);
            char report[64];
            std::snprintf(report, sizeof(report), "B:45.0 /100.0 T0:%.1f /210.0", nozzle / 10.0);
            feed_gcode(report);
        }
        tick();
    };
    for (int nozzle : {1700, 2060, 2070, 2079}) {
        report_for(nozzle, 5);
        INFO("nozzle=" << nozzle);
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }

    // Within 2C the nozzle has arrived; the bed takes the label on a report
    // after the tick that saw it arrive.
    report_for(2085, 1);
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    report_for(2090, 1);
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: an action code right after a wait report keeps its phase",
                 "[print][collector][snapmaker][heater_wait]") {
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2100, 2100);
    feed_gcode("B:45.0 /100.0 T0:210.0 /210.0");
    clock_.advance(std::chrono::milliseconds(500));
    feed_gcode("// Success: Set action code PRINT_SWITCH_CHECKING");
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    clock_.advance(std::chrono::milliseconds(500));
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a report older than the wait cadence does not relabel",
                 "[print][collector][snapmaker][heater_wait]") {
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2100, 2100);
    feed_gcode("B:45.0 /100.0 T0:210.0 /210.0");
    clock_.advance(std::chrono::seconds(10));
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a wait with every heater near target leaves the phase alone",
                 "[print][collector][snapmaker][heater_wait]") {
    set_all_temps(/*bed*/ 990, 1000, /*ext*/ 2080, 2100);
    feed_gcode("B:99.0 /100.0 T0:208.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a chamber wait with the bed and nozzle ready shows the chamber",
                 "[print][collector][snapmaker][heater_wait]") {
    // TEMPERATURE_WAIT on a heater_generic reports only heaters with a gcode
    // id, so the chamber shows up in the live subjects, not in the line.
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    set_chamber(300, 600);
    feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
    REQUIRE(get_current_message() == "Waiting for chamber 30 / 60°C");
    CHECK(chamber_wait_flag() == 1);

    clock_.advance(std::chrono::seconds(5));
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(chamber_wait_flag() == 0);
}

// The U1 sequence behind the chamber prompt: M109 waits for the nozzle, then
// the slicer's TEMPERATURE_WAIT SENSOR="heater_generic dragonbreath"
// MINIMUM=35 waits on a chamber whose heater target the box zeroed. The
// reports never name the chamber, and nothing sets a chamber target.
TEST_CASE_METHOD(
    SnapmakerHeaterWaitFixture,
    "Snapmaker U1: a chamber wait with the heater off shows the chamber and not the nozzle",
    "[print][collector][snapmaker][heater_wait][chamber_wait]") {
    use_appliance_chamber();
    refuse_chamber_target();
    set_chamber(280, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 1500, 2100);
    for (int t = 0; t < 3; ++t) {
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:100.0 /100.0 T0:150.0 /210.0");
        tick();
    }
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);

    // The nozzle arrives; M109 settles and the chamber wait takes over the
    // same once-a-second report.
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    for (int t = 0; t < 3; ++t) {
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
        tick();
    }
    INFO("message=" << get_current_message());
    CHECK(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    CHECK(chamber_wait_flag() == 0);

    for (int t = 0; t < 4; ++t) {
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
        tick();
    }
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
    CHECK(get_current_message() == "Waiting for chamber 28°C");
    CHECK(chamber_wait_flag() == 1);

    SECTION("the label follows the chamber") {
        set_chamber(291, 0);
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
        tick();
        CHECK(get_current_message() == "Waiting for chamber 29°C");
        CHECK(get_current_phase() == PrintStartPhase::SOAKING);
    }

    SECTION("the reports stopping gives the announced phase back") {
        clock_.advance(std::chrono::seconds(5));
        tick();
        CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
        CHECK(chamber_wait_flag() == 0);
    }
}

TEST_CASE_METHOD(
    SnapmakerHeaterWaitFixture,
    "Snapmaker U1: a wait past the nozzle and bed on a printer with no chamber is not one",
    "[print][collector][snapmaker][heater_wait][chamber_wait]") {
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    for (int t = 0; t < 10; ++t) {
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
        tick();
    }
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(chamber_wait_flag() == 0);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "a sensor-only chamber soak is a wait and never the chamber heater's",
                 "[print][collector][heater_wait][chamber_wait]") {
    // A Voron heat soak: TEMPERATURE_WAIT SENSOR="temperature_sensor chamber"
    // MINIMUM=40 with no chamber heater at all.
    state().temperature_state().set_chamber_sensor_name("temperature_sensor chamber");
    set_chamber(300, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    report_for(15);
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(chamber_wait_flag() == 0);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "another wait on a printer with a healthy chamber heater is not the chamber's",
                 "[print][collector][heater_wait][chamber_wait]") {
    // A probe coil warming (TEMPERATURE_WAIT on the probe's sensor) while the
    // chamber heater sits idle with nothing refused.
    use_appliance_chamber();
    set_chamber(300, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    report_for(15);
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(chamber_wait_flag() == 0);
}

// A slicer that sends no M141, or a glue that reports no refusal: the heater
// sits at 0 and TEMPERATURE_WAIT SENSOR="heater_generic dragonbreath" waits.
TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "a TEMPERATURE_WAIT on the chamber heater at target 0 is a chamber stall",
                 "[print][collector][heater_wait][chamber_wait]") {
    use_appliance_chamber();
    set_chamber(280, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    report_for(6);
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);

    SECTION("the line names the chamber heater") {
        PrintStartCollectorTestAccess::answer_wait_line(
            collector(), helix::classify_wait_line(
                             "TEMPERATURE_WAIT SENSOR=\"heater_generic dragonbreath\" MINIMUM=35"));
        report_for(2);
        CHECK(get_current_phase() == PrintStartPhase::SOAKING);
        CHECK(get_current_message() == "Waiting for chamber 28°C");
        CHECK(chamber_wait_flag() == 1);
    }

    SECTION("a passive soak on a separate chamber sensor is not a heater stall") {
        state().temperature_state().set_chamber_sensor_name("temperature_sensor chamber");
        PrintStartCollectorTestAccess::answer_wait_line(
            collector(), helix::classify_wait_line(
                             "TEMPERATURE_WAIT SENSOR=\"temperature_sensor chamber\" MINIMUM=40"));
        report_for(2);
        CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
        CHECK(chamber_wait_flag() == 0);
    }

    SECTION("a probe coil's wait is not the chamber's") {
        PrintStartCollectorTestAccess::answer_wait_line(
            collector(),
            helix::classify_wait_line(
                "TEMPERATURE_WAIT SENSOR=\"temperature_probe beacon_coil\" MINIMUM=40"));
        report_for(2);
        CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
        CHECK(chamber_wait_flag() == 0);
    }
}

// The read goes through the real query and ranged download. Klipper reports
// file_position at the START of a line until its command returns, so the
// waiting line is the one that starts there, not the one that ends there.
TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "the chamber wait line is read at the print position, not before it",
                 "[print][collector][heater_wait][chamber_wait][wait_line]") {
    const std::string file = "G28\nM106 S0\n"
                             "TEMPERATURE_WAIT SENSOR=\"heater_generic dragonbreath\" MINIMUM=35 "
                             "; comment\nG90\n";
    const size_t pos = file.find("TEMPERATURE_WAIT");
    ArchiveTransfers transfers{client(), ""};
    transfers.put("u1_print.gcode", file);
    ArchiveMoonrakerAPIMock api{client(), state(), transfers};
    IMoonrakerAPI* const previous_api = get_moonraker_api();
    set_moonraker_api(&api);
    struct RestoreApi {
        IMoonrakerAPI* api;
        ~RestoreApi() {
            set_moonraker_api(api);
        }
    } restore{previous_api};
    lv_subject_copy_string(state().print_state().get_print_filename_subject(), "u1_print.gcode");
    client().set_object_status("virtual_sdcard", {{"file_position", pos}});

    use_appliance_chamber();
    set_chamber(280, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    report_for(6);
    REQUIRE(transfers.ranges == 1);
    report_for(2);
    CHECK(get_current_phase() == PrintStartPhase::SOAKING);
    CHECK(chamber_wait_flag() == 1);
}

// The nozzle sits inside the at-target band while the bed is far short, so a
// guess says bed. The command at the print position says otherwise when it
// can be read, and only then.
TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "a heater wait shows the heater its command names, else the guess",
                 "[print][collector][heater_wait][wait_line]") {
    ServedPrint print{client(), state(), "u1_print.gcode"};
    ArchiveTransfers& transfers = print.transfers;
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2090, 2100);

    const auto wait_on = [&](const std::string& command) {
        const std::string file = "G28\n" + command + "\nG90\n";
        transfers.put("u1_print.gcode", file);
        client().set_object_status("virtual_sdcard", {{"file_position", file.find(command)}});
        report_for(2);
    };

    SECTION("M109 is the nozzle") {
        wait_on("M109 S210");
        REQUIRE(transfers.ranges == 1);
        CHECK(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }
    SECTION("a TEMPERATURE_WAIT on an extruder is the nozzle") {
        wait_on("TEMPERATURE_WAIT SENSOR=extruder MINIMUM=205");
        CHECK(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    }
    SECTION("a command that is no wait leaves the guess") {
        wait_on("G4 P1000");
        REQUIRE(transfers.ranges == 1);
        CHECK(get_current_phase() == PrintStartPhase::HEATING_BED);
    }
    SECTION("a failed read leaves the guess, and the wait stops asking") {
        transfers.fail_ranges = true;
        wait_on("M109 S210");
        REQUIRE(transfers.ranges == 1);
        CHECK(get_current_phase() == PrintStartPhase::HEATING_BED);
        const int queries = client().call_count("printer.objects.query");
        report_for(3);
        CHECK(client().call_count("printer.objects.query") == queries);
        CHECK(get_current_phase() == PrintStartPhase::HEATING_BED);
    }
    SECTION("a transfers API without ranged reads is never asked where the print is") {
        transfers.ranged = false;
        const int queries = client().call_count("printer.objects.query");
        wait_on("M109 S210");
        CHECK(client().call_count("printer.objects.query") == queries);
        CHECK(transfers.ranges == 0);
        CHECK(get_current_phase() == PrintStartPhase::HEATING_BED);
    }
    SECTION("the position moving mid-wait reads the next command") {
        const std::string file = "M109 S170\nM190 S100\nG90\n";
        transfers.put("u1_print.gcode", file);
        set_all_temps(/*bed*/ 450, 1000, /*ext*/ 1990, 1700);
        client().set_object_status("virtual_sdcard", {{"file_position", 0}});
        report_for(2);
        REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
        // The reports go on without a gap; one tick reads the move, the next shows it.
        set_all_temps(/*bed*/ 450, 1000, /*ext*/ 1700, 1700);
        client().set_object_status("virtual_sdcard", {{"file_position", file.find("M190")}});
        report_for(2);
        CHECK(transfers.ranges == 2);
        CHECK(get_current_phase() == PrintStartPhase::HEATING_BED);
    }
}

// A Voron-style printer with a chamber sensor and no chamber heater, whose
// [gcode_macro M191] waits on that sensor. M191 there names no heater, so the
// wait is no chamber-heater stall.
TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "M191 on a printer with no chamber heater is not a chamber wait",
                 "[print][collector][heater_wait][chamber_wait][wait_line]") {
    ServedPrint print{client(), state(), "u1_print.gcode"};
    state().temperature_state().set_chamber_sensor_name("temperature_sensor chamber");
    REQUIRE(state().temperature_state().chamber_heater_name().empty());
    const std::string file = "G28\nM191 S45\nG90\n";
    print.transfers.put("u1_print.gcode", file);
    client().set_object_status("virtual_sdcard", {{"file_position", file.find("M191")}});
    set_chamber(300, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    report_for(10);
    REQUIRE(PrintStartCollectorTestAccess::wait_line_kind(collector()) == helix::WaitKind::M191);
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(chamber_wait_flag() == 0);
}

// An answer for a wait that has ended, or for the previous print, describes
// a command no longer running. Both reads are async, so either can land late.
TEST_CASE_METHOD(SnapmakerHeaterWaitFixture, "a wait line answered after its wait ended is dropped",
                 "[print][collector][heater_wait][wait_line]") {
    ServedPrint print{client(), state(), "u1_print.gcode"};
    const std::string file = "M109 S210\nG4 P1000\nG90\n";
    print.transfers.put("u1_print.gcode", file);
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2090, 2100);
    const auto kind = [&]() { return PrintStartCollectorTestAccess::wait_line_kind(collector()); };
    const auto end_wait = [&]() {
        clock_.advance(std::chrono::seconds(5));
        tick();
    };

    SECTION("a ranged read still out when the wait ends") {
        print.transfers.hold_from = 1;
        client().set_object_status("virtual_sdcard", {{"file_position", 0}});
        report_for(1);
        REQUIRE(print.transfers.held.size() == 1);
        end_wait();
        print.transfers.release_held();
        drain_async_updates();
        CHECK_FALSE(kind());
    }

    SECTION("a ranged read still out when the next print starts") {
        print.transfers.hold_from = 1;
        client().set_object_status("virtual_sdcard", {{"file_position", 0}});
        report_for(1);
        REQUIRE(print.transfers.held.size() == 1);
        collector().stop();
        collector().start();
        drain_async_updates();
        print.transfers.release_held();
        drain_async_updates();
        CHECK_FALSE(kind());
    }

    SECTION("a position query still out when the wait ends") {
        client().set_object_status("virtual_sdcard", {{"file_position", 0}});
        client().defer_next("printer.objects.query");
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:45.0 /100.0 T0:209.0 /210.0");
        REQUIRE_FALSE(kind());
        end_wait();
        // The next wait stands at G4. The held answer, fired now, says M109.
        client().set_object_status("virtual_sdcard", {{"file_position", file.find("G4")}});
        report_for(1);
        REQUIRE(kind() == helix::WaitKind::Other);
        client().set_object_status("virtual_sdcard", {{"file_position", 0}});
        client().fire_deferred("printer.objects.query");
        drain_async_updates();
        CHECK(kind() == helix::WaitKind::Other);
    }
}

TEST_CASE("the heater a wait command names", "[print][collector][wait_line]") {
    using helix::classify_wait_line;
    const std::string heater = "heater_generic dragonbreath";
    const auto named = [&](const char* line) {
        return PrintStartCollector::heater_waited_on(classify_wait_line(line), heater);
    };
    CHECK(named("M109 S210") == PrintStartPhase::HEATING_NOZZLE);
    CHECK(named("M109 S170 T2") == PrintStartPhase::HEATING_NOZZLE);
    CHECK(named("M190 S100") == PrintStartPhase::HEATING_BED);
    CHECK(named("M191 S60") == PrintStartPhase::SOAKING);
    CHECK_FALSE(PrintStartCollector::heater_waited_on(classify_wait_line("M191 S60"), ""));
    CHECK(named("TEMPERATURE_WAIT SENSOR=heater_bed MINIMUM=95") == PrintStartPhase::HEATING_BED);
    CHECK(named("TEMPERATURE_WAIT SENSOR=extruder2 MINIMUM=200") ==
          PrintStartPhase::HEATING_NOZZLE);
    CHECK(named("TEMPERATURE_WAIT SENSOR=\"heater_generic dragonbreath\" MINIMUM=35") ==
          PrintStartPhase::SOAKING);
    CHECK_FALSE(named("TEMPERATURE_WAIT SENSOR=\"temperature_sensor chamber\" MINIMUM=40"));
    CHECK_FALSE(named("TEMPERATURE_WAIT SENSOR=\"temperature_probe beacon_coil\" MINIMUM=40"));
    CHECK_FALSE(named("G4 P1000"));
    CHECK_FALSE(PrintStartCollector::heater_waited_on(
        classify_wait_line("TEMPERATURE_WAIT SENSOR=\"heater_generic dragonbreath\" MINIMUM=35"),
        ""));
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "the chamber device's own wait labels the chamber and ends with it",
                 "[print][collector][heater_wait][chamber_wait]") {
    use_appliance_chamber();
    set_chamber(280, 0);
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    state().temperature_state().update_from_status(
        {{"dragonbreath", {{"wait_active", true}, {"wait_target", 60.0}}}});
    report_for(2);
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
    CHECK(get_current_message() == "Waiting for chamber 28 / 60°C");
    CHECK(chamber_wait_flag() == 1);

    // The wait ends (the chamber got hot, or the target was cleared) while
    // the next wait goes on reporting: the chamber is no longer why.
    state().temperature_state().update_from_status(
        {{"dragonbreath", {{"wait_active", false}, {"wait_target", nullptr}}}});
    report_for(1);
    CHECK(chamber_wait_flag() == 0);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: the chamber waits for the bed to reach its target",
                 "[print][collector][snapmaker][heater_wait]") {
    // 3C short is not at target, so the wait is still the bed's.
    set_all_temps(/*bed*/ 970, 1000, /*ext*/ 2100, 2100);
    set_chamber(300, 600);
    feed_gcode("B:97.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a long chamber wait holds the pre-print past its deadline",
                 "[print][collector][snapmaker][heater_wait]") {
    set_all_temps(/*bed*/ 1000, 1000, /*ext*/ 2100, 2100);
    set_chamber(300, 600);
    for (int t = 1; t <= 900; ++t) {
        clock_.advance(std::chrono::seconds(1));
        feed_gcode("B:100.0 /100.0 T0:210.0 /210.0");
        if (t % 5 == 0) {
            tick();
        }
    }
    INFO("phase=" << static_cast<int>(get_current_phase()));
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: before the first real signal a wait report changes nothing",
                 "[print][collector][snapmaker][heater_wait]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    // Proactive detection owns this window: mid-home, it shows Homing even
    // with the bed far below its target and a wait reporting.
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xy");
    set_all_temps(/*bed*/ 250, 1000, /*ext*/ 0, 0);
    feed_gcode("B:25.0 /100.0");
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
}

TEST_CASE_METHOD(SnapmakerHeaterWaitFixture,
                 "Snapmaker U1: a wait showing when the print ends does not reach the next one",
                 "[print][collector][snapmaker][heater_wait]") {
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2100, 2100);
    feed_gcode("B:45.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    SECTION("a fresh start") {
        collector().stop();
        collector().start();
    }
    SECTION("a reset while active") {
        collector().reset();
    }
    drain_async_updates();
    collector().enable_fallbacks();

    // Proactive detection shows the bed heating before any signal, and the
    // reports stopping must not hand the last print's plate detect back.
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    clock_.advance(std::chrono::seconds(5));
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

namespace {
class ArtilleryHeaterWaitFixture : public PrintStartCollectorHeaterFixture {
  public:
    ArtilleryHeaterWaitFixture() {
        collector().set_profile(PrintStartProfile::load("artillery_m1"));
        collector().start();
        drain_async_updates();
        collector().enable_fallbacks();
    }

    void feed(const std::string& line) {
        send_gcode_response(line);
        drain_async_updates();
    }

    void tick() {
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
    }

    int progress() {
        return lv_subject_get_int(state().print_state().get_print_start_progress_subject());
    }

    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};
} // namespace

TEST_CASE_METHOD(ArtilleryHeaterWaitFixture,
                 "A heater wait gives a sequential profile's phase, message and progress back",
                 "[print][collector][heater_wait]") {
    feed("[AM1P] State: HOMING_PREPRINT");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    const std::string announced = get_current_message();
    REQUIRE(progress() == 62);

    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2100, 2100);
    feed("B:45.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    REQUIRE(progress() == 62);

    clock_.advance(std::chrono::seconds(5));
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message() == announced);
    REQUIRE(progress() == 62);
}

TEST_CASE_METHOD(ArtilleryHeaterWaitFixture,
                 "A phase signal right after a wait's last report keeps its phase",
                 "[print][collector][heater_wait]") {
    feed("[AM1P] State: NOZZLE_CLEAN");
    set_all_temps(/*bed*/ 450, 1000, /*ext*/ 2100, 2100);
    feed("B:45.0 /100.0 T0:210.0 /210.0");
    clock_.advance(std::chrono::milliseconds(500));
    feed("[AM1P] State: HOMING_PREPRINT");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // The first tick lands while the last report is still recent.
    clock_.advance(std::chrono::milliseconds(500));
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    for (int i = 0; i < 4; ++i) {
        clock_.advance(std::chrono::seconds(5));
        tick();
        REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    }
    REQUIRE(progress() == 62);
}

TEST_CASE_METHOD(ArtilleryHeaterWaitFixture,
                 "A chamber wait does not relabel a heating phase the firmware announced",
                 "[print][collector][heater_wait]") {
    feed("[AM1P] State: HEAT_BED");
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    set_all_temps(/*bed*/ 970, 1000, /*ext*/ 2100, 2100);
    lv_subject_set_int(state().temperature_state().get_chamber_temp_subject(), 300);
    lv_subject_set_int(state().temperature_state().get_chamber_target_subject(), 600);
    feed("B:97.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    // Even with the bed at target the firmware's phase stands.
    clock_.advance(std::chrono::seconds(1));
    set_all_temps(1000, 1000, 2100, 2100);
    feed("B:100.0 /100.0 T0:210.0 /210.0");
    tick();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

// ============================================================================
// Snapmaker U1 replay: the console from PRINT_SWITCH_CHECKING to a
// verify_heater shutdown, as Moonraker's gcode_store held it
// (tests/fixtures/u1_heater_wait_console.txt, "HH:MM:SS.mmm line"). Three
// blocking waits report once a second: M109 S170 during the switch check,
// M109 S210 after plate detect, then M109 S170 (a cool-down) straight into
// M190 S100 until the shutdown. The bed is far below 100C through all of them
// and the active tool is T2. Each wait shows the heater its command names, the
// cool-down included.
// ============================================================================

namespace {
std::string collector_fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

/// The U1 start G-code, file lines 80-230 at their real byte offsets
/// (tests/fixtures/u1_tool_probe_clean_start.gcode begins at byte 4342; the
/// bytes before it are blank lines here). The waits stand at: M109 T2 S170
/// 6204, M109 S210 6453, M109 S170 6732, M190 S100 6742, M109 S260 7281,
/// TEMPERATURE_WAIT 7342, M109 S260 T2 7738.
std::string u1_start_file() {
    std::ifstream f(collector_fixture_dir() + "u1_tool_probe_clean_start.gcode", std::ios::binary);
    REQUIRE(f.is_open());
    std::string block((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return std::string(4342, '\n') + block;
}

/// Replays a U1 console capture ("HH:MM:SS.mmm line") at its real timestamps,
/// with the fallback tick on the ETA timer's 5 s cadence. Heater state comes
/// from the wait reports themselves (bed and the active tool T2), seeded with
/// the given values until the first report.
///
/// serve() puts the printed file behind a ranged-read transfers API, and
/// position_at() scripts virtual_sdcard.file_position: from a time on, the
/// print stands at that byte.
class SnapmakerReplayFixture : public SnapmakerCollectorFixture {
  public:
    struct Shown {
        int ms; ///< since the first line
        PrintStartPhase phase;
        std::string message;
        int probes;
    };

    void serve(const std::string& filename, std::string bytes) {
        print_.emplace(client(), state(), filename);
        print_->transfers.put(filename, std::move(bytes));
    }
    int ranged_reads() const {
        return print_ ? print_->transfers.ranges : 0;
    }
    ServedPrint& served() {
        REQUIRE(print_);
        return *print_;
    }
    void position_at(int ms, uint64_t pos) {
        positions_.push_back({ms, pos});
    }
    /// Also deliver each position as a subscribed virtual_sdcard.file_position
    /// frame, from a track of "seconds byte" lines the way a 1 Hz poll saw them.
    void stream_positions(const std::string& track) {
        std::ifstream f(collector_fixture_dir() + track);
        REQUIRE(f.is_open());
        double sec = 0.0;
        uint64_t pos = 0;
        while (f >> sec >> pos) {
            position_at(static_cast<int>(std::lround(sec * 1000)), pos);
        }
        REQUIRE(positions_.size() > 10);
        stream_positions_ = true;
    }

    /// @p tool is the active extruder's id in the wait reports ("T2").
    std::vector<Shown> replay(const std::string& file, int bed, int bed_target, int ext,
                              int ext_target, const std::string& tool = "T2") {
        const std::string path = collector_fixture_dir() + file;
        std::ifstream f(path);
        INFO("fixture: " << path);
        REQUIRE(f.is_open());

        struct Line {
            int ms;
            std::string text;
        };
        std::vector<Line> lines;
        for (std::string raw; std::getline(f, raw);) {
            int h = 0, m = 0;
            double sec = 0.0;
            REQUIRE(std::sscanf(raw.c_str(), "%d:%d:%lf", &h, &m, &sec) == 3);
            lines.push_back({static_cast<int>((h * 3600 + m * 60 + sec) * 1000), raw.substr(13)});
        }
        REQUIRE(lines.size() > 200);

        helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
        set_all_temps(bed, bed_target, ext, ext_target);
        collector().start();
        drain_async_updates();
        collector().enable_fallbacks();

        const int t0 = lines.front().ms;
        int now = t0;
        int next_tick = t0 + 5000;
        std::vector<Shown> shown;
        const auto note = [&]() {
            drain_async_updates();
            drain_async_updates();
            Shown cur{now - t0, get_current_phase(), get_current_message(),
                      PrintStartCollectorTestAccess::get_mesh_probe_current(collector())};
            if (shown.empty() || cur.phase != shown.back().phase ||
                cur.message != shown.back().message) {
                shown.push_back(std::move(cur));
            }
        };
        const auto tick = [&]() {
            collector().check_fallback_completion();
            note();
        };

        const std::regex reading(R"((\w+):(-?\d+\.\d) /(-?\d+\.\d))");
        size_t next_position = 0;
        // Each position from its own time on, up to @p at.
        const auto stand_at = [&](int at) {
            while (next_position < positions_.size() &&
                   positions_[next_position].first <= at - t0) {
                const int moved = t0 + positions_[next_position].first;
                if (moved > now) {
                    clock.advance(std::chrono::milliseconds(moved - now));
                    now = moved;
                }
                client().set_object_status("virtual_sdcard",
                                           {{"file_position", positions_[next_position].second}});
                if (stream_positions_) {
                    client().dispatch_status_update(
                        {{"virtual_sdcard",
                          {{"file_position", positions_[next_position].second}}}});
                    note();
                }
                ++next_position;
            }
        };
        for (const Line& line : lines) {
            while (next_tick <= line.ms) {
                stand_at(next_tick);
                clock.advance(std::chrono::milliseconds(next_tick - now));
                now = next_tick;
                tick();
                next_tick += 5000;
            }
            stand_at(line.ms);
            clock.advance(std::chrono::milliseconds(line.ms - now));
            now = line.ms;

            bool target_changed = false;
            for (std::sregex_iterator it(line.text.begin(), line.text.end(), reading), end;
                 it != end; ++it) {
                const std::string id = (*it)[1];
                const int temp = static_cast<int>(std::lround(std::stod((*it)[2]) * 10));
                const int target = static_cast<int>(std::lround(std::stod((*it)[3]) * 10));
                if (id == "B") {
                    target_changed |= target != bed_target;
                    bed = temp;
                    bed_target = target;
                } else if (id == tool) {
                    target_changed |= target != ext_target;
                    ext = temp;
                    ext_target = target;
                }
            }
            set_all_temps(bed, bed_target, ext, ext_target);
            feed_gcode(line.text);
            note();
            if (target_changed) {
                tick(); // the target observers run the fallback check
            }
        }
        return shown;
    }

    /// "seconds:PHASE" for each phase change, the way a reader scans the bar.
    static std::string phase_trace(const std::vector<Shown>& shown) {
        std::string trace;
        PrintStartPhase last = PrintStartPhase::IDLE;
        for (const Shown& s : shown) {
            if (s.phase == last) {
                continue;
            }
            last = s.phase;
            if (!trace.empty()) {
                trace += ' ';
            }
            trace +=
                std::to_string(s.ms / 1000) + ":" + std::string(print_start_phase_name(s.phase));
        }
        return trace;
    }

    static std::string full_trace(const std::vector<Shown>& shown) {
        std::string trace;
        char buf[160];
        for (const Shown& s : shown) {
            const std::string_view name = print_start_phase_name(s.phase);
            std::snprintf(buf, sizeof(buf), "%.1f %.*s \"%s\" n=%d\n", s.ms / 1000.0,
                          static_cast<int>(name.size()), name.data(), s.message.c_str(), s.probes);
            trace += buf;
        }
        return trace;
    }

  private:
    std::optional<ServedPrint> print_;
    std::vector<std::pair<int, uint64_t>> positions_;
    bool stream_positions_ = false;
};
} // namespace

// ============================================================================
// Snapmaker U1 replay with the print position: a one-head print's console from
// file open to its first layer (tests/fixtures/u1_bmcu_console.txt, 2026-10-17),
// virtual_sdcard.file_position as a 1 Hz poll saw it (u1_bmcu_positions.txt,
// "seconds byte"), and the file's start block at its real offsets
// (u1_bmcu_start.gcode, from byte 12744). The head reports as T1. The
// firmware's print_task_config skipped BED_MESH_CALIBRATE ("does not do
// auto-leveling"), so the position jumps from G28 Z straight to M109 S260.
// ============================================================================

namespace {
std::string u1_bmcu_file() {
    std::ifstream f(collector_fixture_dir() + "u1_bmcu_start.gcode", std::ios::binary);
    REQUIRE(f.is_open());
    std::string block((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return std::string(12744, '\n') + block;
}
} // namespace

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: the print position shows the cleans, Z probes and prime line",
                 "[print][collector][snapmaker][start_block][integration]") {
    serve("u1_bmcu.gcode", u1_bmcu_file());
    stream_positions("u1_bmcu_positions.txt");
    const auto shown = replay("u1_bmcu_console.txt", /*bed*/ 230, 0, /*ext*/ 250, 0, "T1");
    CAPTURE(full_trace(shown));

    // The firmware's action codes name the bed inspect, the extruder check,
    // the feed and the plate detect; the heater waits name their heater. The
    // position adds the rest: G28 X Y, each clean, each G28 Z and the prime
    // line, each from the moment the print reaches its line. The skipped mesh
    // never shows.
    REQUIRE(phase_trace(shown) ==
            "0:INITIALIZING 0:HOMING 26:BED_MESH 87:INITIALIZING 94:HEATING_NOZZLE "
            "115:INITIALIZING 145:HEATING_NOZZLE 186:CLEANING 190:HOMING 210:BED_MESH "
            "224:HEATING_NOZZLE 234:CLEANING 249:HEATING_NOZZLE 270:HEATING_BED 319:HOMING "
            "340:HEATING_NOZZLE 359:PURGING");
    const auto message_at = [&](int s) {
        const auto it = std::find_if(shown.begin(), shown.end(),
                                     [&](const Shown& x) { return x.ms / 1000 == s; });
        REQUIRE(it != shown.end());
        return it->message;
    };
    CHECK(message_at(0) == "Preparing Print...");
    CHECK(shown[1].message == "Homing axes...");
    CHECK(message_at(190) == "Probing Z...");
    CHECK(message_at(319) == "Probing Z...");
    CHECK(message_at(186) == "Cleaning Nozzle...");
    CHECK(message_at(359) == "Purging...");
    for (const Shown& s : shown) {
        CHECK(s.message != "Bed mesh...");
    }
    // One head read; every other ranged read is a wait line's.
    const auto& reads = served().transfers.range_requests;
    CHECK(std::count(reads.begin(), reads.end(),
                     std::pair<uint64_t, size_t>{0, helix::kStartBlockHeadBytes}) == 1);

    // Each phase's history entry is the time it showed, over every visit:
    // HOMING three times, CLEANING and BED_MESH twice.
    collector().complete_from_external_signal("first layer");
    drain_async_updates();
    const int end_ms = shown.back().ms + 17400; // the last line, 377.3 s
    std::map<int, int> shown_ms;
    for (size_t i = 0; i < shown.size(); ++i) {
        const int until = i + 1 < shown.size() ? shown[i + 1].ms : end_ms;
        shown_ms[static_cast<int>(shown[i].phase)] += until - shown[i].ms;
    }
    const auto entries = collector().predictor().get_entries();
    REQUIRE_FALSE(entries.empty());
    const auto& durations = entries.back().phase_durations;
    CAPTURE(durations.size());
    for (const PrintStartPhase phase : {PrintStartPhase::HOMING, PrintStartPhase::BED_MESH,
                                        PrintStartPhase::CLEANING, PrintStartPhase::PURGING}) {
        const int p = static_cast<int>(phase);
        CAPTURE(print_start_phase_name(phase));
        REQUIRE(durations.count(p) == 1);
        CHECK(durations.at(p) == Catch::Approx(shown_ms[p] / 1000).margin(1));
    }
    CHECK(durations.count(static_cast<int>(PrintStartPhase::CLEANING)) == 1);
    CHECK(durations.at(static_cast<int>(PrintStartPhase::CLEANING)) < 25);
    // Nothing showed SOAKING, and INITIALIZING and the heating phases are not
    // recorded as phases.
    CHECK(durations.count(static_cast<int>(PrintStartPhase::SOAKING)) == 0);
    CHECK(durations.count(static_cast<int>(PrintStartPhase::INITIALIZING)) == 0);
    CHECK(durations.count(static_cast<int>(PrintStartPhase::HEATING_BED)) == 0);
}

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: a firmware code on a step's line is not overwritten by the step",
                 "[print][collector][snapmaker][start_block]") {
    // BED_MESH_CALIBRATE announces its 60 s soak first thing, and its status
    // frame, batched, arrives after that console line.
    std::string file = u1_bmcu_file();
    serve("u1_bmcu.gcode", file);
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    collector().start();
    drain_async_updates();
    const auto at = [&](uint64_t pos) {
        client().dispatch_status_update({{"virtual_sdcard", {{"file_position", pos}}}});
        drain_async_updates();
        drain_async_updates();
    };
    at(14316); // G28 Z: the first position reads the head
    at(14316);
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message() == "Probing Z...");

    clock.advance(std::chrono::seconds(20));
    feed_gcode("// Success: Set action code BED_PREHEATING");
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);
    clock.advance(std::chrono::milliseconds(250));
    at(14559);
    CHECK(get_current_phase() == PrintStartPhase::SOAKING);
    CHECK(get_current_message() == "Heat Soaking...");
    // The mesh command prints that code itself, so the step is not held for
    // later either.
    clock.advance(std::chrono::seconds(2));
    at(14559);
    CHECK(get_current_message() == "Heat Soaking...");

    // A step with no console line ahead of it shows.
    clock.advance(std::chrono::seconds(60));
    at(14840);
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: a start-block step from another run or after COMPLETE is dropped",
                 "[print][collector][snapmaker][start_block]") {
    serve("u1_bmcu.gcode", u1_bmcu_file());
    collector().start();
    drain_async_updates();
    const uint64_t run = PrintStartCollectorTestAccess::run_generation(collector());
    const helix::StartBlockStep clean{14002, helix::StartBlockStepKind::CleanNozzle, false, ""};

    PrintStartCollectorTestAccess::apply_start_block_step(collector(), clean, run - 1);
    drain_async_updates();
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
    // The same step from this run shows, so the drop above is the run's doing.
    PrintStartCollectorTestAccess::apply_start_block_step(
        collector(), {13756, helix::StartBlockStepKind::HomeAxes, true, ""}, run);
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    collector().complete_from_external_signal("first layer");
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    PrintStartCollectorTestAccess::apply_start_block_step(collector(), clean, run);
    drain_async_updates();
    CHECK(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: stopping the collector cancels the head read in flight",
                 "[print][collector][snapmaker][start_block]") {
    serve("u1_bmcu.gcode", u1_bmcu_file());
    served().transfers.hold_from = 1;
    collector().start();
    drain_async_updates();
    client().dispatch_status_update({{"virtual_sdcard", {{"file_position", 14002}}}});
    drain_async_updates();
    REQUIRE(served().transfers.ranges == 1);
    REQUIRE(served().transfers.last_cancel);
    CHECK_FALSE(served().transfers.last_cancel->load());

    collector().stop();
    CHECK(served().transfers.last_cancel->load());
    served().transfers.release_held();
    drain_async_updates();
}

namespace {
/// A started U1 collector serving the BMCU file, with position frames.
class SnapmakerStartBlockFixture : public SnapmakerReplayFixture {
  public:
    SnapmakerStartBlockFixture() {
        serve("u1_bmcu.gcode", u1_bmcu_file());
        collector().start();
        drain_async_updates();
    }
    void at(uint64_t pos) {
        client().dispatch_status_update({{"virtual_sdcard", {{"file_position", pos}}}});
        drain_async_updates();
        drain_async_updates();
    }
    helix::sim::SimulatedClock::ManualScope clock{helix::sim::SimSpeed::of(1.0)};
};
} // namespace

TEST_CASE_METHOD(SnapmakerStartBlockFixture,
                 "Snapmaker U1: a step after the previous command's last code shows once the "
                 "grace passes",
                 "[print][collector][snapmaker][start_block]") {
    at(13824); // G28 Z I140 J140: the first position reads the head
    at(13824);
    REQUIRE(get_current_message() == "Probing Z...");

    clock.advance(std::chrono::seconds(10));
    feed_gcode("// Success: Set action code DETECT_PLATE");
    REQUIRE(get_current_message() == "Detecting plate...");
    // The position passes ROUGHLY_CLEAN_NOZZLE: no command of the block prints
    // DETECT_PLATE for a clean, so the code was the plate detect's last word.
    clock.advance(std::chrono::milliseconds(300));
    at(14023);
    CHECK(get_current_message() == "Detecting plate...");

    SECTION("it shows once the grace passes") {
        clock.advance(std::chrono::seconds(1));
        at(14023);
        CHECK(get_current_phase() == PrintStartPhase::CLEANING);
        CHECK(get_current_message() == "Cleaning Nozzle...");
    }
    SECTION("a console change after it cancels it") {
        clock.advance(std::chrono::milliseconds(300));
        feed_gcode("// Success: Set action code PRINT_SWITCH_CHECKING");
        REQUIRE(get_current_message() == "Checking extruders...");
        clock.advance(std::chrono::seconds(2));
        at(14023);
        CHECK(get_current_message() == "Checking extruders...");
    }
}

TEST_CASE_METHOD(SnapmakerStartBlockFixture,
                 "Snapmaker U1: a held step shows at the ETA tick when no position follows",
                 "[print][collector][snapmaker][start_block]") {
    at(13824);
    at(13824);
    clock.advance(std::chrono::seconds(10));
    feed_gcode("// Success: Set action code DETECT_PLATE");
    clock.advance(std::chrono::milliseconds(300));
    at(14023); // past ROUGHLY_CLEAN_NOZZLE: held behind the plate detect's code
    REQUIRE(get_current_message() == "Detecting plate...");

    // The clean's moves keep the position on its line: no frame comes, and
    // only the collector's periodic timer is left to show the step.
    clock.advance(std::chrono::seconds(1));
    PrintStartCollectorTestAccess::fire_eta_timer(collector());
    drain_async_updates();
    drain_async_updates();
    CHECK(get_current_phase() == PrintStartPhase::CLEANING);
}

TEST_CASE_METHOD(SnapmakerStartBlockFixture,
                 "Snapmaker U1: a repeated action code that changes nothing does not hold a step",
                 "[print][collector][snapmaker][start_block]") {
    at(13824);
    at(13824);
    clock.advance(std::chrono::seconds(10));
    feed_gcode("// Success: Set action code DETECT_PLATE");
    clock.advance(std::chrono::seconds(5));
    feed_gcode("// Success: Set action code DETECT_PLATE");
    clock.advance(std::chrono::milliseconds(300));
    at(14023);
    CHECK(get_current_phase() == PrintStartPhase::CLEANING);
}

TEST_CASE_METHOD(SnapmakerStartBlockFixture,
                 "Snapmaker U1: a heater wait reads its line at the subscribed position",
                 "[print][collector][snapmaker][start_block][wait_line]") {
    collector().enable_fallbacks();
    at(14271); // M109 S170, a cool-down; no virtual_sdcard query is answered
    at(14271);
    set_all_temps(/*bed*/ 870, 1000, /*ext*/ 1900, 1700);
    feed_gcode("B:87.0 /100.0 T1:190.0 /170.0");
    for (int i = 0; i < 2; ++i) {
        collector().check_fallback_completion();
        drain_async_updates();
        drain_async_updates();
    }
    // The nozzle above its target reads as arrived, so a guess says the bed.
    CHECK(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
}

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: a head read answered after a new start shows nothing",
                 "[print][collector][snapmaker][start_block]") {
    serve("u1_bmcu.gcode", u1_bmcu_file());
    served().transfers.hold_from = 1;
    collector().start();
    drain_async_updates();
    client().dispatch_status_update({{"virtual_sdcard", {{"file_position", 14023}}}});
    drain_async_updates();
    REQUIRE(served().transfers.ranges == 1);

    // The next print's file has no steps; its frames name it.
    collector().stop();
    served().transfers.put("other.gcode", std::string(14100, '\n') + "M104 S0\n");
    collector().start();
    drain_async_updates();
    client().dispatch_status_update({{"print_stats", {{"filename", "other.gcode"}}},
                                     {"virtual_sdcard", {{"file_position", 14023}}}});
    drain_async_updates();
    REQUIRE(served().transfers.ranges == 2);
    served().transfers.release_held();
    drain_async_updates();
    drain_async_updates();
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
}

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "A profile without a scripted start does not read the print's head",
                 "[print][collector][start_block]") {
    collector().set_profile(PrintStartProfile::load_default());
    ServedPrint print(client(), state(), "plain.gcode");
    print.transfers.put("plain.gcode", "G28\nCLEAN_NOZZLE\nG1 X10 E5\n");
    collector().start();
    drain_async_updates();
    client().dispatch_status_update({{"virtual_sdcard", {{"file_position", 4}}}});
    drain_async_updates();
    client().dispatch_status_update({{"virtual_sdcard", {{"file_position", 4}}}});
    drain_async_updates();
    CHECK(print.transfers.ranges == 0);
    CHECK(get_current_phase() != PrintStartPhase::CLEANING);
}

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: real heater waits show the bed heating between firmware phases",
                 "[print][collector][snapmaker][heater_wait][integration]") {
    // That print's file is not in the repo; it ran the same start block, so
    // its waits stand where the 2026-10-16 print's do, each from its first
    // report. The cool-down hands to M190 at approximately the first report
    // with T2 within 1C of 170 (Klipper's settle band), with no gap in the
    // reports.
    serve("u1_print.gcode", u1_start_file());
    position_at(3065, 6204);
    position_at(42926, 6453);
    position_at(68869, 6732);
    position_at(83877, 6742);
    const auto shown = replay("u1_heater_wait_console.txt", /*bed*/ 460, 1000, /*ext*/ 1488, 1700);

    // The cool-down shows the nozzle from its first report. The M190 that
    // follows is read at the first tick after the position moves (85 s) and
    // shown at the next.
    const std::string trace = phase_trace(shown);
    CAPTURE(full_trace(shown));
    REQUIRE(trace == "0:INITIALIZING 5:HEATING_NOZZLE 13:HOMING 28:BED_MESH 42:HEATING_NOZZLE "
                     "60:BED_MESH 68:HEATING_NOZZLE 90:HEATING_BED");
    REQUIRE(get_current_message() == "Heating Bed...");
}

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: each real wait shows the heater its command waits on",
                 "[print][collector][snapmaker][heater_wait][wait_line][integration]") {
    // The capture has no file_position track, so these times are inferred
    // from the reports: each wait from its first report, and M190 from the
    // first report with T2 within 1C of 170. Klipper's M109 also waits for
    // the temperature to stop moving, so the real hand-off is likely a few
    // seconds later; nothing below pins the hand-off tick.
    serve("u1_tool_probe_clean.gcode", u1_start_file());
    position_at(45400, 6204);
    position_at(84638, 6453);
    position_at(112782, 6732);
    position_at(129793, 6742);
    position_at(529619, 7281);
    position_at(568504, 7738);
    const auto shown =
        replay("u1_tool_probe_clean_console.txt", /*bed*/ 352, 1000, /*ext*/ 1567, 1700);
    CAPTURE(full_trace(shown));

    // What the display showed at `ms`.
    const auto at = [&](int ms) {
        auto it =
            std::find_if(shown.rbegin(), shown.rend(), [&](const Shown& s) { return s.ms <= ms; });
        REQUIRE(it != shown.rend());
        return it->phase;
    };
    const auto never_between = [&](int from, int to, PrintStartPhase phase) {
        CAPTURE(from, to, print_start_phase_name(phase));
        REQUIRE(at(from) != phase);
        for (const Shown& s : shown) {
            if (s.ms > from && s.ms <= to) {
                CAPTURE(s.ms);
                REQUIRE(s.phase != phase);
            }
        }
    };

    // Three nozzle waits: M109 T2 S170 ends in the band where the nozzle is
    // no longer "short", M109 S210 likewise, and M109 S170 cools from 196.
    // Each is the nozzle at every tick, and never the bed.
    for (const auto& [from, to] : {std::pair{45400, 51403}, {84638, 96646}, {112782, 129793}}) {
        CAPTURE(from);
        REQUIRE(at(to) == PrintStartPhase::HEATING_NOZZLE);
        never_between(from, to, PrintStartPhase::HEATING_BED);
    }
    REQUIRE(at(50000) == PrintStartPhase::HEATING_NOZZLE);

    // M190 S100 follows with no gap in the reports: the bed from a few ticks
    // after the hand-off until G28 Z at 324.9 s, its settling tail included.
    for (int ms = 145000; ms < 324904; ms += 5000) {
        CAPTURE(ms);
        REQUIRE(at(ms) == PrintStartPhase::HEATING_BED);
    }
    never_between(319000, 325000, PrintStartPhase::SOAKING);

    REQUIRE(shown.back().phase == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(ranged_reads() == 6);
}

// ============================================================================
// Snapmaker U1 replay: a 3-tool print's console from file open to the nozzle
// heating for the prime line (tests/fixtures/u1_tool_probe_clean_console.txt,
// 2026-10-16, t=0 at 21:48:13.136). Each G28 Z, after a tool pick or before the
// mesh, prints "// z offset:" and touches one point; the mesh is announced by
// BED_PREHEATING (a 60 s soak), BED_PRESCANNING (four corners) and
// BED_LEVELING (36 points). Which heater a wait shows is not asserted here.
// ============================================================================

TEST_CASE_METHOD(SnapmakerReplayFixture,
                 "Snapmaker U1: real tool Z touches and the announced mesh replay in order",
                 "[print][collector][snapmaker][integration]") {
    const auto shown =
        replay("u1_tool_probe_clean_console.txt", /*bed*/ 352, 1000, /*ext*/ 1567, 1700);
    CAPTURE(full_trace(shown));

    // The display change that starts within 1 ms of `ms`.
    const auto at = [&](int ms) -> const Shown& {
        const auto it = std::find_if(shown.begin(), shown.end(),
                                     [&](const Shown& s) { return std::abs(s.ms - ms) <= 1; });
        INFO("no display change at " << ms << " ms");
        REQUIRE(it != shown.end());
        return *it;
    };
    const auto shows = [&](int ms, PrintStartPhase phase, const char* message) {
        const Shown& s = at(ms);
        CAPTURE(ms, s.message);
        REQUIRE(s.phase == phase);
        REQUIRE(s.message == message);
    };

    shows(26516, PrintStartPhase::BED_MESH, "Inspecting bed...");
    shows(36155, PrintStartPhase::INITIALIZING, "Checking extruders...");

    // The tool picked by the switch check touches the bed centre.
    shows(55829, PrintStartPhase::HOMING, "Probing Z...");
    for (const Shown& s : shown) {
        if (s.ms > 55829 && s.ms < 69787) {
            CAPTURE(s.ms, s.message);
            REQUIRE(s.phase != PrintStartPhase::BED_MESH);
        }
    }
    shows(69787, PrintStartPhase::BED_MESH, "Detecting plate...");

    // G28 Z before the mesh, then the announced mesh steps.
    shows(324904, PrintStartPhase::HOMING, "Probing Z...");
    shows(339703, PrintStartPhase::SOAKING, "Heat Soaking...");
    shows(399740, PrintStartPhase::BED_MESH, "Validating bed mesh...");

    // BED_LEVELING restarts the count, and its G28 Z 1 ms later moves nothing.
    shows(433454, PrintStartPhase::BED_MESH, "Bed mesh...");
    const auto leveling = std::find_if(shown.begin(), shown.end(),
                                       [](const Shown& s) { return s.message == "Bed mesh..."; });
    REQUIRE(std::prev(leveling)->message == "Validating bed mesh (4)");
    REQUIRE(leveling->probes == 0);
    REQUIRE(std::next(leveling)->message == "Bed mesh (1)");
    for (auto it = leveling; it != shown.end(); ++it) {
        REQUIRE(it->phase != PrintStartPhase::HOMING);
    }

    // 36 mesh points, none of the corners.
    const auto last_mesh = std::find_if(shown.rbegin(), shown.rend(), [](const Shown& s) {
        return s.phase == PrintStartPhase::BED_MESH;
    });
    REQUIRE(last_mesh->message == "Bed mesh (36)");

    // The nozzle heats for the prime line once the mesh ends.
    REQUIRE(shown.back().phase == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(shown.back().ms >= 529565);
    REQUIRE(shown.back().ms <= 530000);
}

TEST_CASE("heater wait report lines", "[print][collector][heater_wait]") {
    REQUIRE(PrintStartCollectorTestAccess::is_heater_wait_report("B:45.2 /100.0 T0:210.1 /210.0"));
    REQUIRE(PrintStartCollectorTestAccess::is_heater_wait_report("T0:150.0 /210.0"));
    REQUIRE(PrintStartCollectorTestAccess::is_heater_wait_report("B:25.0 /100.0 C:30.0 /60.0"));
    REQUIRE_FALSE(PrintStartCollectorTestAccess::is_heater_wait_report("ok B:45.2 /100.0"));
    REQUIRE_FALSE(PrintStartCollectorTestAccess::is_heater_wait_report("T:0"));
    REQUIRE_FALSE(PrintStartCollectorTestAccess::is_heater_wait_report("// z offset: -0.05"));
    REQUIRE_FALSE(
        PrintStartCollectorTestAccess::is_heater_wait_report("// Success: Set action code X"));
}

// ============================================================================
// A response_pattern whose phase enum was already detected may still refine
// the MESSAGE when it differs (the U1 narrates "Probing Z..." and "Bed
// mesh..." through enums a proactive or earlier signal already claimed), but
// it may never move the phase backwards.
// ============================================================================

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: already-detected phase refines the message, never moves back",
                 "[print][collector][snapmaker][preprint]") {
    collector().start();
    drain_async_updates();

    feed_gcode("// trigger_mcu_pos: {");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message().find("Homing axes") != std::string::npos);

    // Same HOMING enum, different message: the label refines in place.
    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message().find("Probing Z") != std::string::npos);

    feed_gcode("// Success: Set action code PRINT_BED_DETECTING");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message().find("Inspecting bed") != std::string::npos);

    // A late repeat of an earlier enum must not drag the phase back.
    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message().find("Inspecting bed") != std::string::npos);
}

// ============================================================================
// Full U1 pre-print replay at the device's real pace (one print, klippy.log
// timestamps): homing, Z touch, bed inspect, extruder switch check, auto-feed,
// the new tool's Z touch, plate detect + 4 probes — then ~129s of true silence
// while klippy-internal flow calibration and nozzle clean run (no
// gcode_response exists for them) — then the real bed mesh (~14s), then ~39s
// of quiet prime line before layer 1. Nothing narrates the silent prime line,
// and the collector has no priming inference to guess at it: the last
// declared phase owns the display through each silent stretch, and only the
// genuine current_layer 0->1 edge completes the pre-print.
// ============================================================================

TEST_CASE_METHOD(SnapmakerCollectorFixture,
                 "Snapmaker U1: full pre-print replay at real pace - declared phases own the "
                 "display, no inferred Priming",
                 "[print][collector][snapmaker][preprint]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    collector().start();
    drain_async_updates();

    // One print_stats tick: 1s of sim time.
    auto tick = [&]() {
        clock.advance(std::chrono::seconds(1));
        drain_async_updates();
        return get_current_message();
    };
    auto quiet_ticks = [&](int seconds, const char* hold) {
        for (int i = 0; i < seconds; ++i) {
            const std::string msg = tick();
            INFO("quiet tick " << i << ": " << msg);
            REQUIRE(msg.find(hold) != std::string::npos);
            REQUIRE(msg.find("Priming") == std::string::npos);
        }
    };

    feed_gcode("// trigger_mcu_pos: {\"z\": 150}");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message().find("Homing axes") != std::string::npos);

    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_message().find("Probing Z") != std::string::npos);

    feed_gcode("// Success: Set action code PRINT_BED_DETECTING");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message().find("Inspecting bed") != std::string::npos);

    // print_duration goes positive here on the real printer; the sequence
    // continues ticking through it.
    feed_gcode("// Success: Set action code PRINT_SWITCH_CHECKING");
    REQUIRE(get_current_message().find("Checking extruders") != std::string::npos);
    REQUIRE(tick().find("Checking extruders") != std::string::npos);

    feed_gcode("// Success: Set action code PRINT_AUTO_FEEDING");
    REQUIRE(get_current_message().find("Loading filament") != std::string::npos);
    REQUIRE(tick().find("Loading filament") != std::string::npos);

    // The picked tool touches the bed centre (G28 Z I140 J140): one point,
    // sampled repeatedly, is not a mesh.
    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message().find("Probing Z") != std::string::npos);
    feed_gcode("// probe at x: 140.000, y: 140.000 is z=302.521667");
    feed_gcode("// probe at x: 140.000, y: 140.000 is z=302.522500");
    feed_gcode("// probe at x: 140.000, y: 140.000 is z=302.510833");
    REQUIRE(tick().find("Probing Z") != std::string::npos);
    feed_gcode("// z_mesh_complete: 0.2980673660155705");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    // Plate detect and its four probes: the last narrated pre-mesh step.
    feed_gcode("// Success: Set action code DETECT_PLATE");
    REQUIRE(get_current_message().find("Detecting plate") != std::string::npos);
    feed_gcode("// probe at 10.000,10.000 is z=-0.200000");
    feed_gcode("// probe at 35.000,10.000 is z=-0.205000");
    feed_gcode("// probe at 60.000,10.000 is z=-0.210000");
    feed_gcode("// probe at 85.000,10.000 is z=-0.215000");
    REQUIRE(tick().find("Detecting plate") != std::string::npos);

    // ~129s of true silence: flow calibration and nozzle clean are
    // klippy-internal and narrate nothing. The display must keep the plate
    // label the whole way — nothing speaks during a silent stretch.
    quiet_ticks(129, "Detecting plate");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // The real bed mesh, announced by its action code; G28 Z follows 1 ms later.
    feed_gcode("// Success: Set action code BED_LEVELING");
    feed_gcode("// z offset: -0.05");
    REQUIRE(get_current_message().find("Bed mesh") != std::string::npos);
    feed_gcode("// probe at x: 33.742, y: 28.487 is z=-0.085000");
    feed_gcode("// probe at x: 74.442, y: 28.487 is z=-0.063333");
    REQUIRE(tick().find("Bed mesh") != std::string::npos);
    feed_gcode("// Mesh Bed Leveling Complete");

    // ~39s of quiet prime line before layer 1: the prime is not narrated, so
    // the mesh label holds until the layer edge completes the pre-print.
    quiet_ticks(39, "Bed mesh");

    collector().complete_from_external_signal("first layer");
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    REQUIRE(get_current_message().find("Starting Print") != std::string::npos);
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

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Adapted probe count outranks the configured grid when it precedes the mesh",
                 "[print][collector][bed_mesh][adapted]") {
    // Klipper prints the adapted grid inside BED_MESH_CALIBRATE, which can be
    // before the collector enters BED_MESH; entry then asks Moonraker for the
    // configured 6x6 grid.
    client().set_config_bed_mesh_probe_count(6, 6);
    client().gcode_script("BED_MESH_CLEAR");
    collector().start();
    drain_async_updates();

    SECTION("Without an adapted line the configured grid is the total") {
        enter_mesh_via_markers(*this);
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
        CHECK(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 36);
    }

    SECTION("The adapted grid survives mesh entry and the configfile answer") {
        send_gcode_response("G28");
        send_gcode_response("// Adapted probe count: (5,5)");
        send_gcode_response("BED_MESH_CALIBRATE");
        drain_async_updates();
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
        CHECK(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 25);

        send_gcode_response("// probe at 10.000,10.000 is z=0.100000");
        drain_async_updates();
        CHECK(get_current_message().find("(1/25)") != std::string::npos);
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

    // 12:18:50 — G28, announced only by a debug line.
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

    // 12:34:39 — CFS purge, announced by its flush temperature; no
    // BOX_MATERIAL_FLUSH line appears.
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
        // The mock starts with a probed mesh and answers the query with it.
        client().gcode_script("BED_MESH_CLEAR");
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
                 "K2 purge-percent and box tags are ignored without the K2 tag patterns",
                 "[print][collector][k2][negative]") {
    // The creality_k1 profile carries no tag_patterns: on a K1/K1C the
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
 * On any printer that meshes after heating, the heaters sit at target minutes
 * before the pre-print is over. A timeout there ends Preparing mid-sequence
 * and saves no phase timings, so the prediction that set the deadline cannot
 * grow: a K2 Plus predicted at 185s runs ~1140s.
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
    set_all_temps(1050, 1050, 2650, 2650); // both heaters at target
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
    set_all_temps(1050, 1050, 2650, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);
    // Nothing heard for well over the quiet window — this one really is stuck.
    PrintStartCollectorTestAccess::set_last_activity_seconds_ago(collector(), 300);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A long pre-print that is still talking outlives a short prediction",
                 "[print][collector][timeout][k2]") {
    // The K2 Plus pre-print runs ~1140s: heat, then a ~390s mesh, then purge,
    // well past 2.5x a 185s prediction.
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();

    PrintStartCollectorTestAccess::set_predicted_total(collector(), 185.0f);
    set_all_temps(1050, 1050, 2650, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1100);
    PrintStartCollectorTestAccess::set_last_activity_seconds_ago(collector(), 3);

    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A heater sampled during the last print is not climbing on the next",
                 "[print][collector][timeout]") {
    // The last print's final fallback sample caught the bed mid-climb.
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    set_all_temps(500, 1050, 2650, 2650);
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    collector().stop();
    drain_async_updates();

    // The next print starts with the bed hot, just short of its target, and quiet.
    collector().start();
    drain_async_updates();
    reset_collector_to_idle();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    set_all_temps(1040, 1050, 2650, 2650);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    tick_fallbacks();

    // Nothing climbed since this print began, so the quiet timeout ends it.
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
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
    set_all_temps(1050, 1050, 2650, 2650);
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

// ============================================================================
// PHASE-OBJECT STATUS SOURCE TESTS
//
// A profile may declare a status object whose field carries structured phase
// state. Those frames arrive on notify_status_update (not gcode_response) and
// feed the same match/apply pipeline a console line takes, so weights and
// progress behave identically. Console matching is untouched - both feeds
// coexist and weights arbitrate.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Phase-object status frames drive phases like console lines",
                 "[print][collector][phase_object]") {
    collector().start();
    drain_async_updates();

    // A state the profile has no mapping for changes nothing.
    send_status_update({{"operation_context", {{"current_state", "STANDBY"}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // The mod publishes its state machine into the object; the profile maps
    // the string through the same weights a console line takes.
    send_status_update({{"operation_context", {{"current_state", "HOMING"}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    REQUIRE(get_current_message() == "Homing...");
    REQUIRE(get_current_progress() == 5);

    // Next state advances through the mapping - sequential progress follows
    // the state pattern's weight, exactly as a console match would.
    send_status_update({{"operation_context", {{"current_state", "LEVELING"}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Bed Mesh...");
    REQUIRE(get_current_progress() == 25);
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Unchanged phase-object state does not re-fire",
                 "[print][collector][phase_object]") {
    collector().start();
    drain_async_updates();

    send_status_update({{"operation_context", {{"current_state", "MESHING"}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Bed Mesh...");

    // A console signal relabels the same phase - a real sub-phase change that
    // the status feed must not clobber when the object re-delivers its
    // unchanged state (Klipper notifies on every field change in the object).
    send_gcode_response("// State: KAMP LEVELING...");
    REQUIRE(get_current_message() == "Creating bed mesh...");

    send_status_update({{"operation_context", {{"current_state", "MESHING"}}}});
    REQUIRE(get_current_message() == "Creating bed mesh...");

    // A NEW state still applies after the suppressed repeat.
    send_status_update({{"operation_context", {{"current_state", "PRINTING"}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    REQUIRE(get_current_progress() == 100);
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Phase-object match gates the proactive temperature detector",
                 "[print][collector][phase_object]") {
    // Bed heating below target: with no real firmware signal seen, the
    // proactive detector would relabel the phase HEATING_BED on its next
    // check. A phase-object state IS the firmware narrating, so it must latch
    // the same real_signal_seen_ gate a console match latches.
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 250);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    collector().start();
    collector().enable_fallbacks();
    drain_async_updates();

    send_status_update({{"operation_context", {{"current_state", "HOMING"}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);

    collector().check_fallback_completion();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Profiles without a phase object ignore phase-object frames",
                 "[print][collector][phase_object]") {
    collector().set_profile(PrintStartProfile::load("creality_k2"));
    collector().start();
    drain_async_updates();

    // The K2 profile declares no status phase source: the identical frame
    // must be ignored entirely - phase stays at the start() baseline. This is
    // the fallback guarantee for every profile without the field.
    client().dispatch_status_update({{"operation_context", {{"current_state", "HOMING"}}}});
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE(get_current_message() == "Preparing Print...");
}

// ============================================================================
// CONTEXT-QUALIFIED PHASE OBJECT
//
// Forge-X publishes operation_context: a stack of nested steps (context_types,
// outermost first) and current_state, the innermost step's state. One state
// string means different things in different steps, so the profile matches
// "<innermost type>/<state>". Moonraker sends only the fields that changed.
// ============================================================================

class ForgeXCollectorFixture : public PrintStartCollectorHeaterFixture {
  public:
    ForgeXCollectorFixture() {
        auto profile = PrintStartProfile::load("forge_x");
        REQUIRE(profile->name() == "Forge-X Mod");
        collector().set_profile(std::move(profile));
    }

    /// One operation_context delta frame through notify_status_update.
    void context(const json& fields) {
        client().dispatch_status_update({{"operation_context", fields}});
        drain_async_updates();
    }
};

TEST_CASE_METHOD(ForgeXCollectorFixture, "A context-qualified state names the step it belongs to",
                 "[print][collector][phase_object][forge_x]") {
    collector().start();
    drain_async_updates();

    // The print context's LEVELING only decides how to level.
    context({{"context_types", {"print"}}, {"current_state", "LEVELING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // KAMP's own LEVELING is the mesh.
    context({{"context_types", {"print", "kamp"}}, {"current_state", "LEVELING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Creating bed mesh...");
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "Phase-object delta frames re-match the cached state and context",
                 "[print][collector][phase_object][forge_x]") {
    collector().start();
    drain_async_updates();

    context({{"context_types", {"print", "kamp", "nozzle_clean"}}, {"current_state", nullptr}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // A frame carrying only the state pairs it with the cached context.
    context({{"current_state", "COOLING NOZZLE"}});
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    REQUIRE(get_current_message() == "Cooling for probing...");

    // FINISHING ends the nested clean, not the pre-print.
    context({{"current_state", "FINISHING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    REQUIRE(get_current_message() == "Finishing clean...");

    // A frame carrying only the context pairs it with the cached state:
    // kamp/FINISHING names nothing, so the display holds.
    context({{"context_types", {"print", "kamp"}}});
    REQUIRE(get_current_message() == "Finishing clean...");

    context({{"current_state", "LEVELING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // A frame carrying neither field changes nothing.
    context({{"revision", 41}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    context({{"context_types", {"print"}}, {"current_state", "PRINTING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "A null state when a nested step begins is not the outer step's state",
                 "[print][collector][phase_object][forge_x]") {
    collector().start();
    drain_async_updates();

    context({{"context_types", {"print"}}, {"current_state", "LEVELING"}});
    // KAMP opens with no state of its own. Pairing kamp with the print's
    // LEVELING would claim the mesh ten minutes before it starts.
    context({{"context_types", {"print", "kamp"}}, {"current_state", nullptr}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    context({{"context_types", {"print", "kamp", "nozzle_clean"}}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    context({{"current_state", "HEATING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "A live state may move the display back to an earlier phase",
                 "[print][collector][phase_object][forge_x]") {
    collector().start();
    drain_async_updates();

    context(
        {{"context_types", {"print", "kamp", "nozzle_clean"}}, {"current_state", "HEATING BED"}});
    context({{"current_state", "HEATING NOZZLE"}});
    context({{"current_state", "CLEANING"}});
    context({{"context_types", {"print", "kamp"}}, {"current_state", "LEVELING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // After the mesh the print parks and reheats the nozzle the clean cooled.
    context({{"context_types", {"print"}}, {"current_state", "PARKING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(get_current_message() == "Preparing...");
    context({{"current_state", "HEATING NOZZLE"}});
    REQUIRE(get_current_message() == "Heating Nozzle...");

    // A console line is an event, not a state: it still cannot drag the
    // display back to a phase already passed.
    send_gcode_response("// Wait bed temperature to reach 105 Cº");
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
}

TEST_CASE_METHOD(ForgeXCollectorFixture, "The phase-object cache does not outlive the run",
                 "[print][collector][phase_object][forge_x]") {
    collector().start();
    drain_async_updates();
    context({{"context_types", {"print", "kamp"}}, {"current_state", "HEATING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    collector().stop();

    collector().start();
    drain_async_updates();
    // The last run's kamp context must not qualify this run's first state.
    context({{"current_state", "LEVELING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "A context opened before the collector armed qualifies its states",
                 "[print][collector][phase_object][forge_x]") {
    // The print context begins in the same breath as print_stats goes to
    // printing, so its frame can pass before start(), and with no nested
    // step it never changes again.
    client().set_object_status("operation_context",
                               {{"context_types", {"print"}}, {"current_state", "CHECKING FILE"}});
    collector().start();
    drain_async_updates();
    REQUIRE(get_current_message() == "Verifying file...");

    context({{"current_state", "PRIMING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "The start-up query fills only fields no frame has delivered",
                 "[print][collector][phase_object][forge_x]") {
    collector().start();
    drain_async_updates();

    context({{"context_types", {"print", "kamp"}}});
    // A late answer to the start-up query still says the print context; the
    // frame is newer and keeps kamp.
    PrintStartCollectorTestAccess::feed_phase_object_seed(
        collector(),
        {{"operation_context", {{"context_types", {"print"}}, {"current_state", "LEVELING"}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "The last print's leftover PRINTING does not complete the next one",
                 "[print][collector][phase_object][forge_x]") {
    // A file without an end macro leaves its print context standing at
    // PRINTING; the collector arms a moment before the next START_PRINT
    // pushes its own context, so the start-up query answers with the old one.
    client().set_object_status("operation_context",
                               {{"context_types", {"print"}}, {"current_state", "PRINTING"}});
    collector().start();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE_FALSE(collector().in_live_preprint_step());

    context({{"context_types", {"print", "print"}}, {"current_state", nullptr}});
    context({{"current_state", "CHECKING FILE"}});
    REQUIRE(get_current_message() == "Verifying file...");

    // The new print's own PRINTING still completes it.
    context({{"current_state", "PRINTING"}});
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(ForgeXCollectorFixture,
                 "A late start-up answer from an earlier run does not seed the next",
                 "[print][collector][phase_object][forge_x]") {
    const json answer = {
        {"result",
         {{"status",
           {{"operation_context",
             {{"context_types", {"print", "kamp"}}, {"current_state", "LEVELING"}}}}}}}};
    collector().start();
    drain_async_updates();
    const uint64_t first_run = PrintStartCollectorTestAccess::run_generation(collector());
    collector().stop();
    collector().start();
    drain_async_updates();

    PrintStartCollectorTestAccess::answer_phase_object_query(collector(), answer, first_run);
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    PrintStartCollectorTestAccess::answer_phase_object_query(
        collector(), answer, PrintStartCollectorTestAccess::run_generation(collector()));
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(ForgeXCollectorFixture, "Bed mesh validation names no point count",
                 "[print][collector][phase_object][forge_x][mesh]") {
    // _CHECK_BED_MESH probes five points against the loaded 25-point mesh,
    // so a count would read "(5/25)" and end there.
    client().set_config_bed_mesh_probe_count(5, 5);
    collector().start();
    drain_async_updates();

    context({{"context_types", {"print", "mesh_validation"}}, {"current_state", "CHECKING MESH"}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    const double points[][2] = {{-110, 60}, {-110, -110}, {60, -110}, {110, -110}, {110, 60}};
    for (const auto& p : points) {
        for (int sample = 0; sample < 3; ++sample) {
            char line[96];
            std::snprintf(line, sizeof(line), "// probe at %.3f,%.3f is z=-2.100000", p[0], p[1]);
            send_gcode_response(line);
        }
    }
    drain_async_updates();
    CHECK(get_current_message() == "Validating bed mesh...");
    CHECK(PrintStartCollectorTestAccess::get_mesh_probe_total(collector()) == 0);
    CHECK(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 0);
}

TEST_CASE_METHOD(ForgeXCollectorFixture, "A KAMP mesh counts its points against the total",
                 "[print][collector][phase_object][forge_x][mesh]") {
    client().set_config_bed_mesh_probe_count(5, 5);
    client().gcode_script("BED_MESH_CLEAR");
    collector().start();
    drain_async_updates();
    context({{"context_types", {"print", "kamp"}}, {"current_state", "LEVELING"}});
    send_gcode_response("// probe at -105.000,-105.000 is z=-2.025000");
    drain_async_updates();
    CHECK(get_current_message() == "Creating bed mesh (1/25)");
}

TEST_CASE_METHOD(ForgeXCollectorFixture, "A live pre-print step holds the deadline timeouts",
                 "[print][collector][phase_object][forge_x][timeout]") {
    // Heaters at target, the printer silent past the quiet gate and the
    // deadline: everything a timeout waits for, except that the printer says
    // it is still cooling the nozzle inside the clean.
    set_all_temps(1050, 1050, 1200, 1200);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    context({{"context_types", {"print", "kamp", "nozzle_clean"}},
             {"current_state", "COOLING NOZZLE"}});
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);

    SECTION("the adaptive and flat timeouts wait") {
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 600);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    }

    SECTION("the ceiling still ends it") {
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1850);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }

    SECTION("a state that names no step does not release the hold") {
        context({{"context_types", {"print", "kamp"}}, {"current_state", nullptr}});
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 600);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    }

    SECTION("an empty context stack releases the hold") {
        // Every context ended (an error or a restart reset the stack): no
        // step is under way any more.
        context({{"context_types", json::array()}, {"current_state", nullptr}});
        REQUIRE_FALSE(collector().in_live_preprint_step());
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 600);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }

    SECTION("the same step pushed again after the stack emptied holds again") {
        context({{"context_types", json::array()}, {"current_state", nullptr}});
        REQUIRE_FALSE(collector().in_live_preprint_step());
        context({{"context_types", {"print", "kamp", "nozzle_clean"}},
                 {"current_state", "COOLING NOZZLE"}});
        REQUIRE(collector().in_live_preprint_step());
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 600);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A bare phase-object state does not hold the timeouts",
                 "[print][collector][phase_object][timeout]") {
    // A state with no context is a label that may stay up long after its
    // step ended (display_status keeps the last SET_DISPLAY_TEXT), so it
    // leaves the timeouts as they were.
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"bare","phase_object":{"object":"operation_context","field":"current_state"},)"
        R"("state_patterns":[{"pattern":"COOLING","phase":"CLEANING","message":"Cooling for probing..."}]})"));
    REQUIRE(profile != nullptr);
    collector().set_profile(std::move(profile));
    set_all_temps(1050, 1050, 1200, 1200);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    client().dispatch_status_update({{"operation_context", {{"current_state", "COOLING"}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 600);
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A heater cooling toward its target is not at target",
                 "[print][collector][timeout]") {
    // M109 down to a probing temperature blocks the queue for minutes with
    // nothing on the console; the nozzle reads above its target the whole time.
    const auto tick_at = [this](int ext_decideg, int elapsed_s) {
        set_all_temps(1050, 1050, ext_decideg, 1200);
        collector().start();
        drain_async_updates();
        reset_collector_to_idle();
        collector().enable_fallbacks();
        PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
        PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), elapsed_s);
        tick_fallbacks();
        tick_fallbacks();
    };

    SECTION("a nozzle at 141 cooling to 120 waits") {
        tick_at(1410, 600);
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
    }

    SECTION("a nozzle within 5C above its target has arrived") {
        tick_at(1240, 600);
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }

    SECTION("a nozzle that never cools completes at the ceiling") {
        tick_at(1410, 1850);
        REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A signal-format line counts as activity for the timeouts",
                 "[print][collector][timeout][sequential]") {
    auto profile =
        PrintStartProfileTestAccess::parse(nlohmann::json::parse(kSequentialStateProfile));
    REQUIRE(profile != nullptr);
    collector().set_profile(std::move(profile));
    set_all_temps(1050, 1050, 2200, 2200);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 180.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 600);

    send_gcode_response("// State: CLEANING...");
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
}

// ============================================================================
// STATUS-SIGNAL RULE TESTS
//
// Profiles may declare physical phase-inference rules ("status_signals"):
// predicates over status objects, applied on the false->true rising edge only.
// Matches feed apply_profile_match(), so weights arbitrate against console
// narration exactly as they do for every other source.
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "at_cutter fires PURGING on the rising edge of both-negative position",
                 "[print][collector][status_signals]") {
    collector().start();
    drain_async_updates();

    // Mid-bed: neither rule holds, the latch stays disengaged.
    send_status_update({{"toolhead", {{"position", {114.0, 110.0, 10.0}}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // Both axes negative: the head is at the cutter (IFS tool change). The
    // rule's weight is the sequential progress, like any profile match.
    send_status_update({{"toolhead", {{"position", {-2.0, -3.0, 10.0}}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    REQUIRE(get_current_message() == "Changing filament...");
    REQUIRE(get_current_progress() == 30);
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "at_chute fires PURGING from the rear-station zone",
                 "[print][collector][status_signals]") {
    collector().start();
    drain_async_updates();

    send_status_update({{"toolhead", {{"position", {114.0, 230.0, 10.0}}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    REQUIRE(get_current_message() == "Purging...");
    REQUIRE(get_current_progress() == 20);
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Status-signal holds do not re-fire; falling false re-arms",
                 "[print][collector][status_signals]") {
    // A BED_MESH-mapped rule makes a re-fire observable through the sub-phase
    // relabel path: apply_profile_match() relabels BED_MESH on a message
    // change, so an illegitimate re-fire would clobber a console label.
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"edge",)"
        R"("response_patterns":[{"pattern":"sweep marker","phase":"BED_MESH","message":"Loading Bed Mesh...","weight":10}],)"
        R"("status_signals":[{"name":"sweep_zone","object":"toolhead",)"
        R"("when":[{"field":"position","index":1,"op":"gt","value":100}],)"
        R"("phase":"BED_MESH","message":"Sweeping...","weight":10}]})"));
    REQUIRE(profile != nullptr);
    collector().set_profile(profile);
    collector().start();
    drain_async_updates();

    // Rising edge fires.
    send_status_update({{"toolhead", {{"position", {110.0, 120.0, 5.0}}}}});
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    REQUIRE(get_current_message() == "Sweeping...");

    // A console BED_MESH line relabels the same phase - existing semantics.
    send_gcode_response("sweep marker");
    REQUIRE(get_current_message() == "Loading Bed Mesh...");

    // HOLD: a new frame while the predicate still holds must not re-fire and
    // clobber the console relabel.
    send_status_update({{"toolhead", {{"position", {115.0, 125.0, 5.0}}}}});
    REQUIRE(get_current_message() == "Loading Bed Mesh...");

    // Predicate falls false - the rule re-arms.
    send_status_update({{"toolhead", {{"position", {10.0, 12.0, 5.0}}}}});

    // The next rising edge fires again (BED_MESH relabel applies once more).
    send_status_update({{"toolhead", {{"position", {110.0, 130.0, 5.0}}}}});
    REQUIRE(get_current_message() == "Sweeping...");
}

TEST_CASE_METHOD(PrintStartCollectorSequentialFixture,
                 "Status-signal match is confirmation, not narration",
                 "[print][collector][status_signals]") {
    collector().start();
    drain_async_updates();

    SECTION("narration first: the rule may not clobber its label") {
        send_gcode_response("// State: KAMP PRIMING...");
        REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
        REQUIRE(get_current_message() == "Priming nozzle...");
        REQUIRE(get_current_progress() == 90);

        send_status_update({{"toolhead", {{"position", {-2.0, -3.0, 10.0}}}}});
        // PURGING is already detected: the physical signal confirms it but
        // cannot relabel or reset progress.
        REQUIRE(get_current_message() == "Priming nozzle...");
        REQUIRE(get_current_progress() == 90);
    }

    SECTION("rule first: later narration supersedes the inferred label") {
        send_status_update({{"toolhead", {{"position", {-2.0, -3.0, 10.0}}}}});
        REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
        REQUIRE(get_current_message() == "Changing filament...");

        // Console signal formats apply through their own path, so the
        // narration relabels the phase the rule inferred - inference is
        // confirmation and never fights later narration.
        send_gcode_response("// State: KAMP PRIMING...");
        REQUIRE(get_current_message() == "Priming nozzle...");
        REQUIRE(get_current_progress() == 90);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Profiles without status signals ignore rule-object frames",
                 "[print][collector][status_signals]") {
    collector().set_profile(PrintStartProfile::load("creality_k2"));
    collector().start();
    drain_async_updates();

    // The exact frame that fires the sequential fixture's at_cutter rule does nothing here:
    // an absent declaration is exactly the previous behavior, pinned.
    client().dispatch_status_update({{"toolhead", {{"position", {-2.0, -3.0, 10.0}}}}});
    drain_async_updates();

    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE(get_current_message() == "Preparing Print...");
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Default heating rules fire on a cold heater with a target",
                 "[print][collector][status_signals]") {
    collector().start();
    drain_async_updates();

    // No console narration anywhere: the rising edge of "target set while
    // still two degrees short" is the whole evidence.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 23.5}, {"target", 60.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    REQUIRE(get_current_message() == "Heating Bed...");

    client().dispatch_status_update({{"extruder", {{"temperature", 25.0}, {"target", 210.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(get_current_message() == "Heating Nozzle...");
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A status-signal match leaves the proactive detector armed",
                 "[print][collector][status_signals][proactive]") {
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();

    // The default profile's heating rules watch heater_bed and extruder - the
    // same frames the proactive detector reads. A rule holding is inference,
    // not the firmware narrating, so it must not gate that detector off.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 23.5}, {"target", 60.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);

    // Mid-G28 with the bed still climbing. Only the proactive detector can
    // reach HOMING from here: the heating correction beneath it never leaves
    // the two heating phases, so a gated-off detector shows HEATING_BED.
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xy");
    set_all_temps(/*bed*/ 235, 600, /*ext*/ 0, 0);
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();

    INFO("phase=" << static_cast<int>(get_current_phase()) << " msg=" << get_current_message());
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "Default heating rules stay quiet without a real edge",
                 "[print][collector][status_signals]") {
    collector().start();
    drain_async_updates();

    // No target: nothing is heating.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 23.5}, {"target", 0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // Within two degrees of target: the rule's own margin says not heating.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 59.0}, {"target", 60.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);

    // A delta frame without the target field cannot resolve the AND-list, so
    // it does not fire - and it re-arms nothing.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 23.5}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::INITIALIZING);
    REQUIRE(get_current_message() == "Preparing Print...");
}

// ============================================================================
// Simulated-Clock Scaling
// ============================================================================
//
// The collector measures how long a pre-print takes. Reading the simulated
// clock is what lets a mock run reach a 700-second pre-print without spending
// 700 real seconds on it, and what keeps the numbers it records identical at
// every --sim-speed.

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "PrintStartCollector: phase durations come from the simulated clock",
                 "[print][collector][simclock]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    collector().start();
    drain_async_updates();
    drain_async_updates();

    // Enter a phase the collector timestamps, with no time on the clock yet.
    client().dispatch_status_update({{"heater_bed", {{"temperature", 23.5}, {"target", 60.0}}}});
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    REQUIRE(collector().get_current_phase_elapsed_seconds() == 0);

    SECTION("an eleven-minute phase is measured without waiting eleven minutes") {
        clock.advance(std::chrono::seconds(701));
        REQUIRE(collector().get_current_phase_elapsed_seconds() == 701);
    }

    SECTION("the total elapsed the UI shows tracks it too") {
        clock.advance(std::chrono::seconds(701));
        PrintStartCollectorTestAccess::run_eta_update(collector());
        drain_async_updates();
        REQUIRE(lv_subject_get_int(state().print_state().get_preprint_elapsed_subject()) == 701);
    }

    SECTION("fast-forwarding real time is what buys the simulated seconds") {
        // 70.1 real seconds at 10x is the same 701 simulated seconds the phase
        // above took at 1x — the measurement is speed-invariant, the waiting is
        // not.
        helix::sim::SimulatedClock::ManualScope fast(helix::sim::SimSpeed::of(10.0));
        fast.advance_real(std::chrono::milliseconds(70100));
        REQUIRE(collector().get_current_phase_elapsed_seconds() == 701);
    }
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "PrintStartCollector: the stuck-pre-print ceiling is simulated seconds too",
                 "[print][collector][simclock][timeout]") {
    // Elapsed and the timeout constants must share one unit, or a fast-forwarded
    // run either never gives up or gives up instantly. kFallbackTimeout is 300
    // simulated seconds; the boundary is walked from both sides so the negative
    // half cannot pass by the collector simply not running.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    collector().start();
    drain_async_updates();
    drain_async_updates();
    collector().enable_fallbacks();

    // Temps at target and nothing being said: the quiet path. The first call
    // recomputes weights off the freshly-set targets, so the predicted total is
    // cleared afterwards to reach kFallbackTimeout rather than the adaptive
    // ceiling derived from it.
    set_all_temps(600, 600, 2100, 2100);
    collector().check_fallback_completion();
    drain_async_updates();
    PrintStartCollectorTestAccess::clear_prediction_history(collector());
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);

    clock.advance(std::chrono::seconds(299));
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    // Two more simulated seconds is all it takes — no real waiting at all.
    clock.advance(std::chrono::seconds(2));
    collector().check_fallback_completion();
    drain_async_updates();
    drain_async_updates();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

// ============================================================================
// DECLARED HOLDS
//
// A profile pattern can name a capture group holding minutes the printer will
// spend without a word (cosmos_cc1: "Heatsoak: 10.0m", then a silent G4).
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A declared hold counts as the printer talking until it ends",
                 "[print][collector][timeout][hold]") {
    auto profile = PrintStartProfile::load("cosmos_cc1");
    REQUIRE_FALSE(profile->is_default());
    collector().set_profile(std::move(profile));
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1050, 1050, 2600, 2600);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 400);

    SECTION("announced on the console") {
        send_gcode_response("// Heatsoak: 10.0m");
    }
    SECTION("announced only in the display text") {
        client().dispatch_status_update({{"display_status", {{"message", "Heatsoak: 10.0m"}}}});
        drain_async_updates();
    }
    REQUIRE(get_current_phase() == PrintStartPhase::SOAKING);

    // Both heaters at target and nothing said, five seconds short of the hold.
    clock.advance(std::chrono::seconds(595));
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    // The ten minutes, then the usual 90s of quiet.
    clock.advance(std::chrono::seconds(96));
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A declared hold is left out of the time the ceiling measures",
                 "[print][collector][timeout][ceiling][hold]") {
    // A soak announced late in a long pre-print does not spend the ceiling:
    // the minutes it holds are the printer doing what it said it would.
    auto profile = PrintStartProfile::load("cosmos_cc1");
    REQUIRE_FALSE(profile->is_default());
    collector().set_profile(std::move(profile));
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    // A bed that never comes within 2C, so only the ceiling can end this.
    set_all_temps(1000, 1050, 2600, 2600);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 1700);

    // The console line and its display copy announce one hold, not two.
    send_gcode_response("// Heatsoak: 10.0m");
    client().dispatch_status_update({{"display_status", {{"message", "Heatsoak: 10.0m"}}}});
    drain_async_updates();

    // The hold ends at 2300s. At 2395s the printer has been quiet for 95s, but
    // only 1795s of the pre-print were not held.
    clock.advance(std::chrono::seconds(695));
    tick_fallbacks();
    REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);

    clock.advance(std::chrono::seconds(20));
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A printer that keeps announcing holds still ends at the backstop",
                 "[print][collector][timeout][ceiling][hold]") {
    // Each announcement extends the hold by the minute since the last one, so
    // held time keeps pace with the clock. The backstop leaves out at most one
    // ceiling of it: without predictions, 1800s held and 3600s unheld.
    auto profile = PrintStartProfile::load("cosmos_cc1");
    REQUIRE_FALSE(profile->is_default());
    collector().set_profile(std::move(profile));
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    set_all_temps(1000, 1050, 2600, 2600);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();
    PrintStartCollectorTestAccess::set_predicted_total(collector(), 0.0f);
    PrintStartCollectorTestAccess::set_elapsed_seconds(collector(), 3500);

    int elapsed_s = 3500;
    for (; elapsed_s < 5400; elapsed_s += 60) {
        CAPTURE(elapsed_s);
        send_gcode_response("// Heatsoak: 30.0m");
        tick_fallbacks();
        REQUIRE(get_current_phase() != PrintStartPhase::COMPLETE);
        clock.advance(std::chrono::seconds(60));
    }

    CAPTURE(elapsed_s);
    send_gcode_response("// Heatsoak: 30.0m");
    tick_fallbacks();
    REQUIRE(get_current_phase() == PrintStartPhase::COMPLETE);
}

// ============================================================================
// COSMOS (Elegoo Centauri Carbon) pre-print replay
// ============================================================================

/// How a COSMOS replay departs from the recorded print.
struct CosmosReplayVariant {
    /// The heat soak as COSMOS prints it (cosmos.conf heatsoak, 0-30 minutes).
    const char* heatsoak_minutes = "1.0";
    /// false: SMART_PARK and LINE_PURGE print nothing, as with adaptive_purge
    /// off or KAMP's verbose output disabled.
    bool narrates_park_and_purge = true;
};

/**
 * @brief Replay a real Centauri Carbon PRINT_START through the collector
 *
 * From klippy.log of a COSMOS 26.08 print on 2026-09-14 (cold start, 105C bed,
 * 260C nozzle). Heater readings are the log's once-a-second stats sampled at
 * the collector's 5s tick; console lines sit where the log records them,
 * relative to "Starting SD card print". The skew line is not in the log
 * (RESPOND PREFIX goes out raw), so it sits where LINE_PURGE ends.
 *
 * Klipper echoes nothing from inside the macro: the M190 wait is eight minutes
 * of temperature reports and nothing else. The timeline runs on a manual
 * simulated clock, so every stamp the collector takes is where the print put it.
 */
class CosmosPrintStartReplayFixture : public PrintStartCollectorHeaterFixture {
  public:
    struct Result {
        std::string trace;           ///< "second:PHASE" for every phase change
        int completed_at_ms{-1};     ///< timeline position of COMPLETE, -1 if never
        float first_prediction_s{0}; ///< prediction once the heater targets are set
    };

    CosmosPrintStartReplayFixture() {
        ThermalRateManager::instance().reset();
        auto profile = PrintStartProfile::load("cosmos_cc1");
        have_profile_ = !profile->is_default();
        collector().set_profile(std::move(profile));
    }

    ~CosmosPrintStartReplayFixture() override {
        // The collector reads the process-wide manager; later tests must not
        // inherit this printer's rates.
        ThermalRateManager::instance().reset();
    }

    bool have_profile_ = false;

    Result replay(const CosmosReplayVariant& variant = CosmosReplayVariant{}) {
        struct Sample {
            int t_s;
            int bed, bed_target, ext, ext_target; // decidegrees
        };
        // clang-format off
        static constexpr Sample kSamples[] = {
        {0, 245, 0, 266, 0}, {5, 250, 1050, 384, 1400}, {10, 261, 1050, 628, 1400}, {15, 274, 1050, 830, 1400},
        {20, 289, 1050, 1027, 1400}, {25, 304, 1050, 1210, 1400}, {30, 320, 1050, 1358, 1400}, {35, 332, 1050, 1404, 1400},
        {40, 347, 1050, 1412, 1400}, {45, 359, 1050, 1423, 1400}, {50, 373, 1050, 1403, 1400}, {55, 385, 1050, 1403, 1400},
        {60, 398, 1050, 1406, 1400}, {65, 410, 1050, 1395, 1400}, {70, 424, 1050, 1404, 1400}, {75, 437, 1050, 1400, 1400},
        {80, 449, 1050, 1398, 1400}, {85, 459, 1050, 1405, 1400}, {90, 470, 1050, 1396, 1400}, {95, 482, 1050, 1403, 1400},
        {100, 493, 1050, 1399, 1400}, {105, 504, 1050, 1398, 1400}, {110, 516, 1050, 1404, 1400}, {115, 526, 1050, 1396, 1400},
        {120, 537, 1050, 1403, 1400}, {125, 546, 1050, 1398, 1400}, {130, 557, 1050, 1399, 1400}, {135, 566, 1050, 1404, 1400},
        {140, 576, 1050, 1396, 1400}, {145, 586, 1050, 1404, 1400}, {150, 597, 1050, 1399, 1400}, {155, 606, 1050, 1399, 1400},
        {160, 615, 1050, 1404, 1400}, {165, 626, 1050, 1396, 1400}, {170, 635, 1050, 1404, 1400}, {175, 644, 1050, 1397, 1400},
        {180, 654, 1050, 1401, 1400}, {185, 662, 1050, 1400, 1400}, {190, 672, 1050, 1399, 1400}, {195, 680, 1050, 1402, 1400},
        {200, 688, 1050, 1398, 1400}, {205, 697, 1050, 1403, 1400}, {210, 704, 1050, 1397, 1400}, {215, 714, 1050, 1403, 1400},
        {220, 721, 1050, 1397, 1400}, {225, 729, 1050, 1403, 1400}, {230, 738, 1050, 1398, 1400}, {235, 744, 1050, 1402, 1400},
        {240, 752, 1050, 1399, 1400}, {245, 759, 1050, 1399, 1400}, {250, 767, 1050, 1401, 1400}, {255, 775, 1050, 1398, 1400},
        {260, 782, 1050, 1402, 1400}, {265, 790, 1050, 1397, 1400}, {270, 796, 1050, 1403, 1400}, {275, 803, 1050, 1397, 1400},
        {280, 810, 1050, 1403, 1400}, {285, 818, 1050, 1397, 1400}, {290, 825, 1050, 1403, 1400}, {295, 831, 1050, 1398, 1400},
        {300, 838, 1050, 1402, 1400}, {305, 844, 1050, 1399, 1400}, {310, 850, 1050, 1401, 1400}, {315, 857, 1050, 1400, 1400},
        {320, 864, 1050, 1399, 1400}, {325, 870, 1050, 1401, 1400}, {330, 876, 1050, 1399, 1400}, {335, 883, 1050, 1402, 1400},
        {340, 889, 1050, 1398, 1400}, {345, 894, 1050, 1403, 1400}, {350, 900, 1050, 1397, 1400}, {355, 907, 1050, 1403, 1400},
        {360, 913, 1050, 1397, 1400}, {365, 919, 1050, 1403, 1400}, {370, 925, 1050, 1397, 1400}, {375, 931, 1050, 1403, 1400},
        {380, 937, 1050, 1397, 1400}, {385, 943, 1050, 1403, 1400}, {390, 949, 1050, 1397, 1400}, {395, 954, 1050, 1403, 1400},
        {400, 960, 1050, 1398, 1400}, {405, 965, 1050, 1402, 1400}, {410, 971, 1050, 1399, 1400}, {415, 976, 1050, 1400, 1400},
        {420, 981, 1050, 1400, 1400}, {425, 986, 1050, 1399, 1400}, {430, 992, 1050, 1402, 1400}, {435, 997, 1050, 1398, 1400},
        {440, 1002, 1050, 1403, 1400}, {445, 1006, 1050, 1397, 1400}, {450, 1010, 1050, 1403, 1400}, {455, 1015, 1050, 1397, 1400},
        {460, 1020, 1050, 1403, 1400}, {465, 1024, 1050, 1397, 1400}, {470, 1029, 1050, 1403, 1400}, {475, 1032, 1050, 1397, 1400},
        {480, 1037, 1050, 1403, 1400}, {485, 1041, 1050, 1398, 1400}, {490, 1044, 1050, 1402, 1400}, {495, 1047, 1050, 1398, 1400},
        {500, 1049, 1050, 1402, 1400}, {505, 1052, 1050, 1399, 1400}, {510, 1054, 1050, 1401, 1400}, {515, 1056, 1050, 1399, 1400},
        {520, 1057, 1050, 1400, 1400}, {525, 1058, 1050, 1400, 1400}, {530, 1058, 1050, 1400, 1400}, {535, 1058, 1050, 1401, 1400},
        {540, 1059, 1050, 1399, 1400}, {545, 1059, 1050, 1400, 2600}, {550, 1059, 1050, 1555, 2600}, {555, 1059, 1050, 1742, 2600},
        {560, 1058, 1050, 1905, 2600}, {565, 1057, 1050, 2046, 2600}, {570, 1057, 1050, 2186, 2600}, {575, 1057, 1050, 2317, 2600},
        {580, 1056, 1050, 2431, 2600}, {585, 1055, 1050, 2550, 2600}, {590, 1054, 1050, 2590, 2600}, {595, 1053, 1050, 2616, 2600},
        {600, 1053, 1050, 2617, 2600}, {605, 1052, 1050, 2601, 2600}, {610, 1051, 1050, 2606, 2600}, {615, 1051, 1050, 2597, 2600},
        {620, 1050, 1050, 2598, 2600},
        };
        // clang-format on

        struct Narration {
            int t_ms;
            std::string console;
            std::string display; ///< SET_DISPLAY_TEXT also lands in display_status
        };
        // The recorded soak narrates at 484.4s and holds the heaters through the
        // 540s sample; a longer soak moves everything after it later.
        constexpr int kSoakNarrationMs = 484400;
        constexpr int kSoakHeldThroughS = 540;
        const int extra_s =
            static_cast<int>(std::lround((std::stod(variant.heatsoak_minutes) - 1.0) * 60.0));
        const int extra_ms = extra_s * 1000;
        const std::string soak = std::string("Heatsoak: ") + variant.heatsoak_minutes + "m";
        std::vector<Narration> narration = {
            {kSoakNarrationMs, "// " + soak, soak},
            {542500 + extra_ms, "// Using default bed mesh", "Using default bed mesh"},
            {542600 + extra_ms, "// Smart Park location: 41.0714,33.0046.", ""},
            {605600 + extra_ms,
             "// KAMP purge is not using firmware retraction, it is recommended to configure it.",
             ""},
            {605700 + extra_ms,
             "// KAMP purge starting at 113.0002, 33.0046 and purging 30.0mm of filament, "
             "requested flow rate is 12.0mm3/s.",
             ""},
            {618500 + extra_ms,
             "Info: No skew profile defined. If skew correction is desired, create "
             "'my_skew_profile' in printer.cfg",
             ""},
        };
        if (!variant.narrates_park_and_purge) {
            narration.erase(std::remove_if(narration.begin(), narration.end(),
                                           [](const Narration& n) {
                                               return n.console.find("Smart Park") !=
                                                          std::string::npos ||
                                                      n.console.find("KAMP") != std::string::npos;
                                           }),
                            narration.end());
        }
        // Through the extra soak time the heaters read what they read at 540s.
        std::vector<Sample> timeline;
        for (const Sample& s : kSamples) {
            if (s.t_s > kSoakHeldThroughS) {
                timeline.push_back({s.t_s + extra_s, s.bed, s.bed_target, s.ext, s.ext_target});
                continue;
            }
            timeline.push_back(s);
            if (s.t_s == kSoakHeldThroughS) {
                for (int t = s.t_s + 5; t <= s.t_s + extra_s; t += 5) {
                    timeline.push_back({t, s.bed, s.bed_target, s.ext, s.ext_target});
                }
            }
        }
        // The M190 wait releases just before the heat soak narration; M109
        // starts with the smart park and reports until the nozzle arrives.
        constexpr int kBedWaitEndMs = 484400;
        const int nozzle_wait_start_ms = 543600 + extra_ms;
        const int nozzle_wait_end_ms = 587600 + extra_ms;

        Result result;
        int now_ms = 0;
        PrintStartPhase shown = PrintStartPhase::IDLE;
        const auto advance_to = [&](int t_ms) {
            clock_.advance(std::chrono::milliseconds(t_ms - now_ms));
            now_ms = t_ms;
        };
        const auto note_phase = [&]() {
            drain_async_updates();
            drain_async_updates();
            const PrintStartPhase phase = get_current_phase();
            if (phase == shown) {
                return;
            }
            shown = phase;
            if (!result.trace.empty()) {
                result.trace += ' ';
            }
            result.trace +=
                std::to_string(now_ms / 1000) + ":" + std::string(print_start_phase_name(phase));
            if (phase == PrintStartPhase::COMPLETE) {
                result.completed_at_ms = now_ms;
            }
        };

        const Sample& first = kSamples[0];
        set_all_temps(first.bed, first.bed_target, first.ext, first.ext_target);
        collector().start();
        collector().enable_fallbacks();
        note_phase();

        size_t next = 0;
        for (const Sample& s : timeline) {
            while (next < narration.size() && narration[next].t_ms <= s.t_s * 1000) {
                const Narration& n = narration[next++];
                advance_to(n.t_ms);
                send_gcode_response(n.console);
                if (!n.display.empty()) {
                    client().dispatch_status_update({{"display_status", {{"message", n.display}}}});
                }
                note_phase();
            }
            if (result.completed_at_ms >= 0) {
                break;
            }

            advance_to(s.t_s * 1000);
            set_all_temps(s.bed, s.bed_target, s.ext, s.ext_target);
            const bool waiting = (s.bed_target > 0 && now_ms < kBedWaitEndMs) ||
                                 (now_ms >= nozzle_wait_start_ms && now_ms <= nozzle_wait_end_ms);
            if (waiting) {
                char report[64];
                std::snprintf(report, sizeof(report), "B:%.1f /%.1f T0:%.1f /%.1f", s.bed / 10.0,
                              s.bed_target / 10.0, s.ext / 10.0, s.ext_target / 10.0);
                send_gcode_response(report);
            }
            collector().check_fallback_completion();
            // Before any heating is measured, the prediction is the defaults'.
            if (s.bed_target > 0 && result.first_prediction_s == 0.0f) {
                result.first_prediction_s =
                    PrintStartCollectorTestAccess::get_predicted_total(collector());
            }
            PrintStartCollectorTestAccess::run_eta_update(collector());
            note_phase();
        }
        return result;
    }

  private:
    /// The default phases a first print on this printer reads, and no saved history.
    PreprintConfigScope config_{"Elegoo Centauri Carbon"};
    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};

TEST_CASE_METHOD(CosmosPrintStartReplayFixture,
                 "PrintStartCollector: real COSMOS pre-print completes at its last step",
                 "[print][collector][cosmos][integration]") {
    REQUIRE(have_profile_);
    // bed_x_max 256 reads as a medium bed to the size guess.
    constexpr float kBedXMax = 256.0f;
    float min_prediction_s = 0.0f;

    SECTION("with the size-guess heating rates") {
        // 1.5 s/C predicts about a third of the real pre-print, so every
        // elapsed-time threshold short of the ceiling is spent before the bed
        // nears its target.
        ThermalRateManager::instance().apply_archetype_defaults(kBedXMax, "");
    }
    SECTION("with the heating rates from the printer database") {
        ThermalRateManager::instance().apply_archetype_defaults(kBedXMax, "Elegoo Centauri Carbon");
        // The first print is predicted within 10% of the 618s it took.
        min_prediction_s = 618.0f * 0.9f;
    }

    const Result result = replay();
    CAPTURE(result.trace, result.first_prediction_s);
    REQUIRE(result.trace == "0:INITIALIZING 5:HEATING_BED 484:SOAKING 542:BED_MESH "
                            "542:HEATING_NOZZLE 605:PURGING 618:COMPLETE");
    REQUIRE(result.completed_at_ms == 618500);
    REQUIRE(result.first_prediction_s >= min_prediction_s);
}

TEST_CASE_METHOD(CosmosPrintStartReplayFixture,
                 "PrintStartCollector: a long COSMOS heat soak holds the pre-print open",
                 "[print][collector][cosmos][integration]") {
    REQUIRE(have_profile_);
    // The soak is one "Heatsoak: 10.0m" line, then ten silent minutes of G4
    // with both heaters already at target: nothing else the collector can see
    // says the printer is still working.
    ThermalRateManager::instance().apply_archetype_defaults(256.0f, "Elegoo Centauri Carbon");

    CosmosReplayVariant variant;
    variant.heatsoak_minutes = "10.0";
    const Result result = replay(variant);
    CAPTURE(result.trace);
    REQUIRE(result.trace == "0:INITIALIZING 5:HEATING_BED 484:SOAKING 1082:BED_MESH "
                            "1082:HEATING_NOZZLE 1145:PURGING 1158:COMPLETE");
    REQUIRE(result.completed_at_ms == 618500 + 540000);
}

TEST_CASE_METHOD(CosmosPrintStartReplayFixture,
                 "PrintStartCollector: a COSMOS pre-print learns the heating rates it took",
                 "[print][collector][cosmos][thermal_rate]") {
    REQUIRE(have_profile_);
    // The bed took ~484s from 24.5C to 105C, about 6 s/C. The nozzle climbed
    // to 140C in ~30s, held there for eight minutes, then took ~45s from 140C
    // to 260C, about 0.38 s/C.
    ThermalRateManager::instance().apply_archetype_defaults(256.0f, "Elegoo Centauri Carbon");

    const Result result = replay();
    REQUIRE(result.completed_at_ms == 618500);

    auto& rates = ThermalRateManager::instance();
    const float bed = rates.get_model("heater_bed").blended_rate_for_save();
    const float nozzle = rates.get_model("extruder").blended_rate_for_save();
    CAPTURE(bed, nozzle);
    CHECK(bed >= 5.0f);
    CHECK(bed <= 7.0f);
    CHECK(nozzle >= 0.3f);
    CHECK(nozzle <= 0.6f);

    // The completion saved those rates for the next print.
    helix::Config* cfg = helix::Config::get_instance();
    CHECK(cfg->get<float>("/thermal/rates/heater_bed/heat_rate", 0.0f) == Catch::Approx(bed));
    CHECK(cfg->get<float>("/thermal/rates/extruder/heat_rate", 0.0f) == Catch::Approx(nozzle));
}

TEST_CASE_METHOD(CosmosPrintStartReplayFixture,
                 "PrintStartCollector: a quiet COSMOS park and purge shows the nozzle heating",
                 "[print][collector][cosmos][integration]") {
    REQUIRE(have_profile_);
    // Without the smart park narration, the stored mesh line is the last word
    // before M109 raises the nozzle from 140C to 260C. The wait's first report
    // (543.6s) is what moves the label, on the next fallback tick.
    ThermalRateManager::instance().apply_archetype_defaults(256.0f, "Elegoo Centauri Carbon");

    CosmosReplayVariant variant;
    variant.narrates_park_and_purge = false;
    const Result result = replay(variant);
    CAPTURE(result.trace);
    REQUIRE(result.trace == "0:INITIALIZING 5:HEATING_BED 484:SOAKING 542:BED_MESH "
                            "545:HEATING_NOZZLE 618:COMPLETE");
}

// ============================================================================
// REAL FORGE-X 1.4.2 PRE-PRINT REPLAY
//
// An AD5M Pro on Forge-X 1.4.2 (2026-10-15, KAMP, ABS 105/265): the console
// (tests/fixtures/forgex_142_console.txt), the heaters every 5s
// (tests/fixtures/forgex_142_heaters.txt) and the operation_context frames,
// at their real times. 1.4.2 prints no "// State:" lines; it narrates through
// operation_context, which the capture did not record. kForgeXContextFrames
// rebuilds it from the macros (base.cfg START_PRINT, KAMP, CLEAR_NOZZLE,
// _WAIT_TEMPERATURE) against the console, as the deltas Moonraker sends.
// ============================================================================

namespace {
struct ForgeXFrame {
    const char* at; ///< HH:MM:SS.mmm
    const char* fields;
};

// clang-format off
const ForgeXFrame kForgeXContextFrames[] = {
    {"16:19:29.110", R"({"context_types":["print"],"current_state":null})"},
    {"16:19:31.730", R"({"current_state":"CHECKING FILE"})"},
    {"16:19:33.000", R"({"current_state":"HOMING"})"},
    {"16:19:37.400", R"({"current_state":"LEVELING"})"},
    {"16:19:37.440", R"({"context_types":["print","kamp"],"current_state":null})"},
    {"16:19:37.444", R"({"context_types":["print","kamp","nozzle_clean"]})"},
    {"16:19:37.600", R"({"current_state":"HEATING"})"},
    {"16:19:45.640", R"({"current_state":"HEATING BED"})"},
    {"16:27:12.290", R"({"current_state":"HEATING NOZZLE"})"},
    {"16:28:01.640", R"({"current_state":"PREPARING TO CLEAN"})"},
    {"16:28:15.950", R"({"current_state":"CLEANING"})"},
    {"16:28:24.440", R"({"current_state":"COOLING NOZZLE"})"},
    {"16:30:16.120", R"({"current_state":"FINISHING"})"},
    {"16:30:17.400", R"({"context_types":["print","kamp"],"current_state":"LEVELING"})"},
    {"16:32:44.605", R"({"context_types":["print"],"current_state":"PARKING"})"},
    {"16:32:46.690", R"({"current_state":"HEATING NOZZLE"})"},
    {"16:33:22.920", R"({"current_state":"PARKING"})"},
    {"16:33:23.120", R"({"current_state":"PRIMING"})"},
    {"16:33:37.500", R"({"current_state":"PRINTING"})"},
};
// clang-format on

int forgex_clock_ms(const std::string& hhmmss) {
    int h = 0, m = 0;
    double sec = 0.0;
    if (std::sscanf(hhmmss.c_str(), "%d:%d:%lf", &h, &m, &sec) != 3) {
        return -1;
    }
    return static_cast<int>(std::lround((h * 3600 + m * 60 + sec) * 1000.0));
}

/// Fixture lines "HH:MM:SS.mmm rest", '#' lines skipped.
std::vector<std::pair<int, std::string>> read_forgex_fixture(const std::string& name) {
    std::vector<std::pair<int, std::string>> rows;
    std::ifstream f(collector_fixture_dir() + name);
    for (std::string raw; std::getline(f, raw);) {
        if (raw.empty() || raw[0] == '#') {
            continue;
        }
        rows.emplace_back(forgex_clock_ms(raw.substr(0, 12)), raw.substr(13));
    }
    return rows;
}
} // namespace

class ForgeXPrintStartReplayFixture : public ForgeXCollectorFixture {
  public:
    struct Result {
        std::string trace;          ///< "second:PHASE" for every phase change
        int completed_at_ms{-1};    ///< timeline position of COMPLETE, -1 if never
        int mesh_entered_at_ms{-1}; ///< first BED_MESH, -1 if never
        int mesh_probes_at_entry{-1};
        int mesh_probe_current{0}; ///< when the mesh ended
        int mesh_probe_total{0};
        std::string mesh_message;    ///< the last mesh label
        float first_prediction_s{0}; ///< prediction once the bed target is set
        float cooldown_prediction_s{0};
        float max_prediction_s{0};        ///< highest prediction once the bed target is set
        float min_prediction_s{1.0e9f};   ///< lowest prediction once the bed target is set
        bool live_step_at_priming{false}; ///< in_live_preprint_step() once priming starts
        bool live_step_at_complete{true};
        std::string predictions; ///< "second:total" at every heater sample
    };

    ForgeXPrintStartReplayFixture() {
        ThermalRateManager::instance().reset();
        // What the printer answered: the clean cleared the live mesh, so the
        // denominator comes from [bed_mesh] probe_count.
        client().set_config_bed_mesh_probe_count(5, 5);
        client().gcode_script("BED_MESH_CLEAR");
    }

    ~ForgeXPrintStartReplayFixture() override {
        ThermalRateManager::instance().reset();
    }

    /// @param history_total_s Seconds of one saved pre-print, or 0 for none.
    Result replay(int history_total_s = 0) {
        struct Event {
            int ms;
            enum Kind { CONSOLE, HEATERS, FRAME } kind;
            std::string text;
        };
        std::vector<Event> events;
        for (auto& [ms, text] : read_forgex_fixture("forgex_142_console.txt")) {
            std::string line;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    line += text[i + 1] == 'n' ? '\n' : text[i + 1];
                    ++i;
                } else {
                    line += text[i];
                }
            }
            events.push_back({ms, Event::CONSOLE, line});
        }
        for (auto& [ms, text] : read_forgex_fixture("forgex_142_heaters.txt")) {
            events.push_back({ms, Event::HEATERS, text});
        }
        for (const ForgeXFrame& f : kForgeXContextFrames) {
            events.push_back({forgex_clock_ms(f.at), Event::FRAME, f.fields});
        }
        REQUIRE(events.size() == 159 + 172 + std::size(kForgeXContextFrames));
        std::stable_sort(events.begin(), events.end(),
                         [](const Event& a, const Event& b) { return a.ms < b.ms; });

        const int t0 = forgex_clock_ms("16:19:28.442"); // SDCARD_PRINT_FILE
        // The priming line's first extrusion: print_duration starts counting.
        const int priming_ms = forgex_clock_ms("16:33:23.130") - t0;
        bool priming_seen = false;
        const int cooldown_ms = forgex_clock_ms("16:29:30.000") - t0;
        Result result;
        int now_ms = 0;
        PrintStartPhase shown = PrintStartPhase::IDLE;
        const auto note_phase = [&]() {
            drain_async_updates();
            drain_async_updates();
            const PrintStartPhase phase = get_current_phase();
            if (phase == PrintStartPhase::BED_MESH) {
                result.mesh_message = get_current_message();
                result.mesh_probe_current =
                    PrintStartCollectorTestAccess::get_mesh_probe_current(collector());
                result.mesh_probe_total =
                    PrintStartCollectorTestAccess::get_mesh_probe_total(collector());
            }
            if (phase == shown) {
                return;
            }
            shown = phase;
            if (!result.trace.empty()) {
                result.trace += ' ';
            }
            result.trace +=
                std::to_string(now_ms / 1000) + ":" + std::string(print_start_phase_name(phase));
            if (phase == PrintStartPhase::COMPLETE) {
                result.completed_at_ms = now_ms;
            }
            if (phase == PrintStartPhase::BED_MESH && result.mesh_entered_at_ms < 0) {
                result.mesh_entered_at_ms = now_ms;
                result.mesh_probes_at_entry = result.mesh_probe_current;
            }
        };

        set_all_temps(275, 0, 348, 0);
        collector().start();
        collector().enable_fallbacks();
        if (history_total_s > 0) {
            helix::PreprintEntry entry{};
            entry.total_seconds = history_total_s;
            entry.timestamp = 1;
            entry.phase_durations = {{static_cast<int>(PrintStartPhase::CLEANING), 120}};
            entry.temp_bucket = 1;
            entry.window = helix::PreprintWindow::PrinterEdge;
            PrintStartCollectorTestAccess::load_prediction_entries(collector(), {entry});
        }
        note_phase();

        for (const Event& e : events) {
            const int at = e.ms - t0;
            clock_.advance(std::chrono::milliseconds(at - now_ms));
            now_ms = at;
            switch (e.kind) {
            case Event::CONSOLE:
                send_gcode_response(e.text);
                break;
            case Event::FRAME:
                context(json::parse(e.text));
                break;
            case Event::HEATERS: {
                float bed = 0, ext = 0;
                int bed_target = 0, ext_target = 0;
                REQUIRE(std::sscanf(e.text.c_str(), "%f %d %f %d", &bed, &bed_target, &ext,
                                    &ext_target) == 4);
                set_all_temps(static_cast<int>(std::lround(bed * 10)), bed_target * 10,
                              static_cast<int>(std::lround(ext * 10)), ext_target * 10);
                collector().check_fallback_completion();
                if (bed_target > 0 && result.first_prediction_s == 0.0f) {
                    result.first_prediction_s =
                        PrintStartCollectorTestAccess::get_predicted_total(collector());
                }
                const float predicted =
                    PrintStartCollectorTestAccess::get_predicted_total(collector());
                if (now_ms >= cooldown_ms && result.cooldown_prediction_s == 0.0f) {
                    result.cooldown_prediction_s = predicted;
                }
                if (bed_target > 0) {
                    result.max_prediction_s = std::max(result.max_prediction_s, predicted);
                    result.min_prediction_s = std::min(result.min_prediction_s, predicted);
                }
                result.predictions += std::to_string(now_ms / 1000) + ":" +
                                      std::to_string(static_cast<int>(predicted)) + " ";
                PrintStartCollectorTestAccess::run_eta_update(collector());
                break;
            }
            }
            note_phase();
            if (!priming_seen && now_ms >= priming_ms) {
                priming_seen = true;
                result.live_step_at_priming = collector().in_live_preprint_step();
            }
            if (result.completed_at_ms >= 0) {
                result.live_step_at_complete = collector().in_live_preprint_step();
                break;
            }
        }
        return result;
    }

  private:
    PreprintConfigScope config_{"FlashForge Adventurer 5M Pro (ForgeX)"};
    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};

TEST_CASE_METHOD(ForgeXPrintStartReplayFixture,
                 "PrintStartCollector: real Forge-X 1.4.2 pre-print completes at PRINTING",
                 "[print][collector][forge_x][integration]") {
    constexpr float kBedXMax = 220.0f;
    int history_total_s = 0;
    float min_prediction_s = 0.0f;
    float max_prediction_s = 1.0e6f;
    float max_whole_run_s = 849.0f * 1.3f;

    SECTION("with the size-guess heating rates") {
        ThermalRateManager::instance().apply_archetype_defaults(kBedXMax, "");
    }
    SECTION("with the heating rates from the printer database") {
        ThermalRateManager::instance().apply_archetype_defaults(
            kBedXMax, "FlashForge Adventurer 5M Pro (ForgeX)");
        // The first print is predicted within 15% of the 849s it took.
        min_prediction_s = 849.0f * 0.85f;
        max_prediction_s = 849.0f * 1.15f;
    }
    SECTION("with a history shorter than this pre-print") {
        // A saved pre-print a timeout cut short, recording no heating: the
        // start pays at least its own heating, so the short history cannot
        // pull the deadline in front of the silent nozzle cool-down.
        ThermalRateManager::instance().apply_archetype_defaults(kBedXMax, "");
        history_total_s = 380;
    }

    const Result result = replay(history_total_s);
    CAPTURE(result.trace, result.first_prediction_s, result.cooldown_prediction_s,
            result.mesh_message);
    if (history_total_s > 0) {
        // The trace heats from HEATING_BED (9s) to CLEANING (513s).
        constexpr float kTraceHeatingS = 513.0f - 9.0f;
        REQUIRE(result.cooldown_prediction_s > static_cast<float>(history_total_s));
        REQUIRE(result.cooldown_prediction_s >= kTraceHeatingS);
    }

    // The pre-print ends at print/PRINTING (849.1s), not at the clean's
    // FINISHING (647.7s) or any timeout before it.
    REQUIRE(result.completed_at_ms == 849058);
    REQUIRE(result.trace == "0:INITIALIZING 4:HOMING 9:HEATING_BED 463:HEATING_NOZZLE "
                            "513:CLEANING 648:BED_MESH 796:HEATING_NOZZLE 834:PURGING "
                            "849:COMPLETE");

    // The mesh starts at kamp/LEVELING (648.96s), not at the print's LEVELING
    // (8.96s), and the two cleaning probes are not its first points.
    REQUIRE(result.mesh_entered_at_ms == 648958);
    REQUIRE(result.mesh_probes_at_entry == 0);
    REQUIRE(result.mesh_probe_total == 25);
    REQUIRE(result.mesh_probe_current == 25);
    REQUIRE(result.mesh_message == "Creating bed mesh (25/25)");

    // The priming line starts print_duration 14s before PRINTING. This printer
    // reports layers, so only layer 1 completes; one that did not would fall
    // back to first extrusion, which the priming step holds.
    REQUIRE(result.live_step_at_priming);
    REQUIRE_FALSE(MoonrakerManager::should_complete_preprint(
        /*printer_reports_layers=*/false, /*current_layer=*/0, /*print_duration=*/1,
        /*seen_layer_zero=*/false, /*layer_advanced=*/false, result.live_step_at_priming));
    REQUIRE_FALSE(result.live_step_at_complete);

    REQUIRE(result.first_prediction_s >= min_prediction_s);
    REQUIRE(result.first_prediction_s <= max_prediction_s);
    // With the database rates the bed's fast first degrees do not drag the
    // estimate down while it climbs.
    REQUIRE(result.min_prediction_s >= min_prediction_s);
    // The prediction holds near the real length through the whole pre-print,
    // the slow last degrees of the bed included.
    INFO(result.predictions);
    REQUIRE(result.max_prediction_s <= max_whole_run_s);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A bed's slow last degrees do not inflate the predicted pre-print",
                 "[print][collector][eta][thermal_rate]") {
    // A generic printer: the bed heats from cold toward 100C the way a bed
    // does, fast then crawling (100 - 75*e^(-t/150)), and only once it is
    // within 2C does the macro set the nozzle target. The predicted total is
    // recomputed then, with the bed deep in its tail.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    PreprintConfigScope config{""};
    ThermalRateManager::instance().reset();
    ThermalRateManager::instance().apply_archetype_defaults(235.0f, "");

    set_all_temps(250, 0, 250, 0);
    collector().start();
    drain_async_updates();
    collector().enable_fallbacks();

    float max_prediction = 0.0f;
    int nozzle_set_at = -1;
    int done_at = -1;
    float ext = 25.0f;
    for (int t = 1; t <= 900 && done_at < 0; ++t) {
        clock.advance(std::chrono::seconds(1));
        const float bed = 100.0f - 75.0f * std::exp(-static_cast<float>(t) / 150.0f);
        if (nozzle_set_at < 0 && bed >= 98.0f) {
            nozzle_set_at = t;
        }
        if (nozzle_set_at >= 0) {
            ext = std::min(220.0f, ext + 4.0f);
            if (ext >= 220.0f) {
                done_at = t;
            }
        }
        set_all_temps(static_cast<int>(bed * 10), 1000, static_cast<int>(ext * 10),
                      nozzle_set_at >= 0 ? 2200 : 0);
        collector().check_fallback_completion();
        PrintStartCollectorTestAccess::run_eta_update(collector());
        drain_async_updates();
        if (t >= 5) {
            max_prediction = std::max(
                max_prediction, PrintStartCollectorTestAccess::get_predicted_total(collector()));
        }
    }
    REQUIRE(done_at > 0);
    CAPTURE(nozzle_set_at, done_at, max_prediction);
    // The generic default phases (homing, mesh, QGL, Z tilt, clean, purge:
    // 260s) count toward the total whether or not this start runs them.
    constexpr float kGenericDefaultPhasesS = 260.0f;
    REQUIRE(max_prediction <= 1.3f * static_cast<float>(done_at) + kGenericDefaultPhasesS);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "A bed mesh with no probing under way gives way to the nozzle heat after it",
                 "[print][collector][heating][mesh]") {
    // A stored mesh loads in under a second, and a macro that goes on to heat
    // the nozzle to print temperature without a word leaves nothing else to
    // move the display off the mesh.
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));

    SECTION("a nozzle target raised after the mesh began") {
        set_all_temps(1050, 1050, 1400, 1400);
        collector().start();
        drain_async_updates();
        collector().enable_fallbacks();
        tick_fallbacks();
        send_gcode_response("BED_MESH_CALIBRATE");
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

        set_all_temps(1050, 1050, 1400, 2600); // M109 S260
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

        SECTION("gives way once the nozzle climbs") {
            set_all_temps(1050, 1050, 1600, 2600);
            tick_fallbacks();
            REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
            REQUIRE(get_current_message() == "Heating Nozzle...");
        }

        SECTION("keeps the mesh while a probe line is recent") {
            send_gcode_response("// probe at 100.000,100.000 is z=0.031000");
            set_all_temps(1050, 1050, 1600, 2600);
            tick_fallbacks();
            REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

            clock.advance(std::chrono::seconds(31));
            set_all_temps(1050, 1050, 1800, 2600);
            tick_fallbacks();
            REQUIRE(get_current_phase() == PrintStartPhase::HEATING_NOZZLE);
        }
    }

    SECTION("a nozzle still climbing to the target it had when the mesh began") {
        set_all_temps(1050, 1050, 1000, 1500);
        collector().start();
        drain_async_updates();
        collector().enable_fallbacks();
        tick_fallbacks();
        send_gcode_response("BED_MESH_CALIBRATE");
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

        set_all_temps(1050, 1050, 1100, 1500);
        tick_fallbacks();
        set_all_temps(1050, 1050, 1200, 1500);
        tick_fallbacks();
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    }
}

/**
 * @brief Replay a silent Voron V0 PRINT_START through the generic profile
 *
 * From a 2026-10 capture: G28 with both heaters off, SET_DISPLAY_TEXT
 * "Bed: 90c", a move to the bed centre, M190 S90 (30C to 90C in about 5.5
 * minutes), SET_DISPLAY_TEXT "Hotend: 270c", M109 S270 (32C to 270C in 54s),
 * SKEW_PROFILE LOAD, a KAMP LINE_PURGE, then layer 1. Nothing is narrated to
 * the console beyond Klipper's once-a-second heater wait reports, and there is
 * no bed mesh. Heater readings are linear between the capture's stamps, sent
 * once a second the way Moonraker forwards them; the collector's timer runs
 * every 5s. Second 0 is 22:48:15.
 */
class SilentVoronReplayFixture : public PrintStartCollectorHeaterFixture {
  public:
    struct Snapshot {
        PrintStartPhase phase{PrintStartPhase::IDLE};
        int remaining{0};
        int progress{0};
    };

    static constexpr int kBedTargetS = 15;    // 22:48:30
    static constexpr int kBedAtTargetS = 350; // 22:54:05
    static constexpr int kExtTargetS = 352;   // 22:54:07
    static constexpr int kExtAtTargetS = 406; // 22:55:01
    static constexpr int kPurgeStartS = 407;  // 22:55:02
    static constexpr int kFirstLayerS = 415;  // 22:55:10

    SilentVoronReplayFixture() {
        ThermalRateManager::instance().reset();
        // A V0's 120mm bed reads as a small bed to the size guess.
        ThermalRateManager::instance().apply_archetype_defaults(120.0f, "Voron 0.2");
    }

    ~SilentVoronReplayFixture() override {
        ThermalRateManager::instance().reset();
    }

    void settle() {
        drain_async_updates();
        drain_async_updates();
    }

    int remaining() {
        return lv_subject_get_int(state().print_state().get_preprint_remaining_subject());
    }

    void set_homed(const char* axes) {
        lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), axes);
        // The homed_axes observer runs the fallback check on every change.
        tick_fallbacks();
    }

    void display(const std::string& message) {
        client().dispatch_status_update({{"display_status", {{"message", message}}}});
        settle();
    }

    /// Runs the capture to layer 1 and returns what the panel showed each second.
    std::vector<Snapshot> replay() {
        std::vector<Snapshot> shown(kFirstLayerS + 1);
        const auto lerp = [](int t, int t0, int t1, double v0, double v1) {
            if (t <= t0) {
                return v0;
            }
            if (t >= t1) {
                return v1;
            }
            return v0 + (v1 - v0) * (t - t0) / static_cast<double>(t1 - t0);
        };

        lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "");
        set_all_temps(300, 0, 320, 0);
        collector().start();
        collector().enable_fallbacks();
        collector().note_current_layer(0);
        settle();

        for (int t = 0; t <= kFirstLayerS; ++t) {
            if (t > 0) {
                clock_.advance(std::chrono::seconds(1));
            }
            const double bed = lerp(t, kBedTargetS, kBedAtTargetS, 30.0, 90.0);
            const double ext = lerp(t, kExtTargetS, kExtAtTargetS, 32.0, 270.0);
            const int bed_target = t >= kBedTargetS ? 90 : 0;
            const int ext_target = t >= kExtTargetS ? 270 : 0;

            if (t == 3) {
                set_homed("xy");
            } else if (t == 5) {
                set_homed("xyz");
            } else if (t == 12) {
                display("Bed: 90c");
            } else if (t == kExtTargetS - 1) {
                display("Hotend: 270c");
            }

            set_all_temps(static_cast<int>(std::lround(bed * 10)), bed_target * 10,
                          static_cast<int>(std::lround(ext * 10)), ext_target * 10);
            client().dispatch_status_update(
                {{"heater_bed", {{"temperature", bed}, {"target", bed_target}}},
                 {"extruder", {{"temperature", ext}, {"target", ext_target}}}});
            const bool bed_wait = t >= kBedTargetS && t <= kBedAtTargetS;
            const bool ext_wait = t >= kExtTargetS && t <= kExtAtTargetS;
            if (bed_wait || ext_wait) {
                char report[64];
                std::snprintf(report, sizeof(report), "B:%.1f /%d.0 T0:%.1f /%d.0", bed, bed_target,
                              ext, ext_target);
                send_gcode_response(report);
            }
            if (t >= kPurgeStartS && t < kFirstLayerS) {
                client().dispatch_status_update(
                    {{"motion_report",
                      {{"live_extruder_velocity", 2.5}, {"live_velocity", 50.0}}}});
            } else if (t == kFirstLayerS) {
                client().dispatch_status_update(
                    {{"motion_report", {{"live_extruder_velocity", 0.0}}}});
            }
            settle();

            // The heater-target observers run the fallback check on a new target.
            if (t == kBedTargetS || t == kExtTargetS) {
                tick_fallbacks();
            }
            if (t % 5 == 0) {
                PrintStartCollectorTestAccess::run_eta_update(collector());
                tick_fallbacks();
            }

            if (t == kFirstLayerS) {
                collector().note_current_layer(1);
                collector().complete_from_external_signal("first layer");
                settle();
            }
            shown[t] = {
                get_current_phase(), remaining(),
                lv_subject_get_int(state().print_state().get_print_start_progress_subject())};
        }
        return shown;
    }

  protected:
    // The Voron 0.2 database defaults a first print reads, and no saved history.
    PreprintConfigScope config_{"Voron 0.2"};
    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: silent Voron V0 start shows its steps and learns its length",
                 "[print][collector][preprint][silent_voron][integration]") {
    const auto shown = replay();

    // A cold G28 runs before either heater has a target.
    CHECK(shown[3].phase == PrintStartPhase::HOMING);
    // SET_DISPLAY_TEXT names the bed heat before M190 sets its target.
    CHECK(shown[13].phase == PrintStartPhase::HEATING_BED);

    // At 22:51 the bed has about 3 minutes left and the nozzle another minute:
    // the countdown follows the bed's measured rate, not the size guess.
    const int actual_remaining_at_165 = kFirstLayerS - 165;
    CHECK(shown[165].phase == PrintStartPhase::HEATING_BED);
    CHECK(shown[165].remaining >= actual_remaining_at_165 * 9 / 10);
    CHECK(shown[165].remaining <= actual_remaining_at_165 * 3 / 2);

    CHECK(shown[kExtTargetS - 1].phase == PrintStartPhase::HEATING_NOZZLE);

    // Once the nozzle is at temperature only the purge is left, and the bar
    // credits the mesh and cleaning this macro never ran.
    CHECK(shown[kPurgeStartS + 3].phase == PrintStartPhase::PURGING);
    CHECK(shown[kPurgeStartS + 3].remaining <= 15);
    CHECK(shown[kPurgeStartS + 3].progress >= 90);

    REQUIRE(shown[kFirstLayerS].phase == PrintStartPhase::COMPLETE);

    // The finished start is history, and the bed's measured rate is saved.
    const auto entries = helix::PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].total_seconds == kFirstLayerS);
    helix::Config* cfg = helix::Config::get_instance();
    CHECK(cfg->get<float>("/thermal/rates/heater_bed/heat_rate", 0.0f) ==
          Catch::Approx(5.6f).margin(0.6f));

    // The next print opens on that history: about 7 minutes.
    collector().stop();
    settle();
    set_all_temps(300, 0, 320, 0);
    collector().start();
    settle();
    PrintStartCollectorTestAccess::run_eta_update(collector());
    settle();
    CHECK(PrintStartCollectorTestAccess::get_predicted_total(collector()) ==
          Catch::Approx(kFirstLayerS).margin(10));
    CHECK(remaining() >= 6 * 60);
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: a start that only heats saves its length and heating rate",
                 "[print][collector][preprint][silent_voron][thermal_rate]") {
    // Already homed, no narration: the bed heat is the only phase seen.
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(300, 900, 320, 0);
    collector().start();
    collector().enable_fallbacks();
    settle();
    for (int t = 5; t <= 300; t += 5) {
        clock_.advance(std::chrono::seconds(5));
        set_all_temps(300 + t * 2, 900, 320, 0); // 5 s/C, at 90C by 300s
        tick_fallbacks();
        PrintStartCollectorTestAccess::run_eta_update(collector());
        settle();
    }
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    collector().complete_from_external_signal("first layer");
    settle();

    const auto entries = helix::PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].total_seconds == 300);
    CHECK(entries[0].phase_durations.empty());
    helix::Config* cfg = helix::Config::get_instance();
    CHECK(cfg->get<float>("/thermal/rates/heater_bed/heat_rate", 0.0f) ==
          Catch::Approx(5.0f).margin(0.5f));
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: the history entry records the phases its start skipped",
                 "[print][collector][preprint][silent_voron]") {
    const int mesh = static_cast<int>(PrintStartPhase::BED_MESH);
    const auto run = [&](const std::optional<helix::PreprintPhaseOverrides>& told) {
        set_all_temps(300, 900, 320, 0);
        collector().start();
        collector().enable_fallbacks();
        if (told) {
            collector().note_phase_overrides(*told);
        }
        settle();
        clock_.advance(std::chrono::seconds(60));
        tick_fallbacks();
        settle();
        collector().complete_from_external_signal("first layer");
        settle();
        collector().stop();
        settle();
    };

    run(helix::PreprintPhaseOverrides{{mesh, false},
                                      {static_cast<int>(PrintStartPhase::QGL), true}});
    run(std::nullopt); // a start nothing told: the last run's list does not carry over

    const auto entries = helix::PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].skipped_phases.has_value());
    CHECK(*entries[0].skipped_phases == std::set<int>{mesh});
    CHECK_FALSE(entries[1].skipped_phases.has_value());
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: a start with hot heaters records zero heating",
                 "[print][collector][preprint][silent_voron]") {
    const auto run = [&](int ext_target) {
        set_all_temps(600, 600, 2200, ext_target);
        collector().start();
        collector().enable_fallbacks();
        settle();
        clock_.advance(std::chrono::seconds(60));
        tick_fallbacks();
        settle();
        collector().complete_from_external_signal("first layer");
        settle();
        collector().stop();
        settle();
    };

    run(2200); // both heaters already at target: priced, and the price is 0
    run(0);    // no nozzle target: the heating was never priced

    const auto entries = helix::PreprintPredictor::load_entries_from_config();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].heating_seconds.has_value());
    CHECK(*entries[0].heating_seconds == 0);
    CHECK_FALSE(entries[1].heating_seconds.has_value());
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: an already-homed printer does not show Homing",
                 "[print][collector][preprint][silent_voron][homing]") {
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(300, 0, 320, 0);
    collector().start();
    collector().enable_fallbacks();
    settle();
    set_homed("xyz");
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
}

TEST_CASE_METHOD(
    SilentVoronReplayFixture,
    "PrintStartCollector: a start that cleans before it levels keeps the leveling time",
    "[print][collector][preprint][silent_voron][eta]") {
    // K1, K2, QIDI and AD5M macros clean the nozzle before they level, the
    // reverse of the phase enum. This printer's history has both steps.
    helix::PreprintEntry past;
    past.total_seconds = 180;
    past.timestamp = 1;
    past.phase_durations = {{static_cast<int>(PrintStartPhase::CLEANING), 30},
                            {static_cast<int>(PrintStartPhase::Z_TILT), 120}};
    past.temp_bucket = 2;
    past.window = helix::PreprintWindow::PrinterEdge;
    helix::PreprintPredictor::append_to_config(past);

    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(600, 600, 2000, 2000);
    collector().start();
    collector().enable_fallbacks();
    settle();
    const auto progress = [this] {
        return lv_subject_get_int(state().print_state().get_print_start_progress_subject());
    };
    const auto tick = [this] {
        clock_.advance(std::chrono::seconds(5));
        PrintStartCollectorTestAccess::run_eta_update(collector());
        tick_fallbacks();
    };

    send_gcode_response("CLEAN_NOZZLE");
    for (int t = 5; t <= 25; t += 5) {
        tick();
    }
    const int cleaning_remaining = remaining();
    const int cleaning_progress = progress();

    clock_.advance(std::chrono::seconds(5));
    send_gcode_response("Z_TILT_ADJUST");
    PrintStartCollectorTestAccess::run_eta_update(collector());
    settle();
    CHECK(remaining() <= cleaning_remaining);

    for (int t = 35; t <= 90; t += 5) {
        tick();
    }
    CHECK(progress() > cleaning_progress);
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: extrusion before the nozzle is hot is not the purge",
                 "[print][collector][preprint][silent_voron][purge]") {
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(900, 900, 1500, 2700);
    collector().start();
    collector().enable_fallbacks();
    settle();
    tick_fallbacks();

    // Extrusion while the nozzle climbs.
    client().dispatch_status_update(
        {{"motion_report", {{"live_extruder_velocity", 2.5}, {"live_velocity", 50.0}}}});
    settle();
    CHECK(get_current_phase() != PrintStartPhase::PURGING);

    // The same extrusion once both heaters are at target is the prime line,
    // read from the heater frames without waiting for a tick.
    client().dispatch_status_update({{"extruder", {{"temperature", 269.5}, {"target", 270.0}}}});
    client().dispatch_status_update(
        {{"motion_report", {{"live_extruder_velocity", 2.5}, {"live_velocity", 50.0}}}});
    settle();
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: a target shown on the display leaves homing inference on",
                 "[print][collector][preprint][silent_voron][homing]") {
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "");
    set_all_temps(300, 0, 320, 0);
    collector().start();
    collector().enable_fallbacks();
    settle();

    display("Bed: 90c");
    REQUIRE(get_current_phase() == PrintStartPhase::HEATING_BED);
    set_homed("xy");
    CHECK(get_current_phase() == PrintStartPhase::HOMING);
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: a bed slowing on its approach does not bounce the countdown",
                 "[print][collector][preprint][silent_voron][eta]") {
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(300, 900, 320, 0);
    collector().start();
    collector().enable_fallbacks();
    settle();

    // A first-order approach to 90C: the measured rate keeps slowing.
    int last = -1;
    int increases = 0;
    for (int t = 5; t <= 400; t += 5) {
        clock_.advance(std::chrono::seconds(5));
        const double bed = 90.0 - 60.0 * std::exp(-t / 150.0);
        set_all_temps(static_cast<int>(std::lround(bed * 10)), 900, 320, 0);
        tick_fallbacks();
        PrintStartCollectorTestAccess::run_eta_update(collector());
        settle();
        // The first measured rate may raise the estimate once.
        if (last >= 0 && remaining() > last) {
            ++increases;
        }
        last = remaining();
    }
    CHECK(increases <= 1);
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: an early purge does not credit leveling still to come",
                 "[print][collector][preprint][silent_voron][eta]") {
    // A purge line at print temperature, then a silent gantry level and a
    // mesh: this printer's history has all three.
    helix::PreprintEntry past;
    past.total_seconds = 200;
    past.timestamp = 1;
    past.phase_durations = {{static_cast<int>(PrintStartPhase::Z_TILT), 60},
                            {static_cast<int>(PrintStartPhase::BED_MESH), 90},
                            {static_cast<int>(PrintStartPhase::PURGING), 10}};
    past.temp_bucket = 2;
    past.window = helix::PreprintWindow::PrinterEdge;
    helix::PreprintPredictor::append_to_config(past);

    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(600, 600, 2000, 2000);
    collector().start();
    collector().enable_fallbacks();
    settle();
    const auto tick = [this] {
        clock_.advance(std::chrono::seconds(5));
        PrintStartCollectorTestAccess::run_eta_update(collector());
        tick_fallbacks();
    };

    client().dispatch_status_update(
        {{"motion_report", {{"live_extruder_velocity", 2.5}, {"live_velocity", 50.0}}}});
    settle();
    REQUIRE(get_current_phase() == PrintStartPhase::PURGING);
    client().dispatch_status_update({{"motion_report", {{"live_extruder_velocity", 0.0}}}});

    // The leveling runs silent for a minute after the purge.
    for (int t = 5; t <= 60; t += 5) {
        tick();
    }
    const int leveling_remaining = remaining();
    CHECK(leveling_remaining > 0);

    send_gcode_response("Z_TILT_ADJUST");
    tick();
    CHECK(remaining() <= leveling_remaining);
}

TEST_CASE_METHOD(SilentVoronReplayFixture,
                 "PrintStartCollector: a bed target the start-up seed missed still gates the purge",
                 "[print][collector][preprint][silent_voron][purge]") {
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
    set_all_temps(300, 0, 2700, 2700);
    collector().start();
    collector().enable_fallbacks();
    settle();

    // M140's target frame came before start(); its subject update lands
    // after the seed, and Klipper does not send the target again.
    set_all_temps(300, 900, 2700, 2700);
    tick_fallbacks();
    client().dispatch_status_update(
        {{"motion_report", {{"live_extruder_velocity", 2.5}, {"live_velocity", 50.0}}}});
    settle();
    CHECK(get_current_phase() != PrintStartPhase::PURGING);
}

// ============================================================================
// Gantry leveling / Z tilt from configured probe points
//
// Klipper echoes nothing a START_PRINT macro runs, so QUAD_GANTRY_LEVEL and
// Z_TILT_ADJUST never reach the console by name. Their "probe at X,Y" lines
// and the toolhead position do, at the XY points [quad_gantry_level] /
// [z_tilt] configure. tests/fixtures/voron24_qgl_mesh_console.txt is a Voron
// 2.4's console from Moonraker's gcode_store ("HH:MM:SS line", continuation
// lines untimed): three QGL passes over four corners, then a 7x7 mesh offset
// 25mm in Y by the probe. The replay carries no display text at all.
// ============================================================================

namespace {

class LevelingPointsFixture : public PrintStartCollectorHeaterFixture {
  public:
    struct Line {
        int s;
        std::string text;
    };

    void configure(const char* settings) {
        helix::PrinterDiscovery hw;
        hw.parse_leveling_probe_points(nlohmann::json::parse(settings));
        REQUIRE_FALSE(hw.leveling_probe_points().empty());
        state().set_hardware(hw);
        drain_async_updates();
    }

    void configure_voron24() {
        configure(
            R"({"quad_gantry_level":{"points":[[25.0,25.0],[25.0,275.0],[275.0,275.0],[275.0,25.0]]}})");
    }

    static std::vector<Line> load_capture() {
        const std::string path = collector_fixture_dir() + "voron24_qgl_mesh_console.txt";
        std::ifstream f(path);
        INFO("fixture: " << path);
        REQUIRE(f.is_open());
        std::vector<Line> lines;
        for (std::string raw; std::getline(f, raw);) {
            int h = 0, m = 0, s = 0;
            if (std::sscanf(raw.c_str(), "%d:%d:%d ", &h, &m, &s) == 3 && raw.size() > 9 &&
                raw[2] == ':') {
                lines.push_back({h * 3600 + m * 60 + s, raw.substr(9)});
            } else {
                REQUIRE_FALSE(lines.empty());
                lines.back().text += "\n" + raw;
            }
        }
        return lines;
    }

    /// Replays lines at their real spacing; `at` runs after each line.
    template <typename Fn> void replay(const std::vector<Line>& lines, Fn at) {
        int now = lines.front().s;
        for (const Line& line : lines) {
            clock_.advance(std::chrono::seconds(line.s - now));
            now = line.s;
            send_gcode_response(line.text);
            at(line);
        }
    }

    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};

constexpr int at_time(int h, int m, int s) {
    return h * 3600 + m * 60 + s;
}

} // namespace

TEST_CASE_METHOD(LevelingPointsFixture,
                 "Leveling points: a silent Voron 2.4 QGL shows from its first corner probe, and "
                 "the mesh after it counts only mesh points",
                 "[print][collector][voron][leveling]") {
    configure_voron24();
    collector().start();
    drain_async_updates();

    const auto lines = load_capture();
    REQUIRE(lines.size() > 200);
    bool saw_first_probe = false;
    bool saw_qgl_passes_end = false;
    replay(lines, [&](const Line& line) {
        if (line.s == at_time(20, 1, 54) && !saw_first_probe) {
            saw_first_probe = true;
            CHECK(get_current_phase() == PrintStartPhase::QGL);
            CHECK(get_current_message() == "Leveling Gantry...");
        }
        if (line.s < at_time(20, 1, 54)) {
            CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
        }
        if (line.s == at_time(20, 2, 49) && line.text.find("Retries: 2/5") != std::string::npos) {
            saw_qgl_passes_end = true;
            CHECK(get_current_phase() == PrintStartPhase::QGL);
        }
    });
    REQUIRE(saw_first_probe);
    REQUIRE(saw_qgl_passes_end);

    // 49 mesh points: the 4 corners probed by QGL are not among them.
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 49);
    CHECK(get_current_message().rfind("Bed Mesh (49", 0) == 0);
}

TEST_CASE_METHOD(LevelingPointsFixture,
                 "Leveling points: without configured points, a named QGL keeps every probe "
                 "line out of the mesh",
                 "[print][collector][voron][leveling]") {
    // No discovery points: the blanket guard is all there is. With QGL named
    // by the console, every probe line is skipped until the mesh is entered
    // some other way, so the mesh is never auto-entered from probe lines.
    collector().start();
    drain_async_updates();
    send_gcode_response("QUAD_GANTRY_LEVEL");
    REQUIRE(get_current_phase() == PrintStartPhase::QGL);
    replay(load_capture(), [](const Line&) {});
    CHECK(get_current_phase() == PrintStartPhase::QGL);
}

TEST_CASE_METHOD(LevelingPointsFixture,
                 "Leveling points: a three-point Z tilt shows Z_TILT, then a 5x5 mesh counts 25",
                 "[print][collector][voron][leveling]") {
    configure(R"({"z_tilt":{"points":[[-50.0,18.0],[150.0,348.0],[350.0,18.0]]}})");
    collector().start();
    drain_async_updates();

    // Z_TILT_ADJUST's console at the 2.4 capture's spacing: three samples
    // a second apart per point, three seconds between points, two passes.
    std::vector<Line> lines;
    int t = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& [x, y] : {std::pair{-50.0, 18.0}, {150.0, 348.0}, {350.0, 18.0}}) {
            for (int sample = 0; sample < 3; ++sample) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=%.6f", x, y,
                              0.1 * sample);
                lines.push_back({t, buf});
                t += 1;
            }
            t += 2;
        }
        lines.push_back({t, "// Making the following Z adjustments:\n// stepper_z = 0.011224"});
        lines.push_back({t, "// Retries: " + std::to_string(pass) +
                                "/5 Probed points range: "
                                "0.010000 tolerance: 0.010000"});
        t += 3;
    }
    t += 8; // the 2.4 capture's gap from the last pass to the mesh
    lines.push_back({t, "// Adapted probe count: (5,5)"});
    t += 5;
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 5; ++col) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "// probe at %.3f,%.3f is z=-0.5", 30.0 + 60 * col,
                          10.0 + 60 * row);
            lines.push_back({t, buf});
            t += 4;
        }
    }

    bool first = true;
    replay(lines, [&](const Line&) {
        if (first) {
            first = false;
            CHECK(get_current_phase() == PrintStartPhase::Z_TILT);
            CHECK(get_current_message() == "Z Tilt Adjust...");
        }
    });
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
    CHECK(PrintStartCollectorTestAccess::get_mesh_probe_current(collector()) == 25);
    CHECK(get_current_message().rfind("Bed Mesh (25", 0) == 0);
}

TEST_CASE_METHOD(LevelingPointsFixture,
                 "Leveling points: toolhead descents at two corners show QGL with no console",
                 "[print][collector][voron][leveling]") {
    // Two geometries: a 350 at 10mm travel probing to -1.1, and a 250 at
    // 5mm travel probing to +0.4.
    double a_x = 25, a_y = 25, b_x = 25, b_y = 275, travel = 10, trigger = -1.1, lift = 2;
    SECTION("350mm bed") {
        configure_voron24();
    }
    SECTION("250mm bed") {
        configure(R"({"quad_gantry_level":{"points":[[30,15],[30,205],[220,205],[220,15]]}})");
        a_x = 220, a_y = 205, b_x = 220, b_y = 15, travel = 5, trigger = 0.4, lift = 1.5;
    }
    collector().start();
    drain_async_updates();

    // The QGL tour as toolhead.position reports it: travel at 10mm, probe
    // down to the trigger, retract; about a second per touch.
    const auto touch = [&](double x, double y) {
        collector().note_position_sample(x, y, travel);
        drain_async_updates();
        clock_.advance(std::chrono::seconds(1));
        collector().note_position_sample(x, y, trigger);
        drain_async_updates();
        clock_.advance(std::chrono::seconds(1));
        collector().note_position_sample(x, y, lift);
        drain_async_updates();
        clock_.advance(std::chrono::seconds(2));
    };
    touch(a_x, a_y);
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
    touch(b_x, b_y);
    CHECK(get_current_phase() == PrintStartPhase::QGL);
    CHECK(get_current_message() == "Leveling Gantry...");
}

TEST_CASE_METHOD(LevelingPointsFixture,
                 "Leveling points: parking at one leveling corner is not leveling",
                 "[print][collector][voron][leveling]") {
    configure_voron24();
    collector().start();
    drain_async_updates();

    for (int i = 0; i < 5; ++i) {
        collector().note_position_sample(275, 275, 20.0f);
        drain_async_updates();
        clock_.advance(std::chrono::seconds(2));
        collector().note_position_sample(275, 275, 5.0f);
        drain_async_updates();
        clock_.advance(std::chrono::seconds(2));
    }
    CHECK(get_current_phase() == PrintStartPhase::INITIALIZING);
}

// ============================================================================
// Stationary purges (blob/poop, waste chute) from motion alone
//
// The head holds still and the extruder runs forward without a break: Z climbs
// for a blob, stays put over a chute. Samples reach the collector as the app
// delivers them: the motion_report frame on the WebSocket path and the
// position/velocity subjects on the main thread, where the collector's
// observers and its 5s ETA tick sample them.
// ============================================================================

namespace {

class PurgeShapeFixture : public PrintStartCollectorHeaterFixture {
  public:
    void start_hot() {
        set_all_temps(1000, 1000, 2600, 2600);
        collector().start();
        drain_async_updates();
        client().dispatch_status_update(
            {{"heater_bed", {{"temperature", 100.0}, {"target", 100.0}}},
             {"extruder", {{"temperature", 260.0}, {"target", 260.0}}}});
        drain_async_updates();
        send_gcode_response("BED_MESH_CALIBRATE");
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    }

    void head(double x, double y, double z) {
        auto& m = state().motion_state();
        lv_subject_set_int(m.get_position_x_subject(), static_cast<int>(std::lround(x * 100)));
        lv_subject_set_int(m.get_position_y_subject(), static_cast<int>(std::lround(y * 100)));
        lv_subject_set_int(m.get_position_z_subject(), static_cast<int>(std::lround(z * 100)));
        collector().check_purge_shape(); // the position observers
        drain_async_updates();
    }

    /// Klipper reports both speeds in one motion_report; `head_mm_s` is the toolhead's.
    void extruder(double mm_s, double head_mm_s = 0.0) {
        client().dispatch_status_update(
            {{"motion_report", {{"live_extruder_velocity", mm_s}, {"live_velocity", head_mm_s}}}});
        lv_subject_set_int(state().motion_state().get_live_extruder_velocity_subject(),
                           static_cast<int>(mm_s * 100));
        collector().check_purge_shape(); // the extruder-velocity observer
        drain_async_updates();
    }

    void wait(int ms) {
        clock_.advance(std::chrono::milliseconds(ms));
    }

    void eta_tick() {
        wait(5000);
        collector().check_purge_shape();
        drain_async_updates();
    }

    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};

} // namespace

TEST_CASE_METHOD(PurgeShapeFixture, "Purge shape: a blob purge with no console shows PURGING",
                 "[print][collector][voron][purge]") {
    // This Voron's AFC purge spot at 290,293 rising from 0.5mm, and a
    // front-left bucket rising from 2mm.
    double x = 290, y = 293, z0 = 0.5;
    SECTION("rear-right, low start") {}
    SECTION("front-left, higher start") {
        x = 4, y = 10, z0 = 2.0;
    }
    start_hot();
    head(x, y, z0);
    extruder(3.0);
    for (int i = 1; i <= 8; ++i) {
        wait(500);
        head(x, y, z0 + 0.2 * i);
    }
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
    CHECK(get_current_message() == "Purging...");
}

TEST_CASE_METHOD(PurgeShapeFixture, "Purge shape: continuous flow over a chute shows PURGING",
                 "[print][collector][voron][purge]") {
    double x = 150, y = 355, z = 10;
    SECTION("rear chute") {}
    SECTION("left chute, other height") {
        x = -8, y = 120, z = 3;
    }
    start_hot();
    head(x, y, z);
    extruder(4.0);
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    eta_tick(); // nothing moves, so only the timer samples it
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(PurgeShapeFixture,
                 "Purge shape: load pulses and retracts with the head still are not a purge",
                 "[print][collector][voron][purge]") {
    double x = 290, y = 293, z = 5;
    SECTION("rear-right") {}
    SECTION("front-left") {
        x = 4, y = 10, z = 12;
    }
    start_hot();
    head(x, y, z);
    for (int i = 0; i < 6; ++i) {
        extruder(25.0);
        wait(1500);
        extruder(0.0);
        wait(500);
        extruder(-30.0);
        wait(800);
        extruder(0.0);
        eta_tick();
    }
    CHECK(get_current_phase() == PrintStartPhase::BED_MESH);
}

// ============================================================================
// K2 nozzle clear owns its own purge
//
// With the bed-mesh pre-print option on, a K2 heats the bed to print
// temperature and the nozzle to 140, runs NOZZLE_CLEAR (prtouch_v3), homes Z,
// meshes, and only then heats to print temperature and purges. NOZZLE_CLEAR
// parks at the CFS extrude position and pushes filament to check flow before
// it wipes, so its BOX_GO_TO_EXTRUDE_POS echo and its stationary extrusion
// look exactly like the real purge.
// ============================================================================

namespace {

class K2NozzleClearFixture : public PurgeShapeFixture {
  public:
    K2NozzleClearFixture() {
        auto profile = PrintStartProfile::load("creality_k2");
        REQUIRE(profile != nullptr);
        REQUIRE(profile->name().find("K2") != std::string::npos);
        collector().set_profile(std::move(profile));
    }

    /// Bed at print temperature, nozzle at the mesh's 140.
    void start_mesh_heated() {
        set_all_temps(600, 600, 1400, 1400);
        collector().start();
        drain_async_updates();
        collector().enable_fallbacks();
        client().dispatch_status_update(
            {{"heater_bed", {{"temperature", 60.0}, {"target", 60.0}}},
             {"extruder", {{"temperature", 140.0}, {"target", 140.0}}}});
        drain_async_updates();
    }

    /// The clear's flow check over the chute: the head holds still while the
    /// extruder pushes forward for longer than a purge needs to show.
    void flow_check_at_chute() {
        head(133, 378, 10);
        extruder(4.0);
        eta_tick();
        extruder(0.0);
    }
};

} // namespace

TEST_CASE_METHOD(K2NozzleClearFixture,
                 "K2: the nozzle clear's purge position and flow check are not the purge",
                 "[print][collector][k2][purge]") {
    start_mesh_heated();

    send_gcode_response("// [NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);

    SECTION("echoed park at the extrude position") {
        send_gcode_response("// [GCODE]BOX_GO_TO_EXTRUDE_POS");
    }
    SECTION("silent flow check over the chute") {
        flow_check_at_chute();
    }
    SECTION("box extrude progress") {
        send_gcode_response("// num: 0, velocity: 575.000000, percent 1.000000");
    }
    SECTION("the clear heats for its wipe in between") {
        send_gcode_response("// [GCODE]TEMPERATURE_WAIT SENSOR=extruder MINIMUM=175 MAXIMUM=185");
        send_gcode_response("// [GCODE]BOX_GO_TO_EXTRUDE_POS");
        flow_check_at_chute();
    }
    CHECK(get_current_phase() != PrintStartPhase::PURGING);
    send_gcode_response("// [NOZZLE_CLEAR] FINISH NOZZLE_CLEAR");

    // NEXT_HOMEZ_NACCU + G28 Z, then the mesh.
    send_gcode_response("// [DEBUG]_handle_home_rails_begin");
    REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    send_gcode_response("// exist_points[81], config_points[81]");
    REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);

    // START_PRINT: print temperatures, then the real purge position.
    send_gcode_response("// [GCODE]BOX_GO_TO_EXTRUDE_POS");
    CHECK(get_current_phase() == PrintStartPhase::PURGING);

    // The checklist reads clean, homing, mesh, then the purge running now.
    const auto in = collector().checklist_input();
    CHECK(in.current == PrintStartPhase::PURGING);
    const auto& done = in.done_in_order;
    const auto at = [&](PrintStartPhase p) { return std::find(done.begin(), done.end(), p); };
    CHECK(at(PrintStartPhase::PURGING) == done.end());
    REQUIRE(at(PrintStartPhase::CLEANING) != done.end());
    REQUIRE(at(PrintStartPhase::HOMING) != done.end());
    REQUIRE(at(PrintStartPhase::BED_MESH) != done.end());
    CHECK(at(PrintStartPhase::CLEANING) < at(PrintStartPhase::HOMING));
    CHECK(at(PrintStartPhase::HOMING) < at(PrintStartPhase::BED_MESH));
}

TEST_CASE_METHOD(K2NozzleClearFixture, "K2: the next step ends the clear's hold on purge",
                 "[print][collector][k2][purge]") {
    start_mesh_heated();
    send_gcode_response("// [NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0");
    SECTION("homing, with no finish line") {
        // START_PRINT without the mesh option: G28 Z, then the purge position.
        send_gcode_response("// [DEBUG]_handle_home_rails_begin");
        REQUIRE(get_current_phase() == PrintStartPhase::HOMING);
    }
    SECTION("a mesh entered from its probe lines alone") {
        for (int i = 0; i < 6; ++i) {
            char line[128];
            std::snprintf(line, sizeof(line), "// probe at %.3f,50.000 is z=-0.031000",
                          20.0 + i * 30.0);
            send_gcode_response(line);
        }
        drain_async_updates();
        REQUIRE(get_current_phase() == PrintStartPhase::BED_MESH);
    }
    flow_check_at_chute();
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(K2NozzleClearFixture, "K2: a clear left running does not outlive its run",
                 "[print][collector][k2][purge]") {
    start_mesh_heated();
    send_gcode_response("// [NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    SECTION("reset") {
        collector().reset();
        drain_async_updates();
    }
    SECTION("a new start") {
        collector().stop();
        start_mesh_heated();
    }
    REQUIRE(get_current_phase() != PrintStartPhase::CLEANING);
    flow_check_at_chute();
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(K2NozzleClearFixture, "K2: the clear's flow check is not logged as a purge",
                 "[print][collector][k2][purge]") {
    // Debug bundles are how a K2 checklist report gets triaged, so a purge the
    // display absorbs must not reach the log either.
    start_mesh_heated();
    send_gcode_response("// [NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0");
    {
        helix::TextLogCapture log;
        flow_check_at_chute();
        CHECK_FALSE(log.contains("stationary purge"));
    }
    send_gcode_response("// [DEBUG]_handle_home_rails_begin");
    helix::TextLogCapture log;
    flow_check_at_chute();
    CHECK(log.contains("stationary purge"));
}

TEST_CASE_METHOD(K2NozzleClearFixture,
                 "K2: the box nozzle clean owns nothing, so the purge after it shows",
                 "[print][collector][k2][purge]") {
    start_mesh_heated();
    send_gcode_response("// [GCODE]BOX_NOZZLE_CLEAN");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    send_gcode_response("// [GCODE]BOX_GO_TO_EXTRUDE_POS");
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

TEST_CASE_METHOD(PurgeShapeFixture,
                 "Purge shape: a blob purge right after a brush wipe still shows PURGING",
                 "[print][collector][purge]") {
    // The default profile declares no owning step, so a clean followed by a
    // stationary purge reads as the purge it is.
    start_hot();
    send_gcode_response("CLEAN_NOZZLE");
    REQUIRE(get_current_phase() == PrintStartPhase::CLEANING);
    head(4, 10, 2.0);
    extruder(3.0);
    for (int i = 1; i <= 8; ++i) {
        wait(500);
        head(4, 10, 2.0 + 0.2 * i);
    }
    CHECK(get_current_phase() == PrintStartPhase::PURGING);
}

// ============================================================================
// checklist_input
// ============================================================================

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture, "checklist_input orders done phases by entry",
                 "[print][collector][checklist_input]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    collector().start();
    collector().enable_fallbacks();

    send_gcode_response("G28");
    clock.advance(std::chrono::seconds(12));
    send_gcode_response("QUAD_GANTRY_LEVEL");
    clock.advance(std::chrono::seconds(30));
    send_gcode_response("BED_MESH_CALIBRATE");

    auto in = collector().checklist_input();
    REQUIRE(in.done_in_order.size() == 2);
    CHECK(in.done_in_order[0] == PrintStartPhase::HOMING);
    CHECK(in.done_in_order[1] == PrintStartPhase::QGL);
    CHECK(in.current == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "checklist_input of a new print lists none of the last print's phases",
                 "[print][collector][checklist_input]") {
    collector().start();
    collector().enable_fallbacks();
    send_gcode_response("G28");
    send_gcode_response("QUAD_GANTRY_LEVEL");
    send_gcode_response("BED_MESH_CALIBRATE");
    REQUIRE(collector().checklist_input().done_in_order.size() == 2);
    collector().stop();

    collector().start();

    CHECK(collector().checklist_input().done_in_order.empty());
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "checklist_input actual seconds match time spent",
                 "[print][collector][checklist_input]") {
    helix::sim::SimulatedClock::ManualScope clock(helix::sim::SimSpeed::of(1.0));
    collector().start();
    collector().enable_fallbacks();

    send_gcode_response("G28");
    clock.advance(std::chrono::seconds(12));
    send_gcode_response("QUAD_GANTRY_LEVEL");
    clock.advance(std::chrono::seconds(30));

    auto in = collector().checklist_input();
    REQUIRE(in.actual_seconds.count(static_cast<int>(PrintStartPhase::HOMING)) == 1);
    CHECK(in.actual_seconds.at(static_cast<int>(PrintStartPhase::HOMING)) == 12);
    CHECK(in.current_elapsed_seconds == 30);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "checklist_input takes expected order from the start block",
                 "[print][collector][checklist_input]") {
    collector().start();
    PrintStartCollectorTestAccess::set_start_block_steps(
        collector(), {{100, helix::StartBlockStepKind::HomeAxes, true, "G28"},
                      {200, helix::StartBlockStepKind::TiltZ, true, "Z_TILT_ADJUST"},
                      {300, helix::StartBlockStepKind::BedMesh, true, "BED_MESH_CALIBRATE"}});
    // The macro analysis is only the fallback.
    helix::PrintStartAnalysis analysis;
    analysis.operations.push_back({"QUAD_GANTRY_LEVEL", helix::OperationCategory::QGL});
    collector().set_macro_analysis(analysis);

    auto in = collector().checklist_input();
    REQUIRE(in.expected_order.size() == 3);
    CHECK(in.expected_order[0] == PrintStartPhase::HOMING);
    CHECK(in.expected_order[1] == PrintStartPhase::Z_TILT);
    CHECK(in.expected_order[2] == PrintStartPhase::BED_MESH);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "checklist_input falls back to the macro analysis, in macro order",
                 "[print][collector][checklist_input]") {
    helix::PrintStartAnalysis analysis;
    analysis.operations.push_back({"G28", helix::OperationCategory::HOMING});
    analysis.operations.push_back({"PRINT_START", helix::OperationCategory::START_PRINT});
    analysis.operations.push_back({"QUAD_GANTRY_LEVEL", helix::OperationCategory::QGL});
    analysis.operations.push_back({"G28", helix::OperationCategory::HOMING});
    collector().set_macro_analysis(analysis);
    collector().start();

    auto in = collector().checklist_input();
    REQUIRE(in.expected_order.size() == 2);
    CHECK(in.expected_order[0] == PrintStartPhase::HOMING);
    CHECK(in.expected_order[1] == PrintStartPhase::QGL);
}

TEST_CASE_METHOD(PrintStartCollectorHeaterFixture,
                 "checklist_input with no start block and no analysis has empty expected order",
                 "[print][collector][checklist_input]") {
    collector().start();
    CHECK(collector().checklist_input().expected_order.empty());
}
