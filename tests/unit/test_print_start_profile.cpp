// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_profile.cpp
 * @brief Unit tests for PrintStartProfile JSON-driven pattern matching
 *
 * Tests the profile loading, signal format matching, regex response patterns,
 * and progress calculation. No LVGL or Moonraker required - pure logic tests.
 */

#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/print_start_profile_test_access.h"
#include "print_start_profile.h"

#include <cmath>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Helper to load default profile (works with or without JSON file)
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_default_profile() {
    return PrintStartProfile::load_default();
}

static std::shared_ptr<PrintStartProfile> get_forge_x_profile() {
    return PrintStartProfile::load("forge_x");
}

// ============================================================================
// Default Profile Loading Tests
// ============================================================================

TEST_CASE("PrintStartProfile: default profile loads successfully", "[profile][print]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    SECTION("Profile has a name") {
        REQUIRE_FALSE(profile->name().empty());
        // Could be "Generic" (from JSON) or "Generic (built-in)" (fallback)
        REQUIRE(profile->name().find("Generic") != std::string::npos);
    }

    SECTION("Profile has weighted progress mode") {
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    }

    SECTION("Default profile has no signal formats") {
        REQUIRE_FALSE(profile->has_signal_formats());
    }
}

// ============================================================================
// Forge-X Profile Loading Tests
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x profile loads with signal formats", "[profile][print]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // If forge_x.json is missing, we'll get the default profile
    // Only run forge_x-specific tests if we actually loaded it
    if (profile->name().find("Forge") != std::string::npos) {
        SECTION("Profile has weighted progress mode") {
            // The order of a Forge-X start varies with its settings (KAMP,
            // print_leveling, mesh validation, cleaning), so the ETA engine
            // sizes the phases rather than a fixed sequence.
            REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
        }

        SECTION("Profile has signal formats") {
            REQUIRE(profile->has_signal_formats());
        }

        SECTION("Profile name and description") {
            REQUIRE(profile->name() == "Forge-X Mod");
            REQUIRE_FALSE(profile->description().empty());
        }
    }
}

