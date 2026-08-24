// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_z_offset_persistence.cpp
 * @brief Tests for the firmware-persisted z-offset abstraction.
 *
 * Some firmware keeps the authoritative z-offset outside Klipper's gcode_move
 * and zeroes the live offset outside a print. helix::zoffset owns which
 * firmwares those are, what to subscribe, how to read the value, and how to turn
 * persistence on. Generic code asks these questions and never names a firmware,
 * so these tests are also the guard on that boundary: they exercise the
 * capability API, and reach a vendor only through a printer's macro list.
 */

#include "printer_discovery.h"
#include "z_offset_persistence.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterDiscovery;
using nlohmann::json;

namespace {

/// A printer whose objects list carries the given gcode macros.
PrinterDiscovery printer_with_macros(std::initializer_list<const char*> macros) {
    json objects = json::array();
    objects.push_back("gcode_move");
    objects.push_back("toolhead");
    for (const char* m : macros) {
        objects.push_back(std::string("gcode_macro ") + m);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

/// A ZMOD-shaped save_variables frame.
json zmod_frame(const json& gcode_offsets) {
    return json{{"save_variables", json{{"variables", json{{"gcode_offsets", gcode_offsets}}}}}};
}

} // namespace

// ============================================================================
// Detection and the capability questions
// ============================================================================

TEST_CASE("z-offset persistence: a plain printer needs nothing", "[zoffset][persistence]") {
    // The overwhelming majority: the live gcode_move offset IS the truth, so no
    // extra subscription, no enable command, and the row must not switch source.
    PrinterDiscovery hw = printer_with_macros({"START_PRINT", "END_PRINT"});

    CHECK_FALSE(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(helix::zoffset::required_status_objects(hw).empty());
    CHECK(helix::zoffset::persistence_enable_gcode(hw).empty());
    CHECK(helix::zoffset::persistence_provider_name(hw).empty());
}

TEST_CASE("z-offset persistence: ZMOD is detected by its macro", "[zoffset][persistence]") {
    PrinterDiscovery hw = printer_with_macros({"SAVE_ZMOD_DATA"});

    CHECK(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(contains(helix::zoffset::required_status_objects(hw), "save_variables"));
    CHECK(helix::zoffset::persistence_enable_gcode(hw) == "SAVE_ZMOD_DATA LOAD_ZOFFSET=1");
    CHECK(helix::zoffset::persistence_provider_name(hw) == "ZMOD");
}

TEST_CASE("z-offset persistence: detection is case-insensitive", "[zoffset][persistence]") {
    // PrinterDiscovery upper-cases macro names; the provider table must not
    // depend on how the firmware happened to spell it.
    PrinterDiscovery hw = printer_with_macros({"save_zmod_data"});

    CHECK(helix::zoffset::firmware_persists_z_offset(hw));
}

// ============================================================================
// Reading the stored value
// ============================================================================

TEST_CASE("z-offset persistence: reads the stored offset as microns", "[zoffset][persistence]") {
    auto result = helix::zoffset::read_persisted_offset_microns(zmod_frame(json{{"z", -0.15}}));
    REQUIRE(result.has_value());
    CHECK(*result == -150);
}

TEST_CASE("z-offset persistence: a stored zero is present, not absent", "[zoffset][persistence]") {
    // Distinct from "nothing stored": a user who dialed back to 0 must not be
    // shown a stale non-zero value.
    auto result = helix::zoffset::read_persisted_offset_microns(zmod_frame(json{{"z", 0.0}}));
    REQUIRE(result.has_value());
    CHECK(*result == 0);
}

TEST_CASE("z-offset persistence: rounds to the nearest micron", "[zoffset][persistence]") {
    // The stored value accumulates relative deltas, so a nominal -0.150 arrives
    // as -0.1499999; truncation would strand the display a micron low.
    auto lo = helix::zoffset::read_persisted_offset_microns(zmod_frame(json{{"z", -0.1499999}}));
    REQUIRE(lo.has_value());
    CHECK(*lo == -150);

    auto hi = helix::zoffset::read_persisted_offset_microns(zmod_frame(json{{"z", 0.0255}}));
    REQUIRE(hi.has_value());
    CHECK(*hi == 26);
}

TEST_CASE("z-offset persistence: tolerates sibling axes", "[zoffset][persistence]") {
    // ZMOD's LOAD_GCODE_OFFSET iterates the dict, so more than z is legal.
    auto result = helix::zoffset::read_persisted_offset_microns(
        zmod_frame(json{{"x", 0.1}, {"y", 0.2}, {"z", -0.075}}));
    REQUIRE(result.has_value());
    CHECK(*result == -75);
}

TEST_CASE("z-offset persistence: absent reads mean no news, in every shape",
          "[zoffset][persistence]") {
    using helix::zoffset::read_persisted_offset_microns;

    // Seeded as {'z': None} before the firmware's first save.
    CHECK_FALSE(read_persisted_offset_microns(zmod_frame(json{{"z", nullptr}})).has_value());
    // save_variables present but carrying someone else's keys (AD5X IFS, QIDI Box).
    CHECK_FALSE(read_persisted_offset_microns(
                    json{{"save_variables", json{{"variables", json{{"bambufy_colors", 1}}}}}})
                    .has_value());
    // Subscribed but variables not delivered yet.
    CHECK_FALSE(
        read_persisted_offset_microns(json{{"save_variables", json::object()}}).has_value());
    // A frame about something else entirely — the delta-only case that must not
    // be mistaken for "cleared".
    CHECK_FALSE(read_persisted_offset_microns(json{{"gcode_move", json::object()}}).has_value());
    // Degenerate inputs must not throw.
    CHECK_FALSE(read_persisted_offset_microns(json::object()).has_value());
    CHECK_FALSE(read_persisted_offset_microns(json()).has_value());
}

TEST_CASE("z-offset persistence: rejects malformed stored values", "[zoffset][persistence]") {
    using helix::zoffset::read_persisted_offset_microns;

    // Stored as a string rather than a number.
    CHECK_FALSE(read_persisted_offset_microns(zmod_frame(json{{"z", "-0.15"}})).has_value());
    // gcode_offsets not an object.
    CHECK_FALSE(read_persisted_offset_microns(
                    json{{"save_variables", json{{"variables", json{{"gcode_offsets", -0.15}}}}}})
                    .has_value());
    // variables not an object.
    CHECK_FALSE(read_persisted_offset_microns(json{{"save_variables", json{{"variables", "nope"}}}})
                    .has_value());
}

// ============================================================================
// The enable gate
// ============================================================================

TEST_CASE("z-offset persistence: enable gate requires a firmware that needs it",
          "[zoffset][persistence]") {
    using helix::zoffset::should_enable_persistence;

    CHECK_FALSE(should_enable_persistence(false, false, false));
    CHECK_FALSE(should_enable_persistence(false, true, false));
    CHECK_FALSE(should_enable_persistence(false, false, true));
}

TEST_CASE("z-offset persistence: enable gate fires once, while idle", "[zoffset][persistence]") {
    using helix::zoffset::should_enable_persistence;

    CHECK(should_enable_persistence(true, false, false));
    // Never mid-print: this injects gcode into a running job.
    CHECK_FALSE(should_enable_persistence(true, true, false));
    // Never twice in a session.
    CHECK_FALSE(should_enable_persistence(true, false, true));
}
