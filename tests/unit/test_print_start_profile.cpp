// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_profile.cpp
 * @brief Unit tests for PrintStartProfile JSON-driven pattern matching
 *
 * Tests the profile loading, signal format matching, regex response patterns,
 * and progress calculation. No LVGL or Moonraker required - pure logic tests.
 */

#include "print_start_profile.h"

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
        SECTION("Profile has sequential progress mode") {
            REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL);
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
// Phase Weight Tests
// ============================================================================

TEST_CASE("PrintStartProfile: phase weights match expected values", "[profile][print]") {
    auto profile = get_default_profile();
    REQUIRE(profile != nullptr);

    SECTION("Known phases have non-zero weights") {
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 10);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_BED) == 20);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 20);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::QGL) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::Z_TILT) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 10);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 5);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 5);
    }

    SECTION("Unknown/unused phases return 0") {
        REQUIRE(profile->get_phase_weight(PrintStartPhase::IDLE) == 0);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::COMPLETE) == 0);
    }
}

TEST_CASE("PrintStartProfile: forge_x phase weights", "[profile][print]") {
    auto profile = get_forge_x_profile();
    REQUIRE(profile != nullptr);

    // Only test if forge_x loaded (not default fallback)
    if (profile->name().find("Forge") != std::string::npos) {
        REQUIRE(profile->get_phase_weight(PrintStartPhase::INITIALIZING) == 5);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 5);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_BED) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 15);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 20);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 25);
        REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 10);
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

    SECTION("Extruder temperature wait line with capture group") {
        REQUIRE(profile->try_match_pattern("// Wait extruder temperature to reach 210", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
        // $1 should be substituted with "210"
        REQUIRE(result.message.find("210") != std::string::npos);
    }

    SECTION("Different temperature values") {
        REQUIRE(profile->try_match_pattern("// Wait bed temperature to reach 110", result));
        REQUIRE(result.message.find("110") != std::string::npos);

        REQUIRE(profile->try_match_pattern("// Wait extruder temperature to reach 250", result));
        REQUIRE(result.message.find("250") != std::string::npos);
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

    SECTION("Forge-X profile uses sequential mode") {
        auto profile = get_forge_x_profile();
        REQUIRE(profile != nullptr);

        if (profile->name().find("Forge") != std::string::npos) {
            REQUIRE(profile->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL);
        }
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

    // Should get the default profile (either from JSON or built-in)
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

    SECTION("Default profile is always available (built-in fallback)") {
        // Even if no JSON files exist, load_default() should return a usable profile
        auto profile = PrintStartProfile::load_default();
        REQUIRE(profile != nullptr);
        REQUIRE_FALSE(profile->name().empty());

        // Built-in patterns should work
        PrintStartProfile::MatchResult result;
        REQUIRE(profile->try_match_pattern("G28", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }
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
        REQUIRE(profile->try_match_pattern("// Wait extruder temperature to reach 300", result));
        REQUIRE(result.message.find("300") != std::string::npos);
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

TEST_CASE("PrintStartProfile: creality_k1 phase weights", "[profile][print][k1]") {
    auto profile = get_creality_k1_profile();
    REQUIRE(profile != nullptr);

    if (profile->name().find("K1") == std::string::npos) {
        SKIP("creality_k1.json not available");
    }

    REQUIRE(profile->get_phase_weight(PrintStartPhase::INITIALIZING) == 5);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 10);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 15);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 15);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 30);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 20);
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
    // and weighted progress increases
    std::set<PrintStartPhase> detected;
    int total_weight = 0;

    auto process = [&](const std::string& line) {
        if (profile->try_match_pattern(line, result)) {
            if (detected.insert(result.phase).second) {
                total_weight += profile->get_phase_weight(result.phase);
            }
        }
    };

    process("// not prepare.");
    REQUIRE(detected.count(PrintStartPhase::INITIALIZING) == 1);
    REQUIRE(total_weight == 5);

    process("// x_axes: xyz");
    REQUIRE(detected.count(PrintStartPhase::HOMING) == 1);
    REQUIRE(total_weight == 15);

    process("// [CLEAR_NOZZLE_QUICK] src_pos[2]:3.28");
    REQUIRE(detected.count(PrintStartPhase::CLEANING) == 1);
    REQUIRE(total_weight == 30);

    process("CX_PRINT_LEVELING_CALIBRATION");
    REQUIRE(detected.count(PrintStartPhase::BED_MESH) == 1);
    REQUIRE(total_weight == 45);

    process("// can_break_flag = 0");
    REQUIRE(detected.count(PrintStartPhase::HEATING_NOZZLE) == 1);
    REQUIRE(total_weight == 75);

    // Repeated temp reports should not add new phases
    process("B:56.8 /55.0 T0:175.3 /220.0");
    REQUIRE(total_weight == 75);

    process("// can_break_flag = 3");
    REQUIRE(detected.count(PrintStartPhase::PURGING) == 1);
    REQUIRE(total_weight == 95);
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
        // --- "Changed main state to PRINTING with action" verb
        //     (evidence: gcode_store 23:23:26) ---
        {"// Success: Changed main state to PRINTING with action PRINT_BED_DETECTING",
         PrintStartPhase::BED_MESH, "Inspecting bed..."},
        {"// Success: Changed main state to PRINTING with action PRINT_SWITCH_CHECKING",
         PrintStartPhase::INITIALIZING, "Checking extruders..."},
        {"// Success: Changed main state to PRINTING with action DETECT_PLATE",
         PrintStartPhase::BED_MESH, "Detecting plate..."},
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

    // Initial single-point Z touch right AFTER homing, BEFORE bed mesh — it
    // fires at the very start of the sequence (evidence: gcode_store 23:23:14
    // "// probe_start_x: 5.32188, probe_start_y: 4.87969", immediately after
    // the trigger_mcu_pos homing lines and ~3.5 min before the actual mesh).
    // It used to be mislabelled "Z Calibration..." under BED_MESH, which made
    // the UI show bed-mesh-flavoured text during homing. It now belongs to
    // HOMING / "Probing Z...".
    REQUIRE(
        profile->try_match_pattern("// probe_start_x: 5.32188, probe_start_y: 4.87969", result));
    REQUIRE(result.phase == PrintStartPhase::HOMING);
    REQUIRE(result.message == "Probing Z...");

    // End-of-mesh marker (evidence: gcode_store
    // "// z_mesh_complete: 0.02573436601557052"). The per-probe "probe at x:..."
    // lines are consumed by the collector's mesh counter, not the profile;
    // z_mesh_complete is the END boundary in the response stream.
    REQUIRE(profile->try_match_pattern("// z_mesh_complete: 0.02573436601557052", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    REQUIRE(result.message == "Bed mesh...");

    // Mesh START boundary — bed_mesh.py emits "// z offset:" right before the
    // first mesh probe (evidence: gcode_store 20:21:26 "// z offset: -0.05",
    // immediately preceding "// probe at x: 129.915, y: 125.560 is z=..."). This
    // relabels the display from the prior BED_MESH sub-phase ("Detecting plate")
    // to "Bed mesh" at the moment the real mesh begins — fixing the device bug
    // where only the END marker fired and the stale label persisted through the
    // whole mesh.
    REQUIRE(profile->try_match_pattern("// z offset: -0.05", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    REQUIRE(result.message == "Bed mesh...");

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

TEST_CASE("PrintStartProfile: snapmaker_u1 phase weights sum reasonably",
          "[profile][print][snapmaker]") {
    auto profile = get_snapmaker_u1_profile();
    REQUIRE(profile != nullptr);
    if (profile->name().find("Snapmaker") == std::string::npos) {
        SKIP("snapmaker_u1.json not available");
    }

    // Weights tuned from the REAL 2026-06-18 timeline: heating dominates wall
    // time (bed 60C + nozzle 220C span almost the whole ~4 min), bed work
    // (inspect ~1 min + plate detect + mesh ~1 min) is the next chunk, and the
    // tool-switch / auto-feed / replenish steps land under INITIALIZING.
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HOMING) == 6);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_BED) == 22);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::HEATING_NOZZLE) == 22);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::INITIALIZING) == 14);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::BED_MESH) == 26);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::CLEANING) == 5);
    REQUIRE(profile->get_phase_weight(PrintStartPhase::PURGING) == 5);
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