// ============================================================================
// Default Profile Response Pattern Matching Tests
// (Same cases as test_print_start_collector.cpp to ensure parity)
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns match homing commands", "[profile][print][homing]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("G28", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("G28 X Y Z", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("Homing axes", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("Home All Axes", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    REQUIRE(profile->try_match_pattern("// homing started", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    // Negative cases
    REQUIRE_FALSE(profile->try_match_pattern("G29", result));
    REQUIRE_FALSE(profile->try_match_pattern("M104", result));
}

TEST_CASE("PrintStartProfile: default patterns match heating bed commands",
          "[profile][print][heating]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("M190 S60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("M140 S60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("Heating bed to 60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("Heat Bed", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("BED_TEMP=60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    REQUIRE(profile->try_match_pattern("bed heating", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    // Real Voron V2 macro: M190 S{BED_TEMP}
    REQUIRE(profile->try_match_pattern("M190 S110", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);

    // Negative: setting to 0 (cooling) should not match bed heating
    REQUIRE_FALSE(profile->try_match_pattern("M140 S0", result));

    // M104 S200 matches HEATING_NOZZLE, not HEATING_BED
    REQUIRE(profile->try_match_pattern("M104 S200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
}

TEST_CASE("PrintStartProfile: default patterns match heating nozzle commands",
          "[profile][print][heating]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("M109 S200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("M104 S200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("M104 S150", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("Heating nozzle to 200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("Heating hotend", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("Heating extruder", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    REQUIRE(profile->try_match_pattern("EXTRUDER_TEMP=200", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);

    // Negative: cooling command should not match
    REQUIRE_FALSE(profile->try_match_pattern("M104 S0", result));

    // M190 S60 matches HEATING_BED, not HEATING_NOZZLE
    REQUIRE(profile->try_match_pattern("M190 S60", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
}

TEST_CASE("PrintStartProfile: default patterns match QGL commands", "[profile][print][leveling]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("QUAD_GANTRY_LEVEL", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);

    REQUIRE(profile->try_match_pattern("quad gantry level", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);

    REQUIRE(profile->try_match_pattern("Running QGL", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);

    // Z_TILT_ADJUST matches Z_TILT, not QGL
    REQUIRE(profile->try_match_pattern("Z_TILT_ADJUST", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);
}

TEST_CASE("PrintStartProfile: default patterns match Z_TILT commands",
          "[profile][print][leveling]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("Z_TILT_ADJUST", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);

    REQUIRE(profile->try_match_pattern("z_tilt_adjust", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);

    REQUIRE(profile->try_match_pattern("z tilt adjust", result));
    REQUIRE(result.phase == PrintStartPhase::Z_TILT);

    // QUAD_GANTRY_LEVEL matches QGL, not Z_TILT
    REQUIRE(profile->try_match_pattern("QUAD_GANTRY_LEVEL", result));
    REQUIRE(result.phase == PrintStartPhase::QGL);
}

TEST_CASE("PrintStartProfile: default patterns match bed mesh commands", "[profile][print][mesh]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("BED_MESH_CALIBRATE", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("BED_MESH_PROFILE LOAD=default", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("Loading bed mesh", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("mesh loading", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    REQUIRE(profile->try_match_pattern("BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);

    // Negative
    REQUIRE_FALSE(profile->try_match_pattern("BED_MESH_CLEAR", result));
}

TEST_CASE("PrintStartProfile: default patterns match cleaning commands",
          "[profile][print][cleaning]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("CLEAN_NOZZLE", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("NOZZLE_CLEAN", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("WIPE_NOZZLE", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("nozzle wipe", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    REQUIRE(profile->try_match_pattern("clean nozzle", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);

    // PURGE_LINE matches PURGING, not CLEANING
    REQUIRE(profile->try_match_pattern("PURGE_LINE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);
}

TEST_CASE("PrintStartProfile: default patterns match purging commands",
          "[profile][print][purging]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_pattern("VORON_PURGE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("LINE_PURGE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("PURGE_LINE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("Prime Line", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("PrimeLine", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("Priming extruder", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("KAMP_ADAPTIVE_PURGE", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    REQUIRE(profile->try_match_pattern("purge line done", result));
    REQUIRE(result.phase == PrintStartPhase::PURGING);

    // CLEAN_NOZZLE matches CLEANING, not PURGING
    REQUIRE(profile->try_match_pattern("CLEAN_NOZZLE", result));
    REQUIRE(result.phase == PrintStartPhase::CLEANING);
}

// ============================================================================
// Default Profile Real Voron V2 Macro Test
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns match Voron V2 START_PRINT lines",
          "[profile][print][voron]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    std::vector<TestCase> voron_lines = {
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
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

// ============================================================================
// Default Profile AD5M Macro Test
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns match AD5M START_PRINT lines",
          "[profile][print][ad5m]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    std::vector<TestCase> ad5m_lines = {
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
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

// ============================================================================
// Forge-X Signal Format Matching Tests
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x signal format matching for all 14 states",
          "[profile][print][signal]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // Only run if forge_x loaded
    if (!profile->has_signal_formats()) {
        SKIP("forge_x.json not available, skipping signal format tests");
    }

    PrintStartProfile::MatchResult result;

    struct SignalTest {
        std::string line;
        PrintStartPhase expected_phase;
        int expected_progress;
        const char* description;
    };

    // clang-format off
    std::vector<SignalTest> signals = {
        {"// State: PREPARING...",           PrintStartPhase::INITIALIZING,    3,   "preparing"},
        {"// State: MD5 CHECK",              PrintStartPhase::INITIALIZING,    5,   "md5 check"},
        {"// State: HOMING...",              PrintStartPhase::HOMING,          10,  "homing"},
        {"// State: PREPARE CLEANING...",    PrintStartPhase::CLEANING,        15,  "prepare cleaning"},
        {"// State: HEATING...",             PrintStartPhase::HEATING_BED,     25,  "heating"},
        {"// State: CLEANING START SOON",    PrintStartPhase::CLEANING,        30,  "cleaning start soon"},
        {"// State: CLEANING...",            PrintStartPhase::CLEANING,        38,  "cleaning"},
        {"// State: COOLING DOWN...",        PrintStartPhase::CLEANING,        45,  "cooling down"},
        {"// State: FINISHING CLEANING...",   PrintStartPhase::CLEANING,        55,  "finishing cleaning"},
        {"// State: DONE!",                  PrintStartPhase::CLEANING,        57,  "done"},
        {"// State: KAMP LEVELING...",       PrintStartPhase::BED_MESH,        60,  "kamp leveling"},
        {"// State: WAIT FOR TEMPERATURE...", PrintStartPhase::HEATING_NOZZLE, 82,  "wait for temp"},
        {"// State: KAMP PRIMING...",        PrintStartPhase::PURGING,         90,  "kamp priming"},
        {"// State: PRINTING...",            PrintStartPhase::COMPLETE,         100, "printing"},
    };
    // clang-format on

    for (const auto& tc : signals) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_signal(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
        REQUIRE(result.progress == tc.expected_progress);
    }
}

TEST_CASE("PrintStartProfile: forge_x KAMP LEVELING message says 'Creating bed mesh'",
          "[profile][print][signal]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    if (!profile->has_signal_formats()) {
        SKIP("forge_x.json not available, skipping bed mesh message test");
    }

    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_signal("// State: KAMP LEVELING...", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    REQUIRE(result.message == "Creating bed mesh...");
}

// ============================================================================
// Signal Format Matching with Surrounding Context
// ============================================================================

TEST_CASE("PrintStartProfile: signal matching with surrounding text", "[profile][print][signal]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    if (!profile->has_signal_formats()) {
        SKIP("forge_x.json not available, skipping signal context tests");
    }

    PrintStartProfile::MatchResult result;

    SECTION("Prefix found within longer line") {
        // The prefix "// State: " can appear anywhere in the line
        REQUIRE(profile->try_match_signal("// State: HOMING...", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }

    SECTION("Unrecognized value after prefix does not match") {
        REQUIRE_FALSE(profile->try_match_signal("// State: UNKNOWN_STATE", result));
    }

    SECTION("Empty value after prefix does not match") {
        REQUIRE_FALSE(profile->try_match_signal("// State: ", result));
    }

    SECTION("Line without the prefix does not match") {
        REQUIRE_FALSE(profile->try_match_signal("State: HOMING...", result));
    }
}

// ============================================================================
// Forge-X Response Pattern Matching (Temperature Wait Lines)
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x response patterns match temperature wait lines",
          "[profile][print][pattern]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // Only test if forge_x loaded
    if (profile->name().find("Forge") == std::string::npos) {
        SKIP("forge_x.json not available, skipping response pattern tests");
    }

    PrintStartProfile::MatchResult result;

    SECTION("Bed temperature wait line with capture group") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 60", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
        // $1 should be substituted with "60"
        REQUIRE(result.message.find("60") != std::string::npos);
    }

    SECTION("The extruder wait line names no phase") {
        // The same line announces the heat to print temperature and the
        // cool-down to 120C for probing; the phase object tells them apart.
        REQUIRE_FALSE(profile->try_match_pattern(
            "// Wait extruder temperature to reach 120 C\u00ba", result));
        REQUIRE_FALSE(
            profile->try_match_pattern("// Wait extruder temperature to reach 265", result));
    }

    SECTION("Different temperature values") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 110", result));
        REQUIRE(result.message.find("110") != std::string::npos);
    }

    SECTION("Non-matching lines") {
        REQUIRE_FALSE(profile->try_match_pattern("Wait for temperature", result));
        REQUIRE_FALSE(profile->try_match_pattern("// Set bed temperature to 60", result));
    }
}

// ============================================================================
// Progress Mode Detection
// ============================================================================

TEST_CASE("PrintStartProfile: progress mode detection", "[profile][print]") {
    SECTION("Default profile uses weighted mode") {
        auto profile = get_default_profile();
        REQUIRE(profile != nullptr);
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    }

    SECTION("Artillery M1 profile uses sequential mode") {
        auto profile = PrintStartProfile::load("artillery_m1");
        REQUIRE(profile != nullptr);
        REQUIRE(profile->name() == "Artillery M1 Pro");
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL);
    }
}

// ============================================================================
// Adaptive bed-mesh flag
// ============================================================================

TEST_CASE("PrintStartProfile: adaptive_meshing flag parses correctly", "[profile][print][mesh]") {
    SECTION("Snapmaker U1 declares adaptive_meshing=true") {
        auto profile = PrintStartProfile::load("snapmaker_u1");
        REQUIRE(profile != nullptr);
        REQUIRE(profile->adaptive_meshing() == true);
    }

    SECTION("Default profile does not declare adaptive_meshing") {
        auto profile = PrintStartProfile::load_default();
        REQUIRE(profile != nullptr);
        REQUIRE(profile->adaptive_meshing() == false);
    }

    SECTION("Forge-X profile does not declare adaptive_meshing") {
        auto profile = PrintStartProfile::load("forge_x");
        REQUIRE(profile != nullptr);
        REQUIRE(profile->adaptive_meshing() == false);
    }
}

// ============================================================================
// Missing Profile Fallback
// ============================================================================

TEST_CASE("PrintStartProfile: missing profile falls back to default", "[profile][print]") {
    auto profile = PrintStartProfile::load("nonexistent_profile_xyz");
    REQUIRE(profile != nullptr);

    // Should get the default profile
    REQUIRE(profile->name().find("Generic") != std::string::npos);

    // Should still have working patterns
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_pattern("G28", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);
}

// ============================================================================
// Malformed JSON Handling
// ============================================================================

TEST_CASE("PrintStartProfile: graceful handling of edge cases", "[profile][print]") {
    SECTION("Empty profile name loads default") {
        auto profile = PrintStartProfile::load("");
        REQUIRE(profile != nullptr);
        // Either loads the default or falls back to built-in
    }

    SECTION("Profile with path traversal loads default") {
        auto profile = PrintStartProfile::load("../../../etc/passwd");
        REQUIRE(profile != nullptr);
        // File won't exist, should fall back to default
    }

    SECTION("Default profile loads from default.json") {
        auto profile = PrintStartProfile::load_default();
        REQUIRE(profile != nullptr);
        REQUIRE_FALSE(profile->name().empty());

        PrintStartProfile::MatchResult result;
        REQUIRE(profile->try_match_pattern("G28", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }
}

TEST_CASE("PrintStartProfile: an unparseable default.json yields an empty Generic profile",
          "[profile][print]") {
    // A user copy of default.json outranks the shipped one, so a broken edit is
    // the realistic way to lose the generic profile.
    helix::ConfigDirGuard config_dir("print_start_profile_default");
    std::filesystem::create_directories(config_dir.dir / "print_start_profiles");
    std::ofstream(config_dir.dir / "print_start_profiles" / "default.json")
        << R"({"name": "Generic", "response_patterns": [)";

    helix::LogCapture log;
    auto profile = PrintStartProfile::load_default();
    REQUIRE(profile != nullptr);
    CHECK(log.has_line_with({"error", "No readable default.json"}));
    CHECK(profile->name() == "Generic");
    CHECK(profile->is_default());
    PrintStartProfile::MatchResult result;
    CHECK_FALSE(profile->try_match_pattern("G28", result));
    CHECK_FALSE(profile->has_phase_object());

    // A named profile that is missing falls back to the same empty profile.
    CHECK(PrintStartProfile::load("nonexistent_profile_xyz")->name() == "Generic");
}

// ============================================================================
// Noise Rejection Tests (same as test_print_start_collector.cpp)
// ============================================================================

TEST_CASE("PrintStartProfile: default patterns reject noise lines", "[profile][print][negative]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

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
    };

    for (const auto& line : noise_lines) {
        CAPTURE(line);
        // Should not match any signal format
        REQUIRE_FALSE(profile->try_match_signal(line, result));
        // Should not match any response pattern
        REQUIRE_FALSE(profile->try_match_pattern(line, result));
    }
}

// ============================================================================
// Capture Group Substitution Tests
// ============================================================================

TEST_CASE("PrintStartProfile: capture group substitution in message templates",
          "[profile][print][pattern]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("Forge") == std::string::npos) {
        SKIP("forge_x.json not available");
    }

    PrintStartProfile::MatchResult result;

    SECTION("Single capture group substitution") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 75", result));
        // Template is "Heating bed to $1 C..." -> "Heating bed to 75 C..."
        REQUIRE(result.message.find("75") != std::string::npos);
    }

    SECTION("Capture group with large number") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 110", result));
        REQUIRE(result.message.find("110") != std::string::npos);
    }
}

// ============================================================================
// Creality K1 Profile Tests
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_creality_k1_profile() {
    return PrintStartProfile::load("creality_k1");
}

TEST_CASE("PrintStartProfile: creality_k1 profile loads successfully", "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    SECTION("Profile has correct name") {
        REQUIRE(profile->name() == "Creality K1");
    }

    SECTION("Profile uses weighted progress mode") {
        REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    }

    SECTION("Profile has signal formats for pre-preparation") {
        REQUIRE(profile->has_signal_formats());
    }
}

TEST_CASE("PrintStartProfile: creality_k1 patterns match real K1C gcode responses",
          "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // These are actual gcode responses captured from a K1C print start
    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    // clang-format off
    std::vector<TestCase> k1c_lines = {
        {"// not prepare.",                   PrintStartPhase::INITIALIZING,    "START_PRINT no pre-prep"},
        {"// x_axes: xyz",                    PrintStartPhase::HOMING,          "CX_ROUGH_G28 complete"},
        {"// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.28", PrintStartPhase::CLEANING, "CX_NOZZLE_CLEAR"},
        {"CX_PRINT_LEVELING_CALIBRATION",     PrintStartPhase::BED_MESH,        "leveling calibration"},
        {"// probe at 50.000,50.000 is z=1.23", PrintStartPhase::BED_MESH,     "probe point"},
        {"// can_break_flag = 0",             PrintStartPhase::HEATING_NOZZLE,  "heating wait started"},
        {"// can_break_flag = 3",             PrintStartPhase::PURGING,         "heating done"},
        {"// can_break_flag is 3",            PrintStartPhase::PURGING,         "heating done (alt)"},
    };
    // clang-format on

    for (const auto& tc : k1c_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

TEST_CASE("PrintStartProfile: creality_k1 signal format matches 'print prepared'",
          "[profile][print][k1][signal]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (!profile->has_signal_formats()) {
        SKIP("creality_k1.json not available or no signal formats");
    }

    PrintStartProfile::MatchResult result;

    SECTION("print prepared signal triggers COMPLETE") {
        REQUIRE(profile->try_match_signal("// print prepared", result));
        REQUIRE(result.phase == PrintStartPhase::COMPLETE);
    }

    SECTION("Unrelated messages don't match signal format") {
        REQUIRE_FALSE(profile->try_match_signal("// x_axes: xyz", result));
        REQUIRE_FALSE(profile->try_match_signal("// wait temp end", result));
    }
}

TEST_CASE("PrintStartProfile: creality_k1 rejects noise from K1C responses",
          "[profile][print][k1][negative]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Real K1C noise lines that should NOT match any phase
    std::vector<std::string> noise = {
        "// wait temp end",
        "// wait temp start",
        "// x_park = -104.5",
        "// y_park = 104.5",
        "// Run Current: 0.56A Hold Current: 0.56A",
        "B:56.8 /55.0 T0:175.3 /220.0",
        "File opened:Cube_PLA_25m49s.gcode Size:224837",
        "File selected",
        "Done printing file",
    };

    for (const auto& line : noise) {
        CAPTURE(line);
        REQUIRE_FALSE(profile->try_match_pattern(line, result));
    }
}

TEST_CASE("PrintStartProfile: creality_k1 full print sequence progression",
          "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Walk through the real K1C print sequence and verify phases advance
    std::set<PrintStartPhase> detected;

    auto process = [&](const std::string& line) {
        if (profile->try_match_pattern(line, result)) {
            detected.insert(result.phase);
        }
    };

    process("// not prepare.");
    REQUIRE(detected.count(PrintStartPhase::INITIALIZING) == 1);

    process("// x_axes: xyz");
    REQUIRE(detected.count(PrintStartPhase::HOMING) == 1);

    process("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.28");
    REQUIRE(detected.count(PrintStartPhase::CLEANING) == 1);

    process("CX_PRINT_LEVELING_CALIBRATION");
    REQUIRE(detected.count(PrintStartPhase::BED_MESH) == 1);

    process("// can_break_flag = 0");
    REQUIRE(detected.count(PrintStartPhase::HEATING_NOZZLE) == 1);

    // Repeated temp reports should not add new phases
    process("B:56.8 /55.0 T0:175.3 /220.0");
    REQUIRE(detected.size() == 5);

    process("// can_break_flag = 3");
    REQUIRE(detected.count(PrintStartPhase::PURGING) == 1);
}

// ============================================================================
// Snapmaker U1 Profile Tests
//
// Ground truth captured from a live U1 print on 2026-05-19 by tailing
// /var/log/messages and observing every line that hit
// PrintStartCollector::on_gcode_response. The Snapmaker U1 PRINT_START
// gcode_macro is essentially empty — every preprint phase is driven by
// slicer-injected gcode running on a Klipper fork that emits state
// transitions via SET_ACTION_CODE. These tests codify the response
// strings actually observed so a future profile edit can't silently
// regress phase detection.
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_snapmaker_u1_profile() {
    return PrintStartProfile::load("snapmaker_u1");
}

TEST_CASE("PrintStartProfile: snapmaker_u1 profile loads", "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    REQUIRE(profile->name() == "Snapmaker U1");
    REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::WEIGHTED);
    REQUIRE(profile->has_signal_formats());
}

TEST_CASE("PrintStartProfile: snapmaker_u1 action-code signals route to correct phase",
          "[profile][print][snapmaker][signal]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Each line below was captured VERBATIM from Moonraker's gcode_store on the
    // 2026-06-18 reprint of lid_PLA_6m28s (heads 0+3). The message text is the
    // only surface that explains which pre-print sub-phase is running, so the
    // exact strings matter.
    //
    // Two action-code verb forms appear in the real stream and BOTH must route:
    //   "// Success: Set action code <CODE>"
    //   "// Success: Changed main state to PRINTING with action <CODE>"
    // PRINT_BED_DETECTING arrives ONLY via the second form (evidence:
    // "// Success: Changed main state to PRINTING with action PRINT_BED_DETECTING"
    // at 23:23:26), which the pre-2026-06-18 profile missed entirely.
    struct ActionCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* expected_message;
    };
    std::vector<ActionCase> cases = {
        // --- "Set action code" verb (evidence: gcode_store 23:24:27..23:31:00) ---
        {"// Success: Set action code PRINT_SWITCH_CHECKING", PrintStartPhase::INITIALIZING,
         "Checking extruders..."},
        {"// Success: Set action code PRINT_AUTO_FEEDING", PrintStartPhase::INITIALIZING,
         "Loading filament..."},
        {"// Success: Set action code PRINT_REPLENISHING", PrintStartPhase::INITIALIZING,
         "Replenishing filament..."},
        {"// Success: Set action code DETECT_PLATE", PrintStartPhase::BED_MESH,
         "Detecting plate..."},
        {"// Success: Set action code PRINT_PREEXTRUDING", PrintStartPhase::PURGING, "Priming..."},
        // --- the mesh (evidence: gcode_store 2026-10-16 21:53:52..21:55:26): a
        //     60 s bed soak with a countdown, four corner probes, then the mesh ---
        {"// Success: Set action code BED_PREHEATING", PrintStartPhase::SOAKING, "Heat Soaking..."},
        {"// Success: Set action code BED_PRESCANNING", PrintStartPhase::BED_MESH,
         "Validating bed mesh..."},
        {"// Success: Set action code BED_LEVELING", PrintStartPhase::BED_MESH, "Bed mesh..."},
        // --- "Changed main state to PRINTING with action" verb
        //     (evidence: gcode_store 23:23:26) ---
        {"// Success: Changed main state to PRINTING with action PRINT_BED_DETECTING",
         PrintStartPhase::BED_MESH, "Inspecting bed..."},
        {"// Success: Changed main state to PRINTING with action PRINT_SWITCH_CHECKING",
         PrintStartPhase::INITIALIZING, "Checking extruders..."},
        {"// Success: Changed main state to PRINTING with action DETECT_PLATE",
         PrintStartPhase::BED_MESH, "Detecting plate..."},
        {"// Success: Changed main state to PRINTING with action BED_PREHEATING",
         PrintStartPhase::SOAKING, "Heat Soaking..."},
        {"// Success: Changed main state to PRINTING with action BED_PRESCANNING",
         PrintStartPhase::BED_MESH, "Validating bed mesh..."},
        {"// Success: Changed main state to PRINTING with action BED_LEVELING",
         PrintStartPhase::BED_MESH, "Bed mesh..."},
    };

    for (const auto& c : cases) {
        CAPTURE(c.line);
        REQUIRE(profile->try_match_signal(c.line, result));
        REQUIRE(result.phase == c.expected_phase);
        REQUIRE(result.message == c.expected_message);
    }

    // IDLE action codes intentionally have no mapping — they bracket every real
    // phase ("// Success: Set action code IDLE" / "...with action IDLE") and
    // must NOT steer the UI.
    REQUIRE_FALSE(profile->try_match_signal("// Success: Set action code IDLE", result));
    REQUIRE_FALSE(profile->try_match_signal(
        "// Success: Changed main state to PRINTING with action IDLE", result));

    // Future Snapmaker firmware revisions may emit action codes we don't know
    // about yet; they must fall through cleanly (no false match).
    REQUIRE_FALSE(profile->try_match_signal("// Success: Set action code FOO_UNKNOWN", result));

    // try_match_signal trims trailing whitespace (\r from some firmware variants).
    REQUIRE(profile->try_match_signal("// Success: Set action code DETECT_PLATE\r", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);
}

TEST_CASE("PrintStartProfile: snapmaker_u1 response patterns match real preprint lines",
          "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    PrintStartProfile::MatchResult result;

    // Klipper emits one of these per axis-trigger during G28. Both flavours
    // (single axis, both axes) appeared in the capture. The pattern anchors on
    // the prefix so generic "trigger" strings don't false-match.
    // Evidence (gcode_store 23:23:08 / 23:23:13):
    //   "// trigger_mcu_pos: {'stepper_y': 7794, 'stepper_x': -97442}"
    //   "// trigger_mcu_pos: {'stepper_x': -97378, 'stepper_y': 4660}"
    REQUIRE(profile->try_match_pattern(
        "// trigger_mcu_pos: {'stepper_y': 7794, 'stepper_x': -97442}", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);
    REQUIRE(result.message == "Homing axes...");

    REQUIRE(profile->try_match_pattern(
        "// trigger_mcu_pos: {'stepper_x': -97378, 'stepper_y': 4660}", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);

    // G28 X Y ends with the precise-CoreXY diagonal probe, which prints
    // "// probe_start_x:" under a second 1 s after the trigger_mcu_pos lines
    // (capture 2026-10-16, 21:48:29.469 then 21:48:30.282). trigger_mcu_pos
    // already shows "Homing axes...", so the line maps to nothing.
    REQUIRE_FALSE(
        profile->try_match_pattern("// probe_start_x: 5.41250, probe_start_y: 4.79219", result));

    // "// z offset:" opens every G28 Z: after each tool pick (G28 Z I140 J140,
    // the new tool's coil touching the bed centre at 140,140) and once more
    // before the mesh at (129.915,125.560). Neither is a mesh point: the
    // mesh's first point is (33.742,28.487), after BED_LEVELING (capture
    // 2026-10-16, 21:49:08.965 and 21:53:38.040). The line also follows
    // BED_LEVELING by 1 ms (21:55:26.591); the collector keeps the mesh then.
    REQUIRE(profile->try_match_pattern("// z offset: -0.05", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);
    REQUIRE(result.message == "Probing Z...");

    // "// z_mesh_complete:" ends that same G28 Z (it re-bases the mesh to the
    // active tool); BED_MESH_CALIBRATE never prints it, so it names no phase.
    REQUIRE_FALSE(profile->try_match_pattern("// z_mesh_complete: 0.2980673660155705", result));

    // Negative cases — these must NOT match. FEED_AUTO / FLOW_CALIBRATE /
    // BED_MESH_CALIBRATE / *_CLEAN_NOZZLE are klippy-internal only and never
    // appear in Moonraker's gcode_store, so the profile must not key on them.
    REQUIRE_FALSE(profile->try_match_pattern("G28 X Y", result));
    REQUIRE_FALSE(profile->try_match_pattern("M109 S250", result));
    REQUIRE_FALSE(profile->try_match_pattern("BED_MESH_CALIBRATE", result));
    REQUIRE_FALSE(profile->try_match_pattern("VORON_PURGE", result));
    REQUIRE_FALSE(profile->try_match_pattern("CLEAN_NOZZLE", result));
    REQUIRE_FALSE(profile->try_match_pattern(
        "// [feed] FEED_AUTO MODULE=left CHANNEL=1 LOAD=1 PRINTING=1 EXTRUDER=0", result));
}

TEST_CASE("PrintStartProfile: snapmaker_u1 carries no silent_progression",
          "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    // silent_progression is intentionally EMPTY on the U1. The signal-less
    // clean/mesh/prime stretch is real, but the collector's temps_ready timer
    // fires unreliably on this firmware: the U1 reports state=printing the
    // instant the SD job opens and idles ~90s before any action code, with
    // heater targets set/cleared throughout (ext_target=0 during the early idle
    // gap makes temps_ready trivially true). A temps-ready-based PURGING nudge
    // would announce "Priming..." before the printer has even homed (live U1
    // finding 2026-06-11). The gap is left to the last action-code phase +
    // weighted progress instead. Guards against re-introducing the timer.
    REQUIRE(profile->silent_progression().empty());

    // Adaptive meshing must stay on — the U1 slicer overrides MESH_MIN/MAX so
    // the configfile probe_count (169) hugely overstates the real ~16 probes.
    REQUIRE(profile->adaptive_meshing());
}

// ============================================================================
// Creality K2 Profile Tests
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_creality_k2_profile() {
    return PrintStartProfile::load("creality_k2");
}

/**
 * Every line below is a verbatim gcode_response captured from a K2 Plus
 * pre-print on 2026-08-16 (klippy.log 12:18:50-12:29:02, print
 * quattrobox_bottom_cover_ASA-GF). The profile that shipped before this was
 * written against the macro *names* in gcode_macro.cfg rather than against
 * what the firmware actually echoes, so four of its seven patterns never
 * matched anything on a real print.
 */
TEST_CASE("PrintStartProfile: creality_k2 patterns match real K2 Plus gcode responses",
          "[profile][print][k2]") {
    auto profile = get_creality_k2_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("K2") == std::string::npos) {
        SKIP("creality_k2.json not available");
    }

    PrintStartProfile::MatchResult result;

    struct TestCase {
        std::string line;
        PrintStartPhase expected_phase;
        const char* description;
    };

    // clang-format off
    std::vector<TestCase> k2_lines = {
        {"// [DEBUG]_handle_home_rails_begin",        PrintStartPhase::HOMING,   "G28 at 12:18:50"},
        {"// x_park = 175.0 ",                        PrintStartPhase::HOMING,   "home complete at 12:19:13"},
        {"// send query_z_align cur_retries:0 oid=4", PrintStartPhase::Z_TILT,   "z align at 12:19:15"},
        {"// [NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0", PrintStartPhase::CLEANING, "clean at 12:20:50"},
        {"// [GCODE]BOX_NOZZLE_CLEAN",                PrintStartPhase::CLEANING, "clean at 12:21:02"},
        {"// exist_points[81], config_points[81]",    PrintStartPhase::BED_MESH, "mesh start at 12:22:28"},
        {"// flush_temp: 220",                        PrintStartPhase::PURGING,  "purge at 12:34:39"},
        {"// flush_len: 101.250000",                  PrintStartPhase::PURGING,  "purge at 12:35:24"},
    };
    // clang-format on

    for (const auto& tc : k2_lines) {
        CAPTURE(tc.description, tc.line);
        REQUIRE(profile->try_match_pattern(tc.line, result));
        REQUIRE(result.phase == tc.expected_phase);
    }
}

TEST_CASE("PrintStartProfile: creality_k2 does not claim homing on the post-clean Z recheck",
          "[profile][print][k2][negative]") {
    auto profile = get_creality_k2_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("K2") == std::string::npos) {
        SKIP("creality_k2.json not available");
    }

    // [G28_RE_CHECK] is the Z re-verify that runs AFTER the nozzle clean, 3.5
    // minutes past the real G28. A bare "G28" pattern matched it and nothing
    // earlier, so HOMING was announced in the middle of bed levelling.
    PrintStartProfile::MatchResult result;
    REQUIRE_FALSE(profile->try_match_pattern("// [G28_RE_CHECK]", result));
}

TEST_CASE("PrintStartProfile: creality_k2 keeps the M109 latch that the heater correction needs",
          "[profile][print][k2][heating]") {
    auto profile = get_creality_k2_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("K2") == std::string::npos) {
        SKIP("creality_k2.json not available");
    }

    // BOX_NOZZLE_CLEAN issues M109 S170 then M109 S140 to soften filament for
    // the wipe, so this fires during CLEANING rather than at the real heat.
    // It must stay anyway: it is the ONLY route into a heating phase on this
    // firmware, which emits no M190 and no M109 at print temp. Proactive
    // temperature detection cannot cover the gap — it is gated off the moment
    // any real firmware signal is seen (real_signal_seen_), and the HOMING
    // pattern trips that within a second of the collector starting. Once
    // latched here, the heater correction in check_fallback_completion()
    // re-derives the shown phase from live temps, bed-first, which is what
    // actually puts "Heating Bed" on screen during the soak.
    //
    // Removing this pattern showed "Cleaning Nozzle" for the whole 4-minute
    // ASA bed soak on hardware, then timed out before the mesh (2026-08-16).
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_pattern("// [GCODE]M109 S170", result));
    REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
}

TEST_CASE("PrintStartProfile: creality_k2 uses adaptive meshing", "[profile][print][k2][mesh]") {
    auto profile = get_creality_k2_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("K2") == std::string::npos) {
        SKIP("creality_k2.json not available");
    }

    // The configured grid is 9x9 = 81, but the firmware trims the sweep to the
    // print area: the 2026-08-16 run probed 67 points and skipped five cells of
    // the Y=5 row outright. A fixed 81 denominator can only ever be wrong, so
    // the count renders as "Bed Mesh (N)" with no total.
    REQUIRE(profile->adaptive_meshing());
}

TEST_CASE("PrintStartProfile: creality_k2 rejects noise from K2 responses",
          "[profile][print][k2][negative]") {
    auto profile = get_creality_k2_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("K2") == std::string::npos) {
        SKIP("creality_k2.json not available");
    }

    PrintStartProfile::MatchResult result;
    for (const auto& noise : {
             "// cmd: GET_BOX_STATE",
             "// [DISTUURB_CTL]SET_FANS=0.60 bak_fans=1.00",
             "// z1 Photoelectric switch triggered",
             "// [GCODE]G1 X175.00 Y175.00 Z5.00 F6000.00",
             "// [WHY_DEBUG]get_z_now_comp now_comp:0.017 target_temp:140.00 G28_temp:139.21",
             "// temp_pos:0.29644158299197443",
         }) {
        CAPTURE(noise);
        REQUIRE_FALSE(profile->try_match_pattern(noise, result));
    }
}

// ============================================================================
// Phase-Object Status Source Tests
//
// A profile may declare a status object whose field carries structured phase
// state. The state strings map to phases through state_patterns - the same
// compiled-pattern machinery console lines use - and the object gets
// subscribed so those frames reach the collector.
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x declares its phase object",
          "[profile][print][phase_object]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);
    // Strict on purpose: the JSON ships with the repo, so a missing phase
    // object here is a regression, not an environment gap.
    REQUIRE(profile->name().find("Forge") != std::string::npos);

    REQUIRE(profile->has_phase_object());
    REQUIRE(profile->phase_object_name() == "operation_context");
    REQUIRE(profile->phase_object_field() == "current_state");
    // current_state is the innermost context's state; context_types names
    // which step it belongs to.
    REQUIRE(profile->phase_object_context_field() == "context_types");

    const auto objects = profile->required_status_objects();
    REQUIRE(objects.size() == 1);
    REQUIRE(objects[0] == "operation_context");
}

TEST_CASE("PrintStartProfile: the generic profile reads Klipper's display_status",
          "[profile][print][phase_object]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name() == "Generic");

    // Klipper does not echo commands issued inside a gcode_macro, so a
    // macro-driven PRINT_START never reaches the console stream. What it does
    // reach is display_status.message, which SET_DISPLAY_TEXT and M117 write.
    REQUIRE(profile->has_phase_object());
    REQUIRE(profile->phase_object_name() == "display_status");
    REQUIRE(profile->phase_object_field() == "message");

    // No state_patterns declared: the response patterns serve that feed too.
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_state("Homing...", result));
    CHECK(result.phase == PrintStartPhase::HOMING);
    REQUIRE(profile->try_match_state("Cleaning Nozzle", result));
    CHECK(result.phase == PrintStartPhase::CLEANING);
    REQUIRE(profile->try_match_state("Bed Mesh", result));
    CHECK(result.phase == PrintStartPhase::BED_MESH);

    // Slicer layer chatter shares the field and is not a phase.
    REQUIRE_FALSE(profile->try_match_state("Layer 12/240", result));
}

TEST_CASE("PrintStartProfile: declared state patterns win over response patterns",
          "[profile][print][phase_object]") {
    // The response-pattern fallback applies only to a profile that declares no
    // state patterns of its own: a profile carrying both keeps the two feeds
    // separate, so console-only vocabulary never leaks into the state feed.
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"t7",)"
        R"("response_patterns":[)"
        R"({"pattern":"G28","phase":"HOMING","message":"Homing...","weight":10}],)"
        R"("state_patterns":[)"
        R"({"pattern":"CLEANING","phase":"CLEANING","message":"Cleaning Nozzle...","weight":5}]})"));
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_state("CLEANING", result));
    CHECK(result.phase == PrintStartPhase::CLEANING);
    REQUIRE_FALSE(profile->try_match_state("G28", result));

    // The console feed is untouched by the fallback either way.
    REQUIRE(profile->try_match_pattern("G28", result));
    CHECK(result.phase == PrintStartPhase::HOMING);
}

TEST_CASE("PrintStartProfile: a pattern's hold comes from the capture group it names",
          "[profile][print][pattern][hold]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"hold",)"
        R"("response_patterns":[)"
        R"({"pattern":"Soak: ([0-9.]+)m","phase":"SOAKING","message":"Heat Soaking...",)"
        R"("hold_minutes_group":1},)"
        R"({"pattern":"Wait: ([0-9.]+)m","phase":"SOAKING","hold_minutes_group":2},)"
        R"({"pattern":"G28","phase":"HOMING"}],)"
        R"("state_patterns":[)"
        R"({"pattern":"Dwell ([0-9]+) min","phase":"SOAKING","hold_minutes_group":1}]})"));
    REQUIRE(profile != nullptr);
    PrintStartProfile::MatchResult result;

    SECTION("minutes are read with a decimal point") {
        REQUIRE(profile->try_match_pattern("// Soak: 2.5m", result));
        REQUIRE(result.hold_seconds == 150);
    }

    SECTION("a group the pattern does not have holds nothing") {
        REQUIRE(profile->try_match_pattern("// Wait: 3m", result));
        REQUIRE(result.phase == PrintStartPhase::SOAKING);
        REQUIRE(result.hold_seconds == 0);
    }

    SECTION("a state pattern carries its hold through try_match_state") {
        REQUIRE(profile->try_match_state("Dwell 4 min", result));
        REQUIRE(result.hold_seconds == 240);
    }

    SECTION("a match without a hold clears the last one") {
        REQUIRE(profile->try_match_pattern("// Soak: 2.5m", result));
        REQUIRE(profile->try_match_pattern("G28", result));
        REQUIRE(result.hold_seconds == 0);
    }
}

TEST_CASE("PrintStartProfile: default patterns match macro narration, not just commands",
          "[profile][print][pattern]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name() == "Generic");

    PrintStartProfile::MatchResult result;

    struct Row {
        const char* text;
        PrintStartPhase phase;
    };

    // A community PRINT_START narrates its phases as prose through
    // SET_DISPLAY_TEXT, so the command names the patterns key on never appear.
    const Row rows[] = {
        {"Cleaning Nozzle", PrintStartPhase::CLEANING},
        {"Cleaning nozzle...", PrintStartPhase::CLEANING},
        {"Bed mesh", PrintStartPhase::BED_MESH},
        {"Bed Mesh", PrintStartPhase::BED_MESH},
        {"Heating chamber: 45c", PrintStartPhase::SOAKING},
        {"Waiting for chamber temp", PrintStartPhase::SOAKING},
        {"Bed soak - 2 minutes remaining", PrintStartPhase::SOAKING},
        {"Bed soak finished", PrintStartPhase::SOAKING},
        {"Heat soak", PrintStartPhase::SOAKING},
    };
    for (const auto& row : rows) {
        CAPTURE(row.text);
        REQUIRE(profile->try_match_state(row.text, result));
        CHECK(result.phase == row.phase);
    }

    // Patterns resolve in array order, and the soak entry sits after the
    // bed-heating one: ordinary bed narration keeps its own phase and label.
    REQUIRE(profile->try_match_pattern("Heating Bed: 100c", result));
    CHECK(result.phase == PrintStartPhase::HEATING_BED);
    CHECK(result.message == "Heating Bed...");
    REQUIRE(profile->try_match_pattern("Heat soak", result));
    CHECK(result.phase == PrintStartPhase::SOAKING);
    CHECK(result.message == "Heat Soaking...");

    // A bare "bed mesh" is a mesh signal; the clear command still is not.
    REQUIRE_FALSE(profile->try_match_pattern("BED_MESH_CLEAR", result));
}

TEST_CASE("PrintStartProfile: profiles without phase_object declare no status objects",
          "[profile][print][phase_object]") {
    // Absence must read as absence: no object name, no subscription request.
    // The K2 profile doubles as the regression guard that a printer-specific
    // profile with no phase object stays exactly as it was. (default.json
    // declares both a phase object and status_signals rules, so its objects
    // are pinned separately in the status-signal tests below.)
    const auto k2 = PrintStartProfile::load("creality_k2");
    REQUIRE(k2 != nullptr);
    REQUIRE_FALSE(k2->has_phase_object());
    REQUIRE(k2->phase_object_name().empty());
    REQUIRE(k2->phase_object_field().empty());
    REQUIRE(k2->required_status_objects().empty());
}

TEST_CASE("PrintStartProfile: forge_x state patterns map the mod's state strings",
          "[profile][print][phase_object]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name().find("Forge") != std::string::npos);

    struct StateRow {
        const char* state; ///< "<innermost context type>/<current_state>"
        PrintStartPhase phase;
        const char* message;
    };

    // The whole Forge-X 1.4.2 START_PRINT vocabulary that names a phase.
    // clang-format off
    const StateRow rows[] = {
        {"print/CHECKING FILE",               PrintStartPhase::INITIALIZING,   "Verifying file..."},
        {"print/HOMING",                      PrintStartPhase::HOMING,         "Homing..."},
        {"nozzle_clean/HOMING",               PrintStartPhase::HOMING,         "Homing..."},
        {"nozzle_clean/HEATING",              PrintStartPhase::HEATING_BED,    "Heating..."},
        {"kamp/HEATING",                      PrintStartPhase::HEATING_BED,    "Heating..."},
        {"bed_level/HEATING",                 PrintStartPhase::HEATING_BED,    "Heating..."},
        {"nozzle_clean/HEATING BED",          PrintStartPhase::HEATING_BED,    "Heating Bed..."},
        {"print/HEATING BED",                 PrintStartPhase::HEATING_BED,    "Heating Bed..."},
        {"nozzle_clean/HEATING NOZZLE",       PrintStartPhase::HEATING_NOZZLE, "Heating Nozzle..."},
        {"print/HEATING NOZZLE",              PrintStartPhase::HEATING_NOZZLE, "Heating Nozzle..."},
        {"nozzle_clean/PREPARING TO CLEAN",   PrintStartPhase::CLEANING,       "Preparing to clean..."},
        {"nozzle_clean/CLEANING",             PrintStartPhase::CLEANING,       "Cleaning nozzle..."},
        {"nozzle_clean/COOLING NOZZLE",       PrintStartPhase::CLEANING,       "Cooling for probing..."},
        {"nozzle_clean/FINISHING",            PrintStartPhase::CLEANING,       "Finishing clean..."},
        {"kamp/COOLING NOZZLE",               PrintStartPhase::HEATING_NOZZLE, "Cooling for probing..."},
        {"kamp/LEVELING",                     PrintStartPhase::BED_MESH,       "Creating bed mesh..."},
        {"bed_level/LEVELING",                PrintStartPhase::BED_MESH,       "Creating bed mesh..."},
        {"mesh_validation/CHECKING MESH",     PrintStartPhase::BED_MESH,       "Validating bed mesh..."},
        {"print/PARKING",                     PrintStartPhase::HEATING_NOZZLE, "Preparing..."},
        {"print/RESUMING HEAT",               PrintStartPhase::HEATING_NOZZLE, "Reheating..."},
        {"print/PRIMING",                     PrintStartPhase::PURGING,        "Priming nozzle..."},
        {"print/PRINTING",                    PrintStartPhase::COMPLETE,       "Starting print..."},
    };
    // clang-format on

    for (const auto& row : rows) {
        CAPTURE(row.state);
        PrintStartProfile::MatchResult result;
        REQUIRE(profile->try_match_state(row.state, result));
        CHECK(result.phase == row.phase);
        CHECK(result.message == row.message);
    }

    // States that name no phase of their own. FINISHING ends a nested step
    // mid-pre-print, the print context's LEVELING only decides how to level
    // (the mesh runs later in its own context), and the profile states mean
    // no mesh runs at all. Bare states without a context never match.
    const char* unmapped[] = {
        "bed_level/FINISHING",
        "auto_bed_level/FINISHING",
        "print/LEVELING",
        "print/LOADING PROFILE",
        "print/USING LOADED PROFILE",
        "print/SKIPPING LEVELING",
        "print/COOLING NOZZLE",
        "nozzle_clean/COOLING BED",
        "FINISHING",
        "PRINTING",
        "LEVELING",
        "HOMING",
        "/PRINTING",
        "kamp/PRINTING",
        "nozzle_clean/FINISHING CLEAN",
        "",
    };
    for (const char* state : unmapped) {
        CAPTURE(state);
        PrintStartProfile::MatchResult result;
        REQUIRE_FALSE(profile->try_match_state(state, result));
    }

    // Only the print context's PRINTING completes the pre-print.
    for (const auto& row : rows) {
        CAPTURE(row.state);
        CHECK(
            (row.phase != PrintStartPhase::COMPLETE || std::string(row.state) == "print/PRINTING"));
    }
}

TEST_CASE("PrintStartProfile: forge_x console patterns name only positive steps",
          "[profile][print][pattern]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name().find("Forge") != std::string::npos);

    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_pattern("info Cleaning nozzle with a priming line", result));
    CHECK(result.phase == PrintStartPhase::PURGING);
    REQUIRE(profile->try_match_pattern("// Clean to X: -10, Z: -2.0158333331508258", result));
    CHECK(result.phase == PrintStartPhase::CLEANING);

    // Lines from the 1.4.2 capture that name no step, or name one that has
    // not started yet (KAMP announces itself minutes before its mesh).
    const char* silent[] = {
        "// Initiating KAMP bed leveling",
        "info Started stock nozzle cleaning 265.0/105.0",
        "info Cleaning Z-Offsets:",
        "// Temperature waiting done.",
        "!! Detected bed pressure. Please ensure the bed is clean!",
        "// Starting Smart Park...",
        "// Smart Park location: 110.0,100.0,10.0.",
        "// Mesh Bed Leveling Complete",
        "info Algorithm: LINE_PURGE",
        "HOMING",
    };
    for (const char* line : silent) {
        CAPTURE(line);
        REQUIRE_FALSE(profile->try_match_pattern(line, result));
    }
}

TEST_CASE("PrintStartProfile: a context field qualifies the state the patterns match",
          "[profile][print][phase_object]") {
    using nlohmann::json;
    CHECK(PrintStartProfile::innermost_context(json::array({"print", "kamp", "nozzle_clean"})) ==
          "nozzle_clean");
    CHECK(PrintStartProfile::innermost_context(json::array({"print"})) == "print");
    CHECK(PrintStartProfile::innermost_context(json("kamp")) == "kamp");
    CHECK(PrintStartProfile::innermost_context(json::array()).empty());
    CHECK(PrintStartProfile::innermost_context(json(nullptr)).empty());
    CHECK(PrintStartProfile::innermost_context(json(3)).empty());
    CHECK(PrintStartProfile::innermost_context(json::array({"print", 7})).empty());
    CHECK(PrintStartProfile::qualify_state("kamp", "LEVELING") == "kamp/LEVELING");
    CHECK(PrintStartProfile::qualify_state("", "LEVELING") == "/LEVELING");

    auto profile = PrintStartProfileTestAccess::parse(json::parse(
        R"({"name":"ctx","phase_object":)"
        R"({"object":"operation_context","field":"current_state","context_field":"context_types"}})"));
    REQUIRE(profile != nullptr);
    REQUIRE(profile->has_phase_object());
    CHECK(profile->phase_object_context_field() == "context_types");
    // The context rides in the same object, so nothing more is subscribed.
    CHECK(profile->required_status_objects() == std::vector<std::string>{"operation_context"});
}

TEST_CASE("PrintStartProfile: a phase object without a context field matches bare states",
          "[profile][print][phase_object]") {
    auto plain = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"plain","phase_object":{"object":"operation_context","field":"current_state"}})"));
    REQUIRE(plain != nullptr);
    REQUIRE(plain->has_phase_object());
    CHECK(plain->phase_object_context_field().empty());
    CHECK(get_default_profile()->phase_object_context_field().empty());
}

TEST_CASE("PrintStartProfile: a pattern may say its probes are not mesh points",
          "[profile][print][phase_object]") {
    auto profile = PrintStartProfileTestAccess::parse(
        nlohmann::json::parse(R"({"name":"cp","state_patterns":[)"
                              R"({"pattern":"^check$","phase":"BED_MESH","counts_probes":false},)"
                              R"({"pattern":"^mesh$","phase":"BED_MESH"},)"
                              R"({"pattern":"^odd$","phase":"BED_MESH","counts_probes":"no"}]})"));
    REQUIRE(profile != nullptr);
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_state("check", result));
    CHECK_FALSE(result.counts_probes);
    REQUIRE(profile->try_match_state("mesh", result));
    CHECK(result.counts_probes);
    // A non-boolean keeps the default.
    REQUIRE(profile->try_match_state("odd", result));
    CHECK(result.counts_probes);

    REQUIRE(get_forge_x_profile()->try_match_state("mesh_validation/CHECKING MESH", result));
    CHECK_FALSE(result.counts_probes);
    REQUIRE(get_forge_x_profile()->try_match_state("kamp/LEVELING", result));
    CHECK(result.counts_probes);
}

TEST_CASE("PrintStartProfile: a pattern may own the phases its step emits",
          "[profile][print][owns]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"own","response_patterns":[)"
        R"({"pattern":"^clear$","phase":"CLEANING","owns":["PURGING","purging"]},)"
        R"({"pattern":"^wipe$","phase":"CLEANING"},)"
        R"({"pattern":"^odd$","phase":"CLEANING","owns":"PURGING"},)"
        R"({"pattern":"^bad$","phase":"CLEANING","owns":["NOT_A_PHASE",3,"CLEANING","IDLE"]}]})"));
    REQUIRE(profile != nullptr);
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_pattern("clear", result));
    CHECK(result.owns == std::vector<PrintStartPhase>{PrintStartPhase::PURGING});
    REQUIRE(profile->try_match_pattern("wipe", result));
    CHECK(result.owns.empty());
    // Not an array: ignored whole.
    REQUIRE(profile->try_match_pattern("odd", result));
    CHECK(result.owns.empty());
    // Unknown names, non-strings, the step's own phase and IDLE own nothing.
    REQUIRE(profile->try_match_pattern("bad", result));
    CHECK(result.owns.empty());
}

TEST_CASE("PrintStartProfile: a malformed context field drops the phase object",
          "[profile][print][phase_object]") {
    for (const char* cf : {R"("")", "3", "null", R"(["context_types"])"}) {
        CAPTURE(cf);
        auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
            std::string(
                R"({"name":"bad","phase_object":)"
                R"({"object":"operation_context","field":"current_state","context_field":)") +
            cf + "}}"));
        REQUIRE(profile != nullptr);
        CHECK_FALSE(profile->has_phase_object());
        CHECK(profile->phase_object_context_field().empty());
        CHECK(profile->required_status_objects().empty());
    }

    // A context field without the rest of the declaration declares nothing.
    auto no_field = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"nofield","phase_object":{"object":"operation_context","context_field":"x"}})"));
    REQUIRE(no_field != nullptr);
    CHECK_FALSE(no_field->has_phase_object());
    CHECK(no_field->phase_object_context_field().empty());
}

