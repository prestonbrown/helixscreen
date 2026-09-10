// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_start_phase_names.cpp
 * @brief The canonical PrintStartPhase <-> name mapping.
 *
 * The name is the persisted form: each history entry's "phases" object is keyed
 * by it, so a phase without a name would silently drop out of the prediction
 * history, and two phases sharing a name would merge two durations into one.
 * The round trip below walks every declared value, so a phase added later is
 * covered without anyone remembering to extend this file.
 *
 * The alias table is an external contract: PRINT_START macros emit
 * "HELIX:PHASE:<name>" and printer profiles name a phase in JSON, both of which
 * ship on machines this build never sees. An alias that stops resolving is a
 * silent loss of phase detection on those printers.
 */

#include "print_start_phase.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::print_start_phase_count;
using helix::print_start_phase_from_name;
using helix::print_start_phase_name;
using helix::print_start_phase_stores_duration;
using helix::PrintStartPhase;

TEST_CASE("PrintStartPhase: every declared phase has a unique, round-tripping name",
          "[print][phase][names]") {
    std::set<std::string> seen;

    for (int i = 0; i < print_start_phase_count(); ++i) {
        const auto phase = static_cast<PrintStartPhase>(i);
        CAPTURE(i);

        const auto name = print_start_phase_name(phase);
        REQUIRE_FALSE(name.empty());

        // Names key a JSON object; a duplicate would collapse two phases into
        // one entry, and the loser's duration would be lost on every save.
        CHECK(seen.insert(std::string(name)).second);

        const auto parsed = print_start_phase_from_name(name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == phase);
    }
}

TEST_CASE("PrintStartPhase: SOAKING sits between bed heat and nozzle heat",
          "[print][phase][names]") {
    // The numeric order gates silent progression (a phase never moves backward),
    // so the soak dwell has to be declared where it happens: after the bed is
    // commanded, before the nozzle comes up to temperature.
    CHECK(static_cast<int>(PrintStartPhase::HEATING_BED) <
          static_cast<int>(PrintStartPhase::SOAKING));
    CHECK(static_cast<int>(PrintStartPhase::SOAKING) <
          static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    CHECK(print_start_phase_name(PrintStartPhase::SOAKING) == "SOAKING");
}

TEST_CASE("PrintStartPhase: COMPLETE is the last declared phase", "[print][phase][names]") {
    // print_start_phase_count() derives the range from COMPLETE, and the
    // round-trip test walks that range. A phase declared past COMPLETE would
    // fall outside both.
    CHECK(static_cast<int>(PrintStartPhase::COMPLETE) == print_start_phase_count() - 1);
}

TEST_CASE("PrintStartPhase: the HELIX:PHASE aliases resolve", "[print][phase][names]") {
    struct Case {
        const char* name;
        PrintStartPhase expected;
    };
    const Case cases[] = {
        {"STARTING", PrintStartPhase::INITIALIZING},
        {"START", PrintStartPhase::INITIALIZING},
        {"DONE", PrintStartPhase::COMPLETE},
        {"BED_HEATING", PrintStartPhase::HEATING_BED},
        {"NOZZLE_HEATING", PrintStartPhase::HEATING_NOZZLE},
        {"HEATING_HOTEND", PrintStartPhase::HEATING_NOZZLE},
        {"QUAD_GANTRY_LEVEL", PrintStartPhase::QGL},
        {"Z_TILT_ADJUST", PrintStartPhase::Z_TILT},
        {"BED_LEVELING", PrintStartPhase::BED_MESH},
        {"NOZZLE_CLEAN", PrintStartPhase::CLEANING},
        {"PURGE", PrintStartPhase::PURGING},
        {"PRIMING", PrintStartPhase::PURGING},
        {"HEAT_SOAK", PrintStartPhase::SOAKING},
        {"SOAK", PrintStartPhase::SOAKING},
    };

    for (const auto& c : cases) {
        CAPTURE(c.name);
        const auto parsed = print_start_phase_from_name(c.name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == c.expected);
    }
}

TEST_CASE("PrintStartPhase: an unrecognised name yields no phase", "[print][phase][names]") {
    // Falling back to IDLE would look like a successful parse and reset a print
    // that is already underway.
    CHECK_FALSE(print_start_phase_from_name("").has_value());
    CHECK_FALSE(print_start_phase_from_name("NOT_A_PHASE").has_value());
    CHECK_FALSE(print_start_phase_from_name("homing").has_value());
    // The persisted form is a name; an ordinal reaching the parser means a
    // document that never went through the migration.
    CHECK_FALSE(print_start_phase_from_name("5").has_value());
}

TEST_CASE("PrintStartPhase: the prediction history stores dwell phases only",
          "[print][phase][names]") {
    // Heating is predicted from measured heat rates by ThermalRateModel, and
    // IDLE/INITIALIZING/COMPLETE are transitions with no dwell of their own.
    CHECK(print_start_phase_stores_duration(PrintStartPhase::HOMING));
    CHECK(print_start_phase_stores_duration(PrintStartPhase::SOAKING));
    CHECK(print_start_phase_stores_duration(PrintStartPhase::QGL));
    CHECK(print_start_phase_stores_duration(PrintStartPhase::Z_TILT));
    CHECK(print_start_phase_stores_duration(PrintStartPhase::BED_MESH));
    CHECK(print_start_phase_stores_duration(PrintStartPhase::CLEANING));
    CHECK(print_start_phase_stores_duration(PrintStartPhase::PURGING));

    CHECK_FALSE(print_start_phase_stores_duration(PrintStartPhase::IDLE));
    CHECK_FALSE(print_start_phase_stores_duration(PrintStartPhase::INITIALIZING));
    CHECK_FALSE(print_start_phase_stores_duration(PrintStartPhase::HEATING_BED));
    CHECK_FALSE(print_start_phase_stores_duration(PrintStartPhase::HEATING_NOZZLE));
    CHECK_FALSE(print_start_phase_stores_duration(PrintStartPhase::COMPLETE));
}
