// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_profile.h"
#include "printer_detector.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// FlashForge Creator 5 Profile Tests
//
// Console lines are verbatim from Moonraker's gcode_store on a Creator 5 Pro
// running Z-Mod 1.7.5 (2026-11-06): one start with a saved mesh, one that built
// a full 10x10 mesh. The whole starts replay through the collector in
// test_print_start_collector.cpp (tests/fixtures/c5_zmod_*_start.txt).
// ============================================================================

namespace {

std::shared_ptr<PrintStartProfile> get_creator5_profile() {
    return PrintStartProfile::load("creator5");
}

/// The phase and message a console line maps to, by the matcher the collector
/// tries first: tags, then the response patterns.
struct Mapped {
    bool matched = false;
    bool tag = false;
    PrintStartPhase phase = PrintStartPhase::IDLE;
    std::string message;
};

Mapped map_line(const PrintStartProfile& profile, const std::string& line) {
    PrintStartProfile::MatchResult result;
    if (profile.try_match_tag(line, result)) {
        return {true, true, result.phase, result.message};
    }
    if (profile.try_match_pattern(line, result)) {
        return {true, false, result.phase, result.message};
    }
    return {};
}

} // namespace

TEST_CASE("PrintStartProfile: creator5 maps Z-Mod's START_PRINT narration",
          "[profile][print][creator5]") {
    auto profile = get_creator5_profile();
    REQUIRE(profile != nullptr);
    REQUIRE_FALSE(profile->is_default());
    REQUIRE(profile->name().find("Creator 5") != std::string::npos);

    struct Case {
        const char* line;
        PrintStartPhase phase;
        const char* message;
    };
    // clang-format off
    const Case cases[] = {
        {"// T0 (PLA):\n// temp (auto purge): 220\xC2\xB0" "C\n// temp_wait:         120\xC2\xB0" "C\n"
         "// drop_length:       50 mm",                                            PrintStartPhase::INITIALIZING,   "Preparing..."},
        {"// Grab attempt for T0: 1 of 5...",                                      PrintStartPhase::INITIALIZING,   "Picking up tool"},
        {"// Unload attempt for T0: 1 of 5...",                                    PrintStartPhase::INITIALIZING,   "Docking tool"},
        {"// Waiting for temperature normalization. Extruder: 218.0-222.0 T0 _T_PREPARE",
                                                                                   PrintStartPhase::HEATING_NOZZLE, "Heating Nozzle..."},
        {"// Waiting for temperature normalization. Extruder: 218.0-222.0 T0 _T_RESTORE",
                                                                                   PrintStartPhase::HEATING_NOZZLE, "Heating Nozzle..."},
        {"// Waiting for temperature normalization. Bed: 62.0-68.0. T0: 218.0-222.0 _T_PREPARE",
                                                                                   PrintStartPhase::HEATING_BED,    "Heating Bed..."},
        {"// Waiting for temperature normalization. Bed: 52.0-58.0. T0: 218.0-222.0 START_PRINT",
                                                                                   PrintStartPhase::HEATING_BED,    "Waiting for bed temperature..."},
        {"info Bed calibration. Profile auto. Extruder temp: 220.0 / Bed temp: 55.0",
                                                                                   PrintStartPhase::HEATING_BED,    "Waiting for bed temperature..."},
        {"info Nozzle line purge. Algorithm: LINE_PURGE",                          PrintStartPhase::PURGING,        "Purging..."},
        {"// KAMP purge starts at 113.0, 78.0 and purges 30.0mm of filament, required flow rate is 12.0mm\xC2\xB3/s",
                                                                                   PrintStartPhase::PURGING,        "Purging..."},
        {"echo: START_PRINT has finished its work.",                               PrintStartPhase::COMPLETE,       "Starting Print..."},
    };
    // clang-format on
    for (const Case& c : cases) {
        CAPTURE(c.line);
        const Mapped m = map_line(*profile, c.line);
        REQUIRE(m.matched);
        // Z-Mod's order runs backwards through the phase enum (the bed cools
        // after the mesh), which only a tag may display.
        CHECK(m.tag);
        CHECK(m.phase == c.phase);
        CHECK(m.message == c.message);
    }
}

