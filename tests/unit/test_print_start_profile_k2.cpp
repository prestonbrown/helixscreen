// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"

using namespace helix;

#include "../catch_amalgamated.hpp"

// ============================================================================
// Creality K2 Profile Tests
//
// Every input line below is verbatim from a K2 Plus klippy.log capture
// (2026-08-18, three prints; see PRINT_START_OBSERVERS.md for the source).
// The K2 narrates through [GCODE]/[DEBUG] respond echoes when started from
// a path that enables them, and stays fully silent otherwise - which is why
// the profile also enables position_signals.
// ============================================================================

static std::shared_ptr<PrintStartProfile> get_k2_profile() {
    return PrintStartProfile::load("creality_k2");
}

TEST_CASE("PrintStartProfile: creality_k2 matches captured narration", "[profile][print][k2]") {
    auto profile = get_k2_profile();
    REQUIRE(profile != nullptr);
    REQUIRE(profile->name().find("K2") != std::string::npos);

    PrintStartProfile::MatchResult result;

    SECTION("Homing rails echo") {
        REQUIRE(profile->try_match_pattern("[DEBUG]_handle_home_rails_begin", result));
        REQUIRE(result.phase == PrintStartPhase::HOMING);
    }

    SECTION("Nozzle clear counter") {
        REQUIRE(profile->try_match_pattern("[NOZZLE_CLEAR] START NOZZLE_CLEAR COUNT:0", result));
        REQUIRE(result.phase == PrintStartPhase::CLEANING);
    }

    SECTION("Box nozzle clean") {
        REQUIRE(profile->try_match_pattern("[GCODE]BOX_NOZZLE_CLEAN", result));
        REQUIRE(result.phase == PrintStartPhase::CLEANING);
    }

    SECTION("Pre-wipe nozzle heat uses TEMPERATURE_WAIT, not M109") {
        // The K2 heats to its wipe temperature with M104 + TEMPERATURE_WAIT;
        // the M109 echoes only appear after the wiper. Without this pattern
        // the heating phase is invisible until late in the chain.
        REQUIRE(profile->try_match_pattern(
            "[GCODE]TEMPERATURE_WAIT SENSOR=extruder MINIMUM=175 MAXIMUM=185", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_NOZZLE);
    }

    SECTION("CFS flush position is the purge marker") {
        // BOX_MATERIAL_FLUSH / flush_* never appear in any capture; the box's
        // move to the extrude position is what marks the flush stretch.
        REQUIRE(profile->try_match_pattern("[GCODE]BOX_GO_TO_EXTRUDE_POS", result));
        REQUIRE(result.phase == PrintStartPhase::PURGING);
    }

    SECTION("BED_MESH_CLEAR must NOT announce the mesh phase") {
        // The K2 emits BED_MESH_CLEAR three times per start (mesh validation,
        // pre-nozzle-clean hygiene, pre-print) and again at end of print.
        // Mapping it to BED_MESH would enter the phase on every hygiene clear.
        // Mesh entry comes from the bed-mesh presence flap and position
        // inference instead.
        REQUIRE_FALSE(profile->try_match_pattern("[GCODE]BED_MESH_CLEAR", result));
    }

    SECTION("Our own pre-start command echo announces no phase") {
        // The bed_mesh pre-print option sends BED_MESH_CALIBRATE_START_PRINT
        // ahead of START_PRINT, and the K2 echoes it back. It carries
        // BED_TEMP, EXTRUDER_TEMP and a BED_MESH_CALIBRATE prefix, so a
        // pattern list keyed on those bare tokens enters three phases before
        // the printer has done anything. apply_profile_match() only fires
        // once per phase, so the premature entry also consumes the real one,
        // pins phase_enter_times_ for the history, and sets real_signal_seen_.
        REQUIRE_FALSE(profile->try_match_pattern(
            "[GCODE]BED_MESH_CALIBRATE_START_PRINT GCODE_FILE='bench.gcode' "
            "BED_TEMP=60 EXTRUDER_TEMP=220",
            result));
    }

    SECTION("Real bed heat still announces HEATING_BED") {
        REQUIRE(profile->try_match_pattern("[GCODE]M190 S60", result));
        REQUIRE(result.phase == PrintStartPhase::HEATING_BED);
    }

    SECTION("Real mesh calibrate still announces BED_MESH") {
        REQUIRE(profile->try_match_pattern("[GCODE]BED_MESH_CALIBRATE", result));
        REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    }

    SECTION("Adaptive mesh calibrate still announces BED_MESH") {
        REQUIRE(profile->try_match_pattern("[GCODE]BED_MESH_CALIBRATE PROFILE=adaptive ADAPTIVE=1",
                                           result));
        REQUIRE(result.phase == PrintStartPhase::BED_MESH);
    }

    SECTION("Position inference enabled") {
        // Captured starts with [GCODE] echo disabled narrate nothing at all;
        // the silent window needs the position classifier.
        REQUIRE(profile->position_signals());
    }
}