TEST_CASE("PrintStartProfile: state patterns stay separate from console patterns",
          "[profile][print][phase_object]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name().find("Forge") != std::string::npos);

    PrintStartProfile::MatchResult result;

    // A state key is not a console line: the response_patterns must not pick
    // it up.
    REQUIRE_FALSE(profile->try_match_pattern("print/HOMING", result));

    // And a console line must not be re-interpreted as state input - the
    // state matcher receives the object's field, nothing else.
    REQUIRE_FALSE(profile->try_match_state("info Cleaning nozzle with a priming line", result));
    REQUIRE(profile->try_match_state("print/HOMING", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);
}

TEST_CASE("PrintStartProfile: malformed phase_object is tolerated",
          "[profile][print][phase_object]") {
    // Missing 'field' - ignored, the profile loads without a phase object.
    auto missing_field = PrintStartProfileTestAccess::parse(
        nlohmann::json::parse(R"({"name":"t1","phase_object":{"object":"operation_context"}})"));
    REQUIRE(missing_field != nullptr);
    REQUIRE_FALSE(missing_field->has_phase_object());
    REQUIRE(missing_field->required_status_objects().empty());

    // Non-object phase_object - same outcome.
    auto not_object = PrintStartProfileTestAccess::parse(
        nlohmann::json::parse(R"({"name":"t2","phase_object":"operation_context"})"));
    REQUIRE(not_object != nullptr);
    REQUIRE_FALSE(not_object->has_phase_object());

    // Empty names carry no information - treated as absent.
    auto empty_name = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"t3","phase_object":{"object":"","field":"current_state"}})"));
    REQUIRE(empty_name != nullptr);
    REQUIRE_FALSE(empty_name->has_phase_object());

    // A well-formed declaration beside the malformed ones still parses.
    auto mixed = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"t4","phase_object":{"bogus":1},)"
        R"("state_patterns":[{"pattern":"HOMING","phase":"HOMING","message":"Homing...","weight":5}]})"));
    // The malformed object above lacks object/field, so it reads as absent -
    // but the state_patterns are independent and still parse. A profile with
    // state patterns and no object simply never feeds them from a status
    // frame; matching still works for direct callers.
    REQUIRE(mixed != nullptr);
    PrintStartProfile::MatchResult result;
    REQUIRE(mixed->try_match_state("HOMING", result));
    CHECK(result.phase == PrintStartPhase::HOMING);
}

