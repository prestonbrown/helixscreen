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

/// A Forge-X-shaped mod_params frame.
json forge_x_frame(const json& z_offset) {
    return json{{"mod_params", json{{"variables", json{{"z_offset", z_offset}}}}}};
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

TEST_CASE("z-offset persistence: Forge-X is detected by its macro", "[zoffset][persistence]") {
    // The Adventurer 5M/Pro mod registers SET_MOD and mirrors its store into
    // the mod_params status object; its migration path removes SAVE_ZMOD_DATA,
    // so the two signatures do not coexist on a healthy install.
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});

    CHECK(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(contains(helix::zoffset::required_status_objects(hw), "mod_params"));
    CHECK(helix::zoffset::persistence_enable_gcode(hw) == "SET_MOD PARAM=\"load_zoffset\" VALUE=1");
    CHECK(helix::zoffset::persistence_provider_name(hw) == "Forge-X");
}

TEST_CASE("z-offset persistence: ZMOD wins when both macros are present",
          "[zoffset][persistence]") {
    // Table order is match priority. No real printer exposes both, but a
    // half-migrated box might, and it must resolve to exactly one row.
    PrinterDiscovery hw = printer_with_macros({"SET_MOD", "SAVE_ZMOD_DATA"});

    CHECK(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(helix::zoffset::persistence_provider_name(hw) == "ZMOD");
    CHECK(helix::zoffset::persistence_enable_gcode(hw) == "SAVE_ZMOD_DATA LOAD_ZOFFSET=1");
    CHECK(contains(helix::zoffset::required_status_objects(hw), "save_variables"));
    CHECK_FALSE(contains(helix::zoffset::required_status_objects(hw), "mod_params"));
}

TEST_CASE("z-offset persistence: a near-miss macro detects nothing", "[zoffset][persistence]") {
    // has_macro() is exact: the mod's own SET_MOD_PARAM shares the prefix and
    // must not trip the Forge-X row. Unknown firmware still means no provider,
    // no subscription, no enable command.
    PrinterDiscovery hw = printer_with_macros({"SET_MOD_PARAM"});

    CHECK_FALSE(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(helix::zoffset::required_status_objects(hw).empty());
    CHECK(helix::zoffset::persistence_enable_gcode(hw).empty());
    CHECK(helix::zoffset::persistence_provider_name(hw).empty());
}

// ============================================================================
// Clearing the stale probe delta before an idle adjustment
// ============================================================================

TEST_CASE("z-offset persistence: ZMOD's stale probe delta has a clear command",
          "[zoffset][persistence]") {
    // ZMOD persists every SET_GCODE_OFFSET as `z - _TEST_POINT.temp_z_offset`,
    // where temp_z_offset holds the last print-start probe delta. That variable
    // survives END_PRINT/CANCEL_PRINT, so an idle adjustment stores the intended
    // value minus a stale delta (ghzserg/zmod#699). The fix is to zero it right
    // before the adjustment goes out.
    PrinterDiscovery hw = printer_with_macros({"SAVE_ZMOD_DATA"});

    CHECK(helix::zoffset::stale_probe_delta_clear_gcode(hw) ==
          "SET_GCODE_VARIABLE MACRO=_TEST_POINT VARIABLE=temp_z_offset VALUE=0");
}

TEST_CASE("z-offset persistence: no stale-delta clear without the mechanism",
          "[zoffset][persistence]") {
    // Forge-X keeps no probe-delta transient, and a plain printer has no
    // firmware override at all - either way there is nothing to clear, and an
    // empty string means "send nothing", not "send an empty line".
    CHECK(helix::zoffset::stale_probe_delta_clear_gcode(printer_with_macros({"SET_MOD"})).empty());
    CHECK(helix::zoffset::stale_probe_delta_clear_gcode(
              printer_with_macros({"START_PRINT", "END_PRINT"}))
              .empty());
    CHECK(helix::zoffset::stale_probe_delta_clear_gcode(printer_with_macros({})).empty());
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

TEST_CASE("z-offset persistence: Forge-X reads z_offset as microns", "[zoffset][persistence]") {
    using helix::zoffset::read_persisted_offset_microns;

    auto result = read_persisted_offset_microns(forge_x_frame(-0.15));
    REQUIRE(result.has_value());
    CHECK(*result == -150);

    // Stored zero is present, not absent - same contract as ZMOD.
    auto zero = read_persisted_offset_microns(forge_x_frame(0.0));
    REQUIRE(zero.has_value());
    CHECK(*zero == 0);

    // The stored value accumulates relative deltas here too, so round rather
    // than truncate.
    auto accumulated = read_persisted_offset_microns(forge_x_frame(-0.1499999));
    REQUIRE(accumulated.has_value());
    CHECK(*accumulated == -150);
}

TEST_CASE("z-offset persistence: Forge-X frames without the value mean no news",
          "[zoffset][persistence]") {
    using helix::zoffset::read_persisted_offset_microns;

    // variables carries every mod param, so sibling keys are the norm.
    CHECK_FALSE(read_persisted_offset_microns(
                    json{{"mod_params", json{{"variables", json{{"load_zoffset", 1}}}}}})
                    .has_value());
    // Not written until the first SET_GCODE_OFFSET Z=.
    CHECK_FALSE(read_persisted_offset_microns(forge_x_frame(nullptr)).has_value());
    // Stored as a string rather than a number.
    CHECK_FALSE(read_persisted_offset_microns(forge_x_frame("-0.15")).has_value());
    // Subscribed but variables not delivered yet.
    CHECK_FALSE(read_persisted_offset_microns(json{{"mod_params", json::object()}}).has_value());
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

// ============================================================================
// Helper-Script save-zoffset (prestonbrown/helixscreen#1401)
// ============================================================================

TEST_CASE("z-offset persistence: Helper-Script's wrapper is detected by the renamed original",
          "[zoffset][persistence][1401]") {
    // save-zoffset.cfg wraps SET_GCODE_OFFSET with rename_existing, which is
    // what makes `gcode_macro _SET_GCODE_OFFSET` exist at all - stock Klipper
    // has no such object. Keying on the rename rather than on the wrapper name
    // keeps a custom wrapper with a different rename out.
    PrinterDiscovery hw = printer_with_macros({"_SET_GCODE_OFFSET"});

    CHECK(helix::zoffset::firmware_persists_z_offset(hw));
    CHECK(helix::zoffset::persistence_provider_name(hw) == "Helper-Script");
    CHECK(contains(helix::zoffset::required_status_objects(hw), "save_variables"));
    // The module loads at boot via its own delayed_gcode; there is nothing to
    // enable and no stale probe-delta variable to clear.
    CHECK(helix::zoffset::persistence_enable_gcode(hw).empty());
    CHECK(helix::zoffset::stale_probe_delta_clear_gcode(hw).empty());
}

TEST_CASE("z-offset persistence: the Helper-Script row reads the zoffset save-variable",
          "[zoffset][persistence][1401]") {
    // SAVE_VARIABLE VARIABLE=zoffset VALUE="{'z': -0.475}" lands in Moonraker's
    // status as save_variables.variables.zoffset = {"z": -0.475}.
    json frame =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", -0.475}}}}}}}};
    auto microns = helix::zoffset::read_persisted_offset_microns(frame);
    REQUIRE(microns.has_value());
    CHECK(*microns == -475);

    // Seeded as {'z': None} before the first wrapped SET_GCODE_OFFSET.
    json null_z =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", nullptr}}}}}}}};
    CHECK_FALSE(helix::zoffset::read_persisted_offset_microns(null_z).has_value());

    // A save_variables store without the module's variable is some other
    // consumer's data - not ours to read.
    json other = json{{"save_variables", json{{"variables", json{{"lan_clients", 7}}}}}};
    CHECK_FALSE(helix::zoffset::read_persisted_offset_microns(other).has_value());
}

TEST_CASE("z-offset persistence: ZMOD outranks the Helper-Script wrapper",
          "[zoffset][persistence][1401]") {
    // A box carrying both rows' macros resolves to the first hit in the table;
    // the schemas are distinct so reads never collide either.
    PrinterDiscovery hw = printer_with_macros({"SAVE_ZMOD_DATA", "_SET_GCODE_OFFSET"});
    CHECK(helix::zoffset::persistence_provider_name(hw) == "ZMOD");
}
