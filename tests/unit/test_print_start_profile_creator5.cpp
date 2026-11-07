// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/preprint_config_scope.h"
#include "../test_helpers/print_start_collector_fixture.h"
#include "print_start_profile.h"
#include "printer_detector.h"
#include "simulated_clock.h"
#include "thermal_rate_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// FlashForge Creator 5 Profile Tests
//
// Console lines are verbatim from Moonraker's gcode_store on a Creator 5 Pro
// running Z-Mod 1.7.5 (2026-11-06): one start with a saved mesh, one that built
// a full 10x10 mesh. The whole starts replay through the collector at the end of
// this file (tests/fixtures/c5_zmod_*_start.txt).
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

// ============================================================================
// FlashForge Creator 5 on Z-Mod: real pre-print replays
//
// A Creator 5 Pro on Z-Mod 1.7.5 (2026-11-06), from the answer to Z-Mod's colour
// prompt (t=0) to "START_PRINT has finished its work.":
//   c5_zmod_saved_mesh_start.txt - loads the saved mesh, then waits about 200s
//     for the bed to cool from the +10C it held during the tool pickup.
//   c5_zmod_full_mesh_start.txt - cools the bed for probing, docks the head and
//     probes a 10x10 mesh three samples a point, then picks the tool up again.
// print_stats leaves standby only when the file opens, 80-90s in, which is
// where the collector arms.
// ============================================================================

class CreatorFivePrintStartReplayFixture : public PrintStartCollectorHeaterFixture {
  public:
    struct Result {
        std::string trace; ///< "second:PHASE:message" at each change, mesh counts left out
        int armed_at_ms{-1};
        int completed_at_ms{-1};
        std::string last_mesh_message;
    };

    CreatorFivePrintStartReplayFixture() {
        ThermalRateManager::instance().reset();
        auto profile = PrintStartProfile::load("creator5");
        have_profile_ = !profile->is_default();
        collector().set_profile(std::move(profile));
        // The 10x10 bed_mesh probe_count the Creator 5 ships with; no stored mesh.
        client().set_config_bed_mesh_probe_count(10, 10);
        client().gcode_script("BED_MESH_CLEAR");
    }

    ~CreatorFivePrintStartReplayFixture() override {
        ThermalRateManager::instance().reset();
    }

    bool have_profile_ = false;

    /// @param armed_from_commit arm at t=0, as a print this app starts does
    ///        (the preparing job arms the collector before the printer has a job);
    ///        otherwise arm where print_stats takes the file.
    Result replay(const std::string& fixture, bool armed_from_commit = false) {
        Result result;
        std::ifstream f(collector_fixture_dir() + fixture);
        REQUIRE(f.good());
        int now_ms = 0;
        bool first = true;
        PrintStartPhase shown = PrintStartPhase::IDLE;
        std::string shown_message;
        const auto note = [&]() {
            drain_async_updates();
            drain_async_updates();
            const PrintStartPhase phase = get_current_phase();
            const std::string message = get_current_message();
            const bool counting = message.rfind("Bed Mesh (", 0) == 0;
            if (counting) {
                result.last_mesh_message = message;
            }
            if (phase == shown && (message == shown_message || counting)) {
                return;
            }
            shown = phase;
            shown_message = message;
            if (!result.trace.empty()) {
                result.trace += '\n';
            }
            result.trace += std::to_string(now_ms / 1000) + ":" +
                            std::string(print_start_phase_name(phase)) + ":" + message;
            if (phase == PrintStartPhase::COMPLETE && result.completed_at_ms < 0) {
                result.completed_at_ms = now_ms;
            }
        };
        const auto arm = [&]() {
            if (result.armed_at_ms >= 0) {
                return;
            }
            result.armed_at_ms = now_ms;
            collector().start();
            collector().enable_fallbacks();
            note();
        };

        for (std::string raw; std::getline(f, raw);) {
            if (raw.empty() || raw[0] == '#') {
                continue;
            }
            const size_t sp = raw.find(' ');
            const int t_ms = static_cast<int>(std::lround(std::stod(raw.substr(0, sp)) * 1000.0));
            const char kind = raw[sp + 1];
            const std::string body = raw.substr(sp + 3);
            if (first) {
                now_ms = t_ms;
                first = false;
            }
            clock_.advance(std::chrono::milliseconds(std::max(0, t_ms - now_ms)));
            now_ms = std::max(now_ms, t_ms);
            if (armed_from_commit && t_ms >= 0) {
                arm();
            }

            if (kind == '>') {
                std::string line;
                for (size_t i = 0; i < body.size(); ++i) {
                    if (body[i] == '\\' && i + 1 < body.size()) {
                        line += body[i + 1] == 'n' ? '\n' : body[i + 1];
                        ++i;
                    } else {
                        line += body[i];
                    }
                }
                if (line.rfind("File opened:", 0) == 0) {
                    arm();
                }
                send_gcode_response(line);
            } else {
                char ps[16] = {};
                float bed = 0, bed_target = 0, ext = 0, ext_target = 0;
                REQUIRE(std::sscanf(body.c_str(), "%15s %f %f %f %f", ps, &bed, &bed_target, &ext,
                                    &ext_target) == 5);
                set_all_temps(static_cast<int>(std::lround(bed * 10)),
                              static_cast<int>(std::lround(bed_target * 10)),
                              static_cast<int>(std::lround(ext * 10)),
                              static_cast<int>(std::lround(ext_target * 10)));
                if (std::string(ps) == "printing") {
                    arm();
                }
                if (result.armed_at_ms >= 0) {
                    collector().check_fallback_completion();
                }
            }
            if (result.armed_at_ms >= 0) {
                note();
            }
        }
        return result;
    }