TEST_CASE("PrintStartProfile: malformed state_patterns entries are skipped",
          "[profile][print][phase_object]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"t5","state_patterns":[)"
        R"({"pattern":"HOMING","phase":"HOMING","message":"Homing...","weight":5},)"
        R"({"pattern":"NO_PHASE"},)"
        R"({"pattern":"[UNCLOSED","phase":"CLEANING"},)"
        R"({"phase":"PURGING"},)"
        R"("not-an-object",)"
        R"({"pattern":"PURGING","phase":"PURGING","message":"Purging...","weight":10}]})"));
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    // The two well-formed entries survive.
    REQUIRE(profile->try_match_state("HOMING", result));
    CHECK(result.phase == PrintStartPhase::HOMING);
    CHECK(result.message == "Homing...");
    CHECK(result.progress == 5);

    REQUIRE(profile->try_match_state("PURGING", result));
    CHECK(result.phase == PrintStartPhase::PURGING);
    CHECK(result.progress == 10);

    // Every malformed entry is dropped, not half-applied: the entry missing
    // its phase must never fire, and the invalid regex must not abort the
    // parse of the entries after it.
    REQUIRE_FALSE(profile->try_match_state("NO_PHASE", result));
    REQUIRE_FALSE(profile->try_match_state("CLEANING", result));
    REQUIRE(profile->has_phase_object() == false);
    REQUIRE(profile->required_status_objects().empty());
}

