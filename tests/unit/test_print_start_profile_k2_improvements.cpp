// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

using namespace helix;

#include "../catch_amalgamated.hpp"

// ============================================================================
// k2-improvements profile
//
// The mod replaces Creality's START_PRINT on stock firmware and narrates each
// step through RESPOND with PREFIX="[START_PRINT]:" plus M117. Every input
// below is the literal text its macros emit (features/macros/*.cfg upstream).
// Its heat soak and M191 chamber wait are the longest stretches of the
// sequence and are the reason SOAKING exists.
// ============================================================================

TEST_CASE("PrintStartProfile: k2_improvements maps the mod's narration",
          "[profile][print][k2improvements]") {
    auto profile = PrintStartProfile::load("k2_improvements");
    REQUIRE(profile != nullptr);

    PrintStartProfile::MatchResult result;

    SECTION("Bed wait") {
        REQUIRE(profile->try_match_pattern("// [START_PRINT]: Waiting for bed to reach 60C...",
                                           result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
    }

    SECTION("Soak countdown is SOAKING, not a heating phase") {
        REQUIRE(profile->try_match_pattern("// [START_PRINT]: Soaking: 4 min 0 sec left", result));
        REQUIRE(result.phase == PrintStartPhase::SOAKING);
    }

    SECTION("M191 chamber wait is also SOAKING") {
        REQUIRE(profile->try_match_pattern("// [START_PRINT]: Waiting for chamber to reach 45C...",
                                           result));
        REQUIRE(result.phase == PrintStartPhase::SOAKING);
    }

    SECTION("Bed-assist notice keeps the chamber wait in SOAKING") {
        // M191 drives the bed to 105C to help the chamber. Matching that on
        // the bed heater would walk the phase backwards mid-soak.
        REQUIRE(profile->try_match_pattern(
            "// The chamber heater alone can not reach 45.0c, using bed assist ...", result));
        REQUIRE(result.phase == PrintStartPhase::SOAKING);
    }

    SECTION("Rehome after the soak") {
        REQUIRE(profile->try_match_pattern(
            "// [START_PRINT]: Rehoming Z after reaching bedtemp/soak", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }

    SECTION("Cartographer adaptive mesh") {
        REQUIRE(profile->try_match_pattern("// [START_PRINT]: Carto meshing bed", result));
        REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    }

    SECTION("CFS macros still drive cleaning and purging") {
        REQUIRE(profile->try_match_pattern("[GCODE]BOX_NOZZLE_CLEAN", result));
        REQUIRE(result.phase == PrintStartPhase::CLEANING);
        REQUIRE(profile->try_match_pattern("[GCODE]BOX_GO_TO_EXTRUDE_POS", result));
        REQUIRE(result.phase == PrintStartPhase::PURGING);
    }

    SECTION("The macro's own parameter echo announces no phase") {
        // START_PRINT echoes its arguments before doing anything. The string
        // carries BED_TEMP and EXTRUDER_TEMP, so a pattern keyed on those bare
        // tokens would enter two phases before the printer has moved and
        // consume the real ones.
        REQUIRE_FALSE(profile->try_match_pattern(
            "// [START_PRINT]: Start print macro called with BED_TEMP=60 EXTRUDER_TEMP=220 "
            "CHAMBER_TEMP=0 MATERIAL=PLA",
            result));
    }

    SECTION("Our own pre-start command echo announces no phase") {
        REQUIRE_FALSE(profile->try_match_pattern(
            "[GCODE]BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='bench.gcode' "
            "BED_TEMP=60 EXTRUDER_TEMP=220",
            result));
    }
}
