// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

using namespace helix;

#include "../catch_amalgamated.hpp"

// ============================================================================
// Jacobean's K2 Plus firmware profile
//
// The replacement firmware's START_PRINT (config/k2/start_print.cfg in
// Jacob10383/kalico) narrates each step through STATUS_MSG: M117 plus RESPOND
// PREFIX="[START_PRINT]:". Every input below is that macro's literal text with
// its float temperatures filled in (prestonbrown/helixscreen#2087).
// ============================================================================

TEST_CASE("PrintStartProfile: k2_jacobean maps the firmware's narration",
          "[profile][print][k2][2087]") {
    auto profile = PrintStartProfile::load("k2_jacobean");
    REQUIRE(profile != nullptr);
    REQUIRE_FALSE(profile->is_default());

    PrintStartProfile::MatchResult result;

    SECTION("Preheat and the bed wait are bed heating") {
        for (const char* line : {"[START_PRINT]: Preheating: Nozzle=140C Bed=60.0C Chamber=0.0C",
                                 "[START_PRINT]: Waiting for bed to reach 60.0°C..."}) {
            INFO(line);
            REQUIRE(profile->try_match_pattern(line, result));
            CHECK(result.phase == PrintStartPhase::HEATING_BED);
        }
    }

    SECTION("The soak countdown is SOAKING") {
        REQUIRE(profile->try_match_pattern("[START_PRINT]: Soaking: 4 min 0 sec left", result));
        CHECK(result.phase == PrintStartPhase::SOAKING);
    }

    SECTION("The chamber wait, bed assist included, is chamber heating") {
        for (const char* line :
             {"[START_PRINT]: Using aux fans for bed-assisted chamber heating...",
              "[START_PRINT]: Waiting for chamber to reach minimum 40.0°C (target 50.0°C)..."}) {
            INFO(line);
            REQUIRE(profile->try_match_pattern(line, result));
            CHECK(result.phase == PrintStartPhase::HEATING_CHAMBER);
        }
    }

    SECTION("The adaptive mesh") {
        REQUIRE(profile->try_match_pattern("[START_PRINT]: Meshing bed", result));
        CHECK(result.phase == PrintStartPhase::BED_MESH);
    }

    SECTION("The nozzle waits are nozzle heating") {
        for (const char* line :
             {"[START_PRINT]: Waiting for nozzle to reach 140C before probing...",
              "[START_PRINT]: Setting extruder to 220.0°C..."}) {
            INFO(line);
            REQUIRE(profile->try_match_pattern(line, result));
            CHECK(result.phase == PrintStartPhase::HEATING_NOZZLE);
        }
    }

    SECTION("The macro's own parameter echo announces no phase") {
        // It carries BED_TEMP, EXTRUDER_TEMP and CHAMBER_TEMP before anything
        // has moved.
        REQUIRE_FALSE(profile->try_match_pattern(
            "[START_PRINT]: Start print macro called with {'EXTRUDER_TEMP': '220', "
            "'BED_TEMP': '60', 'MATERIAL': 'PLA', 'CHAMBER_TEMP': '0', 'MIN_CHAMBER_TEMP': '0'}",
            result));
        REQUIRE_FALSE(
            profile->try_match_pattern("[START_PRINT]: Setting Z offset of 0.0 for PLA", result));
    }
}