// ============================================================================
// Status-Signal Rule Tests
//
// A profile may declare rules over status frames ("status_signals"): an
// AND-list of predicates over one object's fields, mapping to the same
// phase/message/weight contract response_patterns carry. Printers that cannot
// narrate their pre-print get phase inference from physics instead of regexes.
// ============================================================================

TEST_CASE("PrintStartProfile: forge_x declares no status-signal rules",
          "[profile][print][status_signals]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name().find("Forge") != std::string::npos);

    // An AD5M has no filament cutter or chute, and its bed runs -110..110:
    // a both-axes-negative rule fires on the mesh's first point (-105,-105).
    REQUIRE(profile->status_signals().empty());
}

TEST_CASE("PrintStartProfile: default profile carries only the heating and extrusion rules",
          "[profile][print][status_signals]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name() == "Generic");

    const auto& rules = profile->status_signals();
    // Deliberately conservative: static predicates only, no position guesses
    // (ambiguous without narration). The count pins "no rules beyond these".
    REQUIRE(rules.size() == 3);

    using Op = PrintStartProfile::StatusPredicate::Op;

    const auto& bed = rules[0];
    REQUIRE(bed.name == "heating_bed");
    REQUIRE(bed.object == "heater_bed");
    REQUIRE(bed.phase == PrintStartPhase::HEATING_BED);
    REQUIRE(bed.message == "Heating Bed...");
    REQUIRE(bed.weight == 15);
    REQUIRE(bed.when.size() == 2);
    REQUIRE(bed.when[0].field == "target");
    REQUIRE(bed.when[0].index == -1);
    REQUIRE(bed.when[0].op == Op::GT);
    REQUIRE(bed.when[0].value == 0.0);
    // temperature < target - 2: the right-hand side is the sibling field.
    REQUIRE(bed.when[1].field == "temperature");
    REQUIRE(bed.when[1].op == Op::LT);
    REQUIRE(bed.when[1].ref_field == "target");
    REQUIRE(bed.when[1].offset == -2.0);

    const auto& nozzle = rules[1];
    REQUIRE(nozzle.name == "heating_nozzle");
    REQUIRE(nozzle.object == "extruder");
    REQUIRE(nozzle.phase == PrintStartPhase::HEATING_NOZZLE);
    REQUIRE(nozzle.message == "Heating Nozzle...");
    REQUIRE(nozzle.weight == 15);
    REQUIRE(nozzle.when.size() == 2);
    REQUIRE(nozzle.when[1].ref_field == "target");
    REQUIRE(nozzle.when[1].offset == -2.0);

    // Klipper refuses to extrude below min_extrude_temp, so filament moving
    // forward while the head moves is a hot nozzle laying down a purge or
    // prime line. Extrusion with the head still is a load push or a
    // stationary purge, which the collector's purge-shape check judges.
    const auto& purge = rules[2];
    REQUIRE(purge.name == "purging");
    REQUIRE(purge.object == "motion_report");
    REQUIRE(purge.phase == PrintStartPhase::PURGING);
    REQUIRE(purge.when.size() == 2);
    REQUIRE(purge.when[0].field == "live_extruder_velocity");
    REQUIRE(purge.when[0].op == Op::GT);
    REQUIRE(purge.when[0].value == 0.0);
    REQUIRE(purge.when[1].field == "live_velocity");
    REQUIRE(purge.when[1].op == Op::GT);
    REQUIRE(purge.when[1].value == 0.0);
    REQUIRE(purge.after_heat);
    REQUIRE_FALSE(nozzle.after_heat);

    // A display label naming a target sets the phase without narrating.
    for (const char* label : {"Bed: 90c", "Hotend: 270c"}) {
        PrintStartProfile::MatchResult result;
        REQUIRE(profile->try_match_pattern(label, result));
        CHECK_FALSE(result.narrates);
    }
    PrintStartProfile::MatchResult m190;
    REQUIRE(profile->try_match_pattern("M190 S90", m190));
    CHECK(m190.narrates);

    // The phase object leads, then the rule objects in file order.
    const auto objects = profile->required_status_objects();
    REQUIRE(objects.size() == 4);
    REQUIRE(objects[0] == "display_status");
    REQUIRE(objects[1] == "heater_bed");
    REQUIRE(objects[2] == "extruder");
    REQUIRE(objects[3] == "motion_report");
}

