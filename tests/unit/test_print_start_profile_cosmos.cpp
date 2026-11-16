// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// COSMOS (Elegoo Centauri Carbon) Profile Tests
//
// Console lines are verbatim from klippy.log of a CC1 print on COSMOS 26.08
// (2026-09-14): RESPOND and SET_DISPLAY_TEXT output reaches gcode_response
// with the "// " prefix, and SET_DISPLAY_TEXT also sets display_status.message
// without it. The skew line comes from LOAD_SKEW_IF_EXISTS, whose RESPOND
// PREFIX="Info:" goes out raw, so it carries no "// ". Lines for branches that
// print did not take (chamber wait, adaptive mesh, cancels) are the
// SET_DISPLAY_TEXT messages in COSMOS macros.cfg.
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_cosmos_profile() {
    return PrintStartProfile::load("cosmos_cc1");
}

static constexpr const char* kNoSkewLine =
    "Info: No skew profile defined. If skew correction is desired, create 'my_skew_profile' "
    "in printer.cfg";

TEST_CASE("PrintStartProfile: cosmos_cc1 maps the PRINT_START narration",
          "[profile][print][cosmos]") {
    auto profile = get_cosmos_profile();
    REQUIRE(profile != nullptr);
    REQUIRE_FALSE(profile->is_default());
    REQUIRE(profile->name().find("COSMOS") != std::string::npos);

    PrintStartProfile::MatchResult result;

    SECTION("Heat soak is its own phase") {
        REQUIRE(profile->try_match_pattern("// Heatsoak: 1.0m", result));
        REQUIRE(result.phase == PrintStartPhase::SOAKING);
        REQUIRE(result.message == "Heat Soaking...");
    }

    SECTION("A heat soak holds the pre-print for the minutes it announces") {
        // COSMOS prints the soak and then waits it out in a silent G4.
        REQUIRE(profile->try_match_pattern("// Heatsoak: 1.0m", result));
        REQUIRE(result.hold_seconds == 60);
        REQUIRE(profile->try_match_pattern("// Heatsoak: 10.0m", result));
        REQUIRE(result.hold_seconds == 600);
        REQUIRE(profile->try_match_pattern("// Heatsoak: 0.5m", result));
        REQUIRE(result.hold_seconds == 30);
        REQUIRE(profile->try_match_pattern("// Heatsoak: 0.0m", result));
        REQUIRE(result.hold_seconds == 0);
    }

    SECTION("Chamber wait is chamber heating that holds nothing") {
        REQUIRE(profile->try_match_pattern("// Heatsoak: 10.0m", result));
        REQUIRE(profile->try_match_pattern("Chamber: 45c", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_CHAMBER);
        REQUIRE(result.message == "Heating Chamber...");
        // M191 waits on the chamber sensor for no known time.
        REQUIRE(result.hold_seconds == 0);
    }

    SECTION("Stored mesh load") {
        REQUIRE(profile->try_match_pattern("// Using default bed mesh", result));
        REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    }

    SECTION("Adaptive mesh probing") {
        REQUIRE(profile->try_match_pattern("Creating adaptive bed mesh", result));
        REQUIRE(result.phase == PrintStartPhase::BED_MESH);
        REQUIRE(result.message == "Probing Bed Mesh...");
    }

    SECTION("Smart park precedes the print-temperature M109") {
        REQUIRE(profile->try_match_pattern("// Smart Park location: 41.0714,33.0046.", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
    }

    SECTION("KAMP line purge") {
        REQUIRE(profile->try_match_pattern(
            "// KAMP purge starting at 113.0002, 33.0046 and purging 30.0mm of filament, "
            "requested flow rate is 12.0mm3/s.",
            result));
        REQUIRE(result.phase == PrintStartPhase::PURGING);
    }

    SECTION("Skew check completes the pre-print whether or not a profile exists") {
        REQUIRE(profile->try_match_pattern(kNoSkewLine, result));
        REQUIRE(result.phase == PrintStartPhase::COMPLETE);

        PrintStartProfile::MatchResult found;
        REQUIRE(profile->try_match_pattern("// Skew profile 'my_skew_profile' found. Loading...",
                                           found));
        REQUIRE(found.phase == PrintStartPhase::COMPLETE);
    }
}

TEST_CASE("PrintStartProfile: cosmos_cc1 reads the macro's display text",
          "[profile][print][cosmos][phase_object]") {
    auto profile = get_cosmos_profile();
    REQUIRE(profile != nullptr);
    REQUIRE_FALSE(profile->is_default());

    // SET_DISPLAY_TEXT lands in display_status.message, with no "// " prefix.
    REQUIRE(profile->has_phase_object());
    REQUIRE(profile->phase_object_name() == "display_status");
    REQUIRE(profile->phase_object_field() == "message");

    PrintStartProfile::MatchResult result;
    REQUIRE(profile->try_match_state("Heatsoak: 10.0m", result));
    REQUIRE(result.phase == PrintStartPhase::SOAKING);
    // The display copy of the soak announces the same hold as the console line.
    REQUIRE(result.hold_seconds == 600);

    REQUIRE(profile->try_match_state("Using default bed mesh", result));
    REQUIRE(result.phase == PrintStartPhase::BED_MESH);
}

TEST_CASE("PrintStartProfile: cosmos_cc1 ignores lines that are not a step",
          "[profile][print][cosmos][negative]") {
    auto profile = get_cosmos_profile();
    REQUIRE(profile != nullptr);
    REQUIRE_FALSE(profile->is_default());

    PrintStartProfile::MatchResult result;

    SECTION("M190 and M109 wait reports") {
        // The heater waits print one of these a second for minutes; matching
        // any phase here would pin the display to whatever the regex says.
        REQUIRE_FALSE(profile->try_match_pattern("B:94.5 /105.0 T0:140.1 /140.0", result));
        REQUIRE_FALSE(profile->try_match_pattern("B:105.0 /105.0 T0:212.4 /260.0", result));
    }

    SECTION("KAMP chatter ahead of the purge") {
        REQUIRE_FALSE(profile->try_match_pattern(
            "// KAMP purge is not using firmware retraction, it is recommended to configure it.",
            result));
        REQUIRE_FALSE(profile->try_match_pattern("// Moving filament tip 5.0mms", result));
    }

    SECTION("Cancel paths announce no phase") {
        REQUIRE_FALSE(profile->try_match_pattern("NO BED MESH!", result));
        REQUIRE_FALSE(profile->try_match_pattern("NO FILAMENT LOADED!", result));
        REQUIRE_FALSE(profile->try_match_pattern(
            "Calibration process not completed. Please complete initial calibration first",
            result));
        REQUIRE_FALSE(profile->try_match_pattern(
            "Adaptive bed mesh requires nozzle z homing. Please enable nozzle z homing.", result));
    }
}