  private:
    PreprintConfigScope config_{"FlashForge Creator 5 Pro"};
    helix::sim::SimulatedClock::ManualScope clock_{helix::sim::SimSpeed::of(1.0)};
};

TEST_CASE_METHOD(CreatorFivePrintStartReplayFixture,
                 "PrintStartCollector: a Creator 5 Z-Mod start with a saved mesh",
                 "[print][collector][creator5][integration]") {
    REQUIRE(have_profile_);
    const Result result = replay("c5_zmod_saved_mesh_start.txt");
    CAPTURE(result.trace);
    // Armed when print_stats takes the file; the bed then cools from 66.7C into
    // Z-Mod's 52-58C window before anything is purged, and only the purge itself
    // (not the "Priming algorithm: LINE_PURGE" setting at 79s) shows Purging.
    REQUIRE(result.armed_at_ms == 77809);
    REQUIRE(result.trace == "77:INITIALIZING:Preparing Print...\n"
                            "78:INITIALIZING:Starting Print...\n"
                            "80:HEATING_BED:Waiting for bed temperature...\n"
                            "279:PURGING:Purging...\n"
                            "284:COMPLETE:Starting Print...");
    REQUIRE(result.completed_at_ms == 284133);
}

TEST_CASE_METHOD(CreatorFivePrintStartReplayFixture,
                 "PrintStartCollector: a Creator 5 Z-Mod start that builds a full mesh",
                 "[print][collector][creator5][integration]") {
    REQUIRE(have_profile_);
    const Result result = replay("c5_zmod_full_mesh_start.txt");
    CAPTURE(result.trace);
    REQUIRE(result.trace == "89:INITIALIZING:Preparing Print...\n"
                            "90:INITIALIZING:Starting Print...\n"
                            "91:HEATING_BED:Waiting for bed temperature...\n"
                            "264:INITIALIZING:Docking tool\n"
                            "296:BED_MESH:Bed Mesh (5/100)\n"
                            "912:HEATING_NOZZLE:Heating Nozzle...\n"
                            "933:INITIALIZING:Picking up tool\n"
                            "941:HEATING_BED:Waiting for bed temperature...\n"
                            "941:PURGING:Purging...\n"
                            "946:COMPLETE:Starting Print...");
    // The mesh shows once five distinct points confirm a sweep (15 probe lines,
    // three samples a point), crediting those five; every later point counts
    // once against the grid's own total.
    REQUIRE(result.last_mesh_message == "Bed Mesh (100/100)");
}

TEST_CASE_METHOD(CreatorFivePrintStartReplayFixture,
                 "PrintStartCollector: a Creator 5 Z-Mod start armed before the file opens",
                 "[print][collector][creator5][integration]") {
    REQUIRE(have_profile_);
    // A print started from this app arms at commit, so the collector sees Z-Mod's
    // preparation that runs before print_stats takes the file: the colour table,
    // the tool pickup and the two heater normalization waits.
    const Result result = replay("c5_zmod_saved_mesh_start.txt", /*armed_from_commit=*/true);
    CAPTURE(result.trace);
    REQUIRE(result.armed_at_ms == 50);
    REQUIRE(result.trace == "0:INITIALIZING:Preparing Print...\n"
                            "0:INITIALIZING:Preparing...\n"
                            "45:INITIALIZING:Picking up tool\n"
                            "51:HEATING_NOZZLE:Heating Nozzle...\n"
                            "75:HEATING_BED:Heating Bed...\n"
                            "78:INITIALIZING:Starting Print...\n"
                            "80:HEATING_BED:Waiting for bed temperature...\n"
                            "279:PURGING:Purging...\n"
                            "284:COMPLETE:Starting Print...");
    REQUIRE(result.completed_at_ms == 284133);
}