TEST_CASE("PrintStartProfile: profiles without status_signals declare none",
          "[profile][print][status_signals]") {
    // Absence must read as absence: no rules to evaluate, no objects
    // subscribed - exactly the behavior before the feature existed.
    const auto k2 = PrintStartProfile::load("creality_k2");
    REQUIRE(k2 != nullptr);
    REQUIRE(k2->status_signals().empty());
    REQUIRE(k2->required_status_objects().empty());

    auto inline_profile =
        PrintStartProfileTestAccess::parse(nlohmann::json::parse(R"({"name":"t6"})"));
    REQUIRE(inline_profile != nullptr);
    REQUIRE(inline_profile->status_signals().empty());
    REQUIRE(inline_profile->required_status_objects().empty());

    // The key present but not an array is a malformed declaration, not a rule
    // set: tolerated like any other parse error.
    auto not_array = PrintStartProfileTestAccess::parse(
        nlohmann::json::parse(R"({"name":"t6b","status_signals":{"name":"x"}})"));
    REQUIRE(not_array != nullptr);
    REQUIRE(not_array->status_signals().empty());
}

TEST_CASE("PrintStartProfile: malformed status_signals entries are skipped",
          "[profile][print][status_signals]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"t7","status_signals":[)"
        R"({"name":"good","object":"toolhead","when":[{"field":"position","index":0,"op":"lt","value":0}],"phase":"PURGING","message":"m","weight":7},)"
        R"("not-an-object",)"
        R"({"object":"toolhead","when":[{"field":"x","op":"lt","value":0}],"phase":"PURGING"},)"
        R"({"name":"no_object","when":[{"field":"x","op":"lt","value":0}],"phase":"PURGING"},)"
        R"({"name":"no_when","object":"toolhead","phase":"PURGING"},)"
        R"({"name":"empty_when","object":"toolhead","when":[],"phase":"PURGING"},)"
        R"({"name":"bad_op","object":"toolhead","when":[{"field":"x","op":"between","value":0}],"phase":"PURGING"},)"
        R"({"name":"no_op","object":"toolhead","when":[{"field":"x","value":0}],"phase":"PURGING"},)"
        R"({"name":"no_tolerance","object":"toolhead","when":[{"field":"x","op":"near","value":5}],"phase":"PURGING"},)"
        R"({"name":"zero_tolerance","object":"toolhead","when":[{"field":"x","op":"near","value":5,"tolerance":0}],"phase":"PURGING"},)"
        R"({"name":"no_rhs","object":"toolhead","when":[{"field":"x","op":"lt"}],"phase":"PURGING"},)"
        R"({"name":"ambiguous_rhs","object":"toolhead","when":[{"field":"x","op":"lt","value":1,"ref_field":"y"}],"phase":"PURGING"},)"
        R"({"name":"bad_index","object":"toolhead","when":[{"field":"x","index":-1,"op":"lt","value":0}],"phase":"PURGING"},)"
        R"({"name":"fractional_index","object":"toolhead","when":[{"field":"x","index":1.5,"op":"lt","value":0}],"phase":"PURGING"},)"
        R"({"name":"no_phase","object":"toolhead","when":[{"field":"x","op":"lt","value":0}]},)"
        R"({"name":"idle_phase","object":"toolhead","when":[{"field":"x","op":"lt","value":0}],"phase":"IDLE"},)"
        R"({"name":"good","object":"toolhead","when":[{"field":"x","op":"lt","value":0}],"phase":"HOMING"},)"
        R"({"name":"late_good","object":"motion_report","when":[{"field":"live_extruder_velocity","op":"gt","value":0}],"phase":"HOMING","message":"m2","weight":9}]})"));
    REQUIRE(profile != nullptr);

    // Every malformed entry is dropped whole - a rule with one bad predicate
    // is not a narrower AND-list. The well-formed first entry survives with
    // its own phase (a duplicate name never overwrites it), and parsing
    // continues past every failure to reach the last entry.
    const auto& rules = profile->status_signals();
    REQUIRE(rules.size() == 2);
    REQUIRE(rules[0].name == "good");
    REQUIRE(rules[0].phase == PrintStartPhase::PURGING);
    REQUIRE(rules[0].weight == 7);
    REQUIRE(rules[1].name == "late_good");
    REQUIRE(rules[1].object == "motion_report");
    REQUIRE(rules[1].when[0].op == PrintStartProfile::StatusPredicate::Op::GT);
}