TEST_CASE("PrintStartProfile: creator5 reads Z-Mod's START_PRINT settings banner as no step",
          "[profile][print][creator5]") {
    auto profile = get_creator5_profile();
    REQUIRE(profile != nullptr);

    // Settings and state reports printed while nothing of the kind runs: the
    // purge algorithm is announced ~200s before the purge, and a stored mesh
    // load takes a fraction of a second before the bed's minutes-long cool.
    const char* const lines[] = {
        "info Priming algorithm: LINE_PURGE",
        "info Nozzle priming: enabled",
        "info Load bed mesh: auto",
        "info Trying to load bed mesh: auto",
        "info BED_MESH_PROFILE LOAD=auto FROM=_START_PRINT // _START_PRINT",
        "info Bed mesh auto loaded",
        "// No need for leveling. Loaded profile: auto",
        "info Printing with bed mesh: auto",
        "info Bed temperature: 55.0",
        "info Extruder temperature: 220.0",
        "// Building bed mesh on each print. // SAVE_ZMOD_DATA PRINT_LEVELING=1",
        "info BED_MESH_CLEAR // _START_PRINT",
        "// Head: -1",
        "// Head: T0",
        "// Fast Smart Park",
        "// Temperature normalization completed T0 START_PRINT",
        "// If it takes a long time, calibrate the PID of the extruder and bed T0 START_PRINT",
        "info END: Nozzle line purge.",
        "// action:prompt_end",
    };
    for (const char* line : lines) {
        CAPTURE(line);
        CHECK_FALSE(map_line(*profile, line).matched);
    }
}

TEST_CASE(
    "PrintStartProfile: creator5 keeps the generic patterns for the Creator 5's other firmwares",
    "[profile][print][creator5]") {
    auto profile = get_creator5_profile();
    auto generic = PrintStartProfile::load_default();
    REQUIRE(profile != nullptr);
    REQUIRE(generic != nullptr);

    // Narration a stock or OpenCreator start may print, none of which Z-Mod's
    // START_PRINT prints: each maps as it does with the generic profile.
    const char* const lines[] = {
        "G28",
        "M190 S60",
        "Bed: 60 C",
        "Heat soaking for 5 minutes",
        "M109 S220",
        "Nozzle: 220 C",
        "QUAD_GANTRY_LEVEL",
        "Z_TILT_ADJUST",
        "BED_MESH_CALIBRATE",
        "Loading bed mesh",
        "CLEAN_NOZZLE",
        "NOZZLE_CLEAR",
        "VORON_PURGE",
    };
    for (const char* line : lines) {
        CAPTURE(line);
        PrintStartProfile::MatchResult expected;
        REQUIRE(generic->try_match_pattern(line, expected));
        const Mapped m = map_line(*profile, line);
        REQUIRE(m.matched);
        CHECK(m.phase == expected.phase);
        CHECK(m.message == expected.message);
    }
}

TEST_CASE("PrinterDetector: both Creator 5 models use the creator5 pre-print profile",
          "[printer][preprint][creator5]") {
    REQUIRE(PrinterDetector::get_print_start_profile("FlashForge Creator 5") == "creator5");
    REQUIRE(PrinterDetector::get_print_start_profile("FlashForge Creator 5 Pro") == "creator5");
}

TEST_CASE("PrintStartProfile: creator5 announces a start with Z-Mod's per-tool table",
          "[profile][print][creator5][2054]") {
    auto profile = get_creator5_profile();
    auto generic = PrintStartProfile::load_default();
    REQUIRE(profile != nullptr);
    REQUIRE(generic != nullptr);

    // PRINT_ZCOLOR prints the table once the colour prompt is answered, 80-90s
    // before print_stats takes the file.
    const std::string table = "// T0 (PLA):\n// temp (auto purge): 220\xC2\xB0"
                              "C\n// temp_wait:         120\xC2\xB0"
                              "C\n// drop_length:       50 mm";
    CHECK(profile->announces_print_start(table));
    CHECK_FALSE(generic->announces_print_start(table));

    // A tool change by hand, a prompt closing and a heater wait name no start.
    const char* const lines[] = {
        "// Grab attempt for T0: 1 of 5...",
        "// Unload attempt for T1: 1 of 5...",
        "// action:prompt_end",
        "// Head: -1",
        "// Waiting for temperature normalization. Extruder: 218.0-222.0 T0 _T_PREPARE",
        "echo: START_PRINT has finished its work.",
    };
    for (const char* line : lines) {
        CAPTURE(line);
        CHECK_FALSE(profile->announces_print_start(line));
    }
}