TEST_CASE("PrintStartProfile: status-signal predicate ops table",
          "[profile][print][status_signals]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"t8","status_signals":[)"
        R"({"name":"eq","object":"o","when":[{"field":"scalar","op":"eq","value":9.5}],"phase":"HOMING","message":"eq holds","weight":1},)"
        R"({"name":"ne","object":"o","when":[{"field":"scalar","op":"ne","value":9}],"phase":"HOMING","weight":2},)"
        R"({"name":"gt","object":"o","when":[{"field":"target","op":"gt","value":50}],"phase":"HOMING","weight":3},)"
        R"({"name":"lt_index","object":"o","when":[{"field":"a.list","index":1,"op":"lt","value":14.5}],"phase":"HOMING","weight":4},)"
        R"({"name":"near_in","object":"o","when":[{"field":"scalar","op":"near","value":10,"tolerance":0.6}],"phase":"HOMING","weight":5},)"
        R"({"name":"near_out","object":"o","when":[{"field":"scalar","op":"near","value":10,"tolerance":0.4}],"phase":"HOMING","weight":6},)"
        R"({"name":"and_list","object":"o","when":[{"field":"target","op":"gt","value":0},{"field":"temperature","op":"lt","ref_field":"target","offset":-2}],"phase":"HEATING_BED","message":"and holds","weight":7},)"
        R"({"name":"and_second_fails","object":"o","when":[{"field":"target","op":"gt","value":0},{"field":"temperature","op":"gt","ref_field":"target"}],"phase":"HOMING","weight":8},)"
        R"({"name":"offset_literal","object":"o","when":[{"field":"scalar","op":"gt","value":5,"offset":3}],"phase":"HOMING","weight":9},)"
        R"({"name":"missing_field","object":"o","when":[{"field":"absent","op":"gt","value":0}],"phase":"HOMING","weight":10},)"
        R"({"name":"non_numeric","object":"o","when":[{"field":"text","op":"eq","value":1}],"phase":"HOMING","weight":11},)"
        R"({"name":"index_range","object":"o","when":[{"field":"a.list","index":7,"op":"gt","value":0}],"phase":"HOMING","weight":12},)"
        R"({"name":"ref_missing","object":"o","when":[{"field":"scalar","op":"lt","ref_field":"absent"}],"phase":"HOMING","weight":13}]})"));
    REQUIRE(profile != nullptr);

    const nlohmann::json status = nlohmann::json::parse(
        R"({"scalar":9.5,"target":60,"temperature":25,"text":"abc","a":{"list":[3,14,15]}})");

    const auto& rules = profile->status_signals();
    REQUIRE(rules.size() == 13);

    struct Row {
        size_t rule;
        bool expected;
    };
    const Row rows[] = {
        {0, true},   // eq: 9.5 == 9.5
        {1, true},   // ne: 9.5 != 9
        {2, true},   // gt: 60 > 50
        {3, true},   // lt with array index: list[1] = 14 < 14.5
        {4, true},   // near: |9.5 - 10| = 0.5 < 0.6
        {5, false},  // near: 0.5 !< 0.4
        {6, true},   // AND of both, second against ref field: 25 < 60 - 2
        {7, false},  // AND fails on the second predicate: 25 > 60 is false
        {8, true},   // offset applies to a literal too: 9.5 > 5 + 3
        {9, false},  // field the frame does not carry
        {10, false}, // non-numeric field never satisfies a numeric op
        {11, false}, // array index out of range
        {12, false}, // ref field the frame does not carry
    };

    for (const auto& row : rows) {
        CAPTURE(rules[row.rule].name);
        PrintStartProfile::MatchResult result;
        const bool held = profile->evaluate_status_signal(status, rules[row.rule], result);
        CHECK(held == row.expected);
    }

    // A holding rule produces the same match shape a pattern match does.
    PrintStartProfile::MatchResult result;
    REQUIRE(profile->evaluate_status_signal(status, rules[0], result));
    CHECK(result.phase == PrintStartPhase::HOMING);
    CHECK(result.message == "eq holds");
    CHECK(result.progress == 1);

    REQUIRE(profile->evaluate_status_signal(status, rules[6], result));
    CHECK(result.phase == PrintStartPhase::HEATING_BED);
    CHECK(result.message == "and holds");
    CHECK(result.progress == 7);
}

// ============================================================================
// Tag patterns
// ============================================================================

TEST_CASE("PrintStartProfile: tag patterns are their own list", "[profile][print][tag]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"tags",)"
        R"("tag_patterns":[{"pattern":"LOAD_UNIT","phase":"INITIALIZING","message":"Loading"}],)"
        R"("response_patterns":[{"pattern":"G28","phase":"HOMING","weight":10}]})"));
    REQUIRE(profile != nullptr);
    PrintStartProfile::MatchResult result;

    REQUIRE(profile->try_match_tag("// LOAD_UNIT 2", result));
    CHECK(result.phase == PrintStartPhase::INITIALIZING);
    CHECK(result.message == "Loading");
    CHECK(result.reported_percent == -1);

    CHECK_FALSE(profile->try_match_tag("G28", result));
    CHECK_FALSE(profile->try_match_pattern("// LOAD_UNIT 2", result));
}

TEST_CASE("PrintStartProfile: a tag's progress comes from the group it names",
          "[profile][print][tag]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"j({"name":"progress","tag_patterns":[)j"
        R"j({"pattern":"flush ([0-9.]+)","phase":"PURGING","message":"Purging",)j"
        R"j("progress_group":1},)j"
        R"j({"pattern":"wipe ([0-9.]+)","phase":"CLEANING","progress_group":2},)j"
        R"j({"pattern":"rinse","phase":"PURGING"}]})j"));
    REQUIRE(profile != nullptr);
    PrintStartProfile::MatchResult result;

    SECTION("a value at or below 1.5 is a fraction") {
        REQUIRE(profile->try_match_tag("flush 0.42", result));
        CHECK(result.reported_percent == 42);
        REQUIRE(profile->try_match_tag("flush 1.5", result));
        CHECK(result.reported_percent == 100);
    }

    SECTION("a larger value is already a percent, clamped") {
        REQUIRE(profile->try_match_tag("flush 75", result));
        CHECK(result.reported_percent == 75);
        REQUIRE(profile->try_match_tag("flush 250", result));
        CHECK(result.reported_percent == 100);
    }

    SECTION("a group the pattern does not have reports nothing") {
        REQUIRE(profile->try_match_tag("wipe 0.5", result));
        CHECK(result.reported_percent == -1);
    }

    SECTION("a match without progress clears the last one") {
        REQUIRE(profile->try_match_tag("flush 0.5", result));
        REQUIRE(profile->try_match_tag("rinse", result));
        CHECK(result.reported_percent == -1);
    }
}

TEST_CASE("PrintStartProfile: percent_from_report", "[profile][print][tag]") {
    CHECK(PrintStartProfile::percent_from_report(0.0) == 0);
    CHECK(PrintStartProfile::percent_from_report(0.005) == 1);
    CHECK(PrintStartProfile::percent_from_report(1.0) == 100);
    CHECK(PrintStartProfile::percent_from_report(2.0) == 2);
    CHECK(PrintStartProfile::percent_from_report(99.6) == 100);
    CHECK(PrintStartProfile::percent_from_report(1e9) == 100);
    CHECK(PrintStartProfile::percent_from_report(-0.5) == -1);
    CHECK(PrintStartProfile::percent_from_report(std::nan("")) == -1);
}

TEST_CASE("PrintStartProfile: case_sensitive compiles a pattern without ICase",
          "[profile][print][pattern]") {
    auto profile = PrintStartProfileTestAccess::parse(nlohmann::json::parse(
        R"({"name":"case",)"
        R"("response_patterns":[)"
        R"({"pattern":"BOX_LOAD","phase":"INITIALIZING","case_sensitive":true},)"
        R"({"pattern":"g28","phase":"HOMING","case_sensitive":"yes"}]})"));
    REQUIRE(profile != nullptr);
    PrintStartProfile::MatchResult result;

    CHECK(profile->try_match_pattern("BOX_LOAD", result));
    CHECK_FALSE(profile->try_match_pattern("box_load", result));
    // A non-boolean value is ignored: the pattern stays case-insensitive.
    CHECK(profile->try_match_pattern("G28", result));
}

TEST_CASE("PrintStartProfile: the K2 profiles' tag patterns read the K2 tag stream",
          "[profile][print][tag][k2]") {
    for (const char* name : {"creality_k2", "k2_improvements"}) {
        auto profile = PrintStartProfile::load(name);
        REQUIRE(profile != nullptr);
        if (profile->is_default()) {
            SKIP(std::string(name) + ".json not available");
        }
        INFO(name);
        PrintStartProfile::MatchResult result;

        REQUIRE(
            profile->try_match_tag("// num: 0, velocity: 575.000000, percent 0.500000", result));
        CHECK(result.phase == PrintStartPhase::PURGING);
        CHECK(result.reported_percent == 50);
        REQUIRE(profile->try_match_tag("// num: 0, velocity: 23.0, percent: 75", result));
        CHECK(result.reported_percent == 75);
        REQUIRE(profile->try_match_tag("// velocity: 23.0 percent 1.000000", result));
        CHECK(result.reported_percent == 100);

        // "percent" alone, or with no number after it, is not the purge tag.
        CHECK_FALSE(profile->try_match_tag("// percent 0.5", result));
        CHECK_FALSE(profile->try_match_tag("// num: 3, percent done", result));

        for (const char* line :
             {"// [box] cut sensor detected", "// [box] cut to return", "BOX_LOAD_MATERIAL"}) {
            REQUIRE(profile->try_match_tag(line, result));
            CHECK(result.phase == PrintStartPhase::INITIALIZING);
            CHECK(result.message == "Loading Filament...");
            CHECK(result.reported_percent == -1);
        }
        CHECK_FALSE(profile->try_match_tag("box_load_material", result));
        CHECK_FALSE(profile->try_match_tag("// probe at 190.000,8.000 is z=-0.580000", result));
    }
}
