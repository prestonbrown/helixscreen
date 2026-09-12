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

#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "printer_discovery.h"
#include "z_offset_persistence.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Config;
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

/// A ZMOD save_variables frame carrying the persistence flag.
json zmod_enable_frame(const json& load_zoffset) {
    return json{{"save_variables", json{{"variables", json{{"load_zoffset", load_zoffset}}}}}};
}

/// A Forge-X mod_params frame carrying the persistence flag.
json forge_x_enable_frame(const json& load_zoffset) {
    return json{{"mod_params", json{{"variables", json{{"load_zoffset", load_zoffset}}}}}};
}

/// Spell a tri-state answer so a case fits on one line and a failure names the
/// state it got instead of printing an opaque optional.
std::string tri(const std::optional<bool>& v) {
    if (!v.has_value()) {
        return "unknown";
    }
    return *v ? "on" : "off";
}

/// The Config singleton, pointed at a named printer so df() routes the record
/// to a stable per-printer key. The suite's isolation listener has already
/// pointed the singleton at the sandbox and blanked its document, so every test
/// starts from a fresh install with no settings.json on disk.
Config* fresh_config(const char* printer_id = "zoffset_test_printer") {
    Config* cfg = Config::get_instance();
    REQUIRE(cfg != nullptr);
    helix::ConfigTestAccess::active_printer_id(*cfg) = printer_id;
    return cfg;
}

/// Replace the live document with whatever save() actually wrote to disk. A
/// record that only reached memory does not survive this.
void reload_from_disk(Config* cfg) {
    const std::string path = cfg->get_path();
    INFO("reading back " << path);
    std::ifstream in(path);
    REQUIRE(in.good());
    json saved = json::parse(in, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(saved.is_discarded());
    helix::ConfigTestAccess::data(*cfg) = saved;
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

TEST_CASE("z-offset persistence: read priority puts ZMOD above Helper-Script",
          "[zoffset][persistence]") {
    // Table order is load-bearing: ZMOD also wraps SET_GCODE_OFFSET, so a box
    // carrying BOTH schemas must resolve to ZMOD's value. The detection test
    // pins this order for match(); this pins it for the read path — a
    // reordering would return 9999 instead of -200.
    json frame =
        json{{"save_variables", json{{"variables", json{{"gcode_offsets", json{{"z", -0.20}}},
                                                        {"zoffset", json{{"z", 9.99}}}}}}}};
    auto result = helix::zoffset::read_persisted_offset_microns(frame);
    REQUIRE(result.has_value());
    CHECK(*result == -200);
}

TEST_CASE("z-offset persistence: a frame without the schema objects reads nothing",
          "[zoffset][persistence]") {
    // The subscription builder only subscribes save_variables / mod_params
    // when a provider matched, so this is the frame shape of the overwhelming
    // majority of printers — the fast path must answer nullopt, not probe.
    json frame = json{{"gcode_move", json{{"z_offset", -0.15}}}, {"toolhead", json{{"x", 1.0}}}};
    CHECK_FALSE(helix::zoffset::read_persisted_offset_microns(frame).has_value());
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

    CHECK_FALSE(should_enable_persistence(false, false, false, std::nullopt));
    CHECK_FALSE(should_enable_persistence(false, true, false, std::nullopt));
    CHECK_FALSE(should_enable_persistence(false, false, true, std::nullopt));
    CHECK_FALSE(should_enable_persistence(false, false, false, false));
}

TEST_CASE("z-offset persistence: enable gate fires once, while idle", "[zoffset][persistence]") {
    using helix::zoffset::should_enable_persistence;

    CHECK(should_enable_persistence(true, false, false, std::nullopt));
    // Never mid-print: this injects gcode into a running job.
    CHECK_FALSE(should_enable_persistence(true, true, false, std::nullopt));
    // Never again once this install has sent it. The flag is persisted, so a
    // user who turns the setting back off is not overruled at the next launch
    // (prestonbrown/helixscreen#1432).
    CHECK_FALSE(should_enable_persistence(true, false, true, std::nullopt));
}

TEST_CASE("z-offset persistence: enable gate stands down when the firmware already agrees",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::should_enable_persistence;

    // The write is persistent firmware state and costs the user the slicer's
    // per-print Z_OFFSET / SKIP_ZOFFSET parameters, so a firmware that already
    // holds the setting must be left alone.
    CHECK_FALSE(should_enable_persistence(true, false, false, true));
    // Off is the state the send exists for.
    CHECK(should_enable_persistence(true, false, false, false));
    // Unknown is not an off. Only a definite true suppresses.
    CHECK(should_enable_persistence(true, false, false, std::nullopt));

    // The other gates still bind regardless of what the firmware reports.
    CHECK_FALSE(should_enable_persistence(true, true, false, false));
    CHECK_FALSE(should_enable_persistence(true, false, true, false));
}

// ============================================================================
// Reading the firmware's current enable state (prestonbrown/helixscreen#1432)
// ============================================================================

TEST_CASE("z-offset persistence: ZMOD's enable flag comes from save_variables",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::persistence_already_enabled;
    PrinterDiscovery hw = printer_with_macros({"SAVE_ZMOD_DATA"});

    CHECK(tri(persistence_already_enabled(hw, zmod_enable_frame(1))) == "on");
    // A 0 on ZMOD is a deliberate user choice: GET_ZMOD_DATA writes the key
    // back as 1 by default at every Klipper start, so the setting can only be
    // off because somebody turned it off.
    CHECK(tri(persistence_already_enabled(hw, zmod_enable_frame(0))) == "off");

    // The key materializes about ten seconds into a Klipper session. Until it
    // does, the frame says nothing - reading that as an off would send the
    // enable gcode to a printer that never needed it.
    json no_key =
        json{{"save_variables", json{{"variables", json{{"gcode_offsets", json{{"z", -0.2}}}}}}}};
    CHECK(tri(persistence_already_enabled(hw, no_key)) == "unknown");
    // save_variables is delta-only, so most frames omit it entirely.
    CHECK(tri(persistence_already_enabled(hw, json{{"gcode_move", json{{"speed", 100.0}}}})) ==
          "unknown");
    CHECK(tri(persistence_already_enabled(hw, json{{"save_variables", json::object()}})) ==
          "unknown");
    // Degenerate inputs must not throw.
    CHECK(tri(persistence_already_enabled(hw, json::object())) == "unknown");
    CHECK(tri(persistence_already_enabled(hw, json())) == "unknown");
}

TEST_CASE("z-offset persistence: Forge-X's enable flag arrives as an int or a bool",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::persistence_already_enabled;
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});

    // An untouched param serialises as the JSON integer 0; once something sets
    // it explicitly the plugin stores a Python bool. All four spellings are
    // real traffic and must map the same way.
    CHECK(tri(persistence_already_enabled(hw, forge_x_enable_frame(0))) == "off");
    CHECK(tri(persistence_already_enabled(hw, forge_x_enable_frame(1))) == "on");
    CHECK(tri(persistence_already_enabled(hw, forge_x_enable_frame(false))) == "off");
    CHECK(tri(persistence_already_enabled(hw, forge_x_enable_frame(true))) == "on");

    // Unlike ZMOD, the plugin populates every declared param at load and the
    // declared default is 0 - so an off here is the shipped state, and the
    // enable send is exactly what it is for.
    CHECK(helix::zoffset::should_enable_persistence(
        true, false, false, persistence_already_enabled(hw, forge_x_enable_frame(0))));

    // No mod_params in this frame, so no answer.
    CHECK(tri(persistence_already_enabled(hw, forge_x_frame(-0.15))) == "unknown");
    CHECK(tri(persistence_already_enabled(hw, json{{"mod_params", json::object()}})) == "unknown");
    CHECK(tri(persistence_already_enabled(hw, json{{"gcode_move", json::object()}})) == "unknown");
    // A value we cannot read is silence, not an off.
    CHECK(tri(persistence_already_enabled(hw, forge_x_enable_frame("1"))) == "unknown");
    CHECK(tri(persistence_already_enabled(hw, forge_x_enable_frame(nullptr))) == "unknown");
}

TEST_CASE("z-offset persistence: firmware with no enable setting reports unknown",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::persistence_already_enabled;

    // Helper-Script's boot delayed_gcode re-applies the stored offset
    // unconditionally: there is no enable gcode and so nothing to read. A key
    // of that name landing in its save_variables belongs to someone else.
    PrinterDiscovery helper = printer_with_macros({"SET_GCODE_OFFSET"});
    REQUIRE(helix::zoffset::persistence_enable_gcode(helper).empty());
    CHECK(tri(persistence_already_enabled(helper, zmod_enable_frame(1))) == "unknown");

    // And a printer with no provider at all answers nothing, whatever arrives.
    PrinterDiscovery plain = printer_with_macros({"START_PRINT", "END_PRINT"});
    CHECK(tri(persistence_already_enabled(plain, zmod_enable_frame(1))) == "unknown");
    CHECK(tri(persistence_already_enabled(plain, forge_x_enable_frame(1))) == "unknown");
}

TEST_CASE("z-offset persistence: the enable flag is read from the matched firmware's schema",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::persistence_already_enabled;

    // Both firmwares name the flag load_zoffset but keep it in different status
    // objects, so a frame carrying both must resolve by the matched provider,
    // not by whichever key is found first.
    json both = json{{"save_variables", json{{"variables", json{{"load_zoffset", 1}}}}},
                     {"mod_params", json{{"variables", json{{"load_zoffset", 0}}}}}};
    CHECK(tri(persistence_already_enabled(printer_with_macros({"SAVE_ZMOD_DATA"}), both)) == "on");
    CHECK(tri(persistence_already_enabled(printer_with_macros({"SET_MOD"}), both)) == "off");
}

// ============================================================================
// Helper-Script save-zoffset (prestonbrown/helixscreen#1401)
// ============================================================================

TEST_CASE("z-offset persistence: Helper-Script's wrapper is detected by the wrapper object",
          "[zoffset][persistence][1401]") {
    // save-zoffset.cfg defines [gcode_macro SET_GCODE_OFFSET] with
    // rename_existing, and shadowing a builtin REQUIRES rename_existing - so
    // the wrapper object existing in objects/list is exactly "a renaming
    // wrapper is installed". The renamed original is a bare command, never an
    // object (verified against debug bundle 5J49T5RU: SET_GCODE_OFFSET in the
    // 83 macro objects, _SET_GCODE_OFFSET in none).
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});

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

// ============================================================================
// Refuting an over-matched provider (prestonbrown/helixscreen#1401)
// ============================================================================
//
// The Helper-Script row detects on the WRAPPER object, which proves something
// shadows SET_GCODE_OFFSET but not that the shadow stores anything - wrapping
// the command to log, clamp, or fan out per-tool offsets is a standard Voron /
// Klippain / toolchanger pattern. The match still latches immediately, because
// the two mistakes are not symmetric: over-matching costs a benign-wrapper user
// the Save Z Offset button, while under-matching folds a real Helper-Script
// offset into the probe and stacks it every boot (0.060 -> 2.515mm over five
// cycles, nozzle-on-bed). Only a frame that PROVES the store is absent relaxes
// it.

TEST_CASE("z-offset persistence: a wrapper with no zoffset store is refuted",
          "[zoffset][persistence][1401]") {
    // The over-match case. save_variables arrived, complete, carrying somebody
    // else's variables and no `zoffset` key at all - save-zoffset.cfg is not
    // installed, so the wrapper is one of the benign kinds and Save Z Offset
    // must come back.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});
    REQUIRE(helix::zoffset::firmware_persists_z_offset(hw));

    json frame = json{{"save_variables", json{{"variables", json{{"lan_clients", 7}}}}}};
    CHECK(helix::zoffset::status_refutes_persistence(hw, frame));

    // An empty store is the same proof: the module seeds its variable, so a
    // delivered-and-empty variables dict cannot be an install.
    json empty = json{{"save_variables", json{{"variables", json::object()}}}};
    CHECK(helix::zoffset::status_refutes_persistence(hw, empty));
}

TEST_CASE("z-offset persistence: a real Helper-Script install is never refuted",
          "[zoffset][persistence][1401]") {
    // The dangerous direction. The store carries the module's variable with a
    // real value: this printer persists, and Save Z Offset must stay down.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});

    json frame =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", -0.475}}}}}}}};
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(hw, frame));

    // Sibling keys in the same store change nothing.
    json with_siblings =
        json{{"save_variables",
              json{{"variables", json{{"lan_clients", 7}, {"zoffset", json{{"z", -0.475}}}}}}}};
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(hw, with_siblings));
}

TEST_CASE("z-offset persistence: the seeded zoffset placeholder does not refute",
          "[zoffset][persistence][1401]") {
    // THE boundary that protects against the damaging direction. save-zoffset.cfg
    // seeds `zoffset` as {'z': None} at install and only fills it on the first
    // wrapped SET_GCODE_OFFSET. The read path correctly calls that "nothing
    // stored yet" - but the module IS installed and its boot gcode WILL re-apply
    // whatever lands there, so treating the placeholder as a refutation would
    // hand the user a Save button on a printer that stacks the offset.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});

    json seeded =
        json{{"save_variables", json{{"variables", json{{"zoffset", json{{"z", nullptr}}}}}}}};
    REQUIRE_FALSE(helix::zoffset::read_persisted_offset_microns(seeded).has_value());
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(hw, seeded));

    // Any shape of the key counts as present: what refutes is the key's total
    // absence, never our failure to parse its value.
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(
        hw, json{{"save_variables", json{{"variables", json{{"zoffset", json::object()}}}}}}));
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(
        hw, json{{"save_variables", json{{"variables", json{{"zoffset", nullptr}}}}}}));
    CHECK_FALSE(helix::zoffset::status_refutes_persistence(
        hw, json{{"save_variables", json{{"variables", json{{"zoffset", "-0.475"}}}}}}));
}

TEST_CASE("z-offset persistence: no news is not evidence of absence",
          "[zoffset][persistence][1401]") {
    // save_variables is delta-only: the overwhelming majority of frames do not
    // carry it, and one that arrives without its variables member has told us
    // nothing either. Refuting on those would drop the stand-down on a real
    // Helper-Script box on the very next gcode_move frame.
    PrinterDiscovery hw = printer_with_macros({"SET_GCODE_OFFSET"});
    using helix::zoffset::status_refutes_persistence;

    CHECK_FALSE(status_refutes_persistence(hw, json{{"gcode_move", json{{"speed", 100.0}}}}));
    CHECK_FALSE(status_refutes_persistence(hw, json{{"save_variables", json::object()}}));
    CHECK_FALSE(
        status_refutes_persistence(hw, json{{"save_variables", json{{"variables", "nope"}}}}));
    CHECK_FALSE(status_refutes_persistence(hw, json{{"save_variables", nullptr}}));
    // Degenerate inputs must not throw.
    CHECK_FALSE(status_refutes_persistence(hw, json::object()));
    CHECK_FALSE(status_refutes_persistence(hw, json()));
}

TEST_CASE("z-offset persistence: unambiguous providers are never refutable",
          "[zoffset][persistence][1401]") {
    using helix::zoffset::status_refutes_persistence;

    // SAVE_ZMOD_DATA and SET_MOD each belong to exactly one firmware, so the
    // match cannot be wrong and no frame gets to argue with it. ZMOD's own
    // store lives under a different key, so the very frame that refutes the
    // wrapper row must leave ZMOD alone.
    json no_zoffset_key = json{{"save_variables", json{{"variables", json{{"lan_clients", 7}}}}}};
    CHECK_FALSE(
        status_refutes_persistence(printer_with_macros({"SAVE_ZMOD_DATA"}), no_zoffset_key));
    CHECK_FALSE(
        status_refutes_persistence(printer_with_macros({"SAVE_ZMOD_DATA"}),
                                   json{{"save_variables", json{{"variables", json::object()}}}}));
    CHECK_FALSE(status_refutes_persistence(printer_with_macros({"SET_MOD"}), no_zoffset_key));
    json empty_mod_params = json{{"mod_params", json{{"variables", json::object()}}}};
    CHECK_FALSE(status_refutes_persistence(printer_with_macros({"SET_MOD"}), empty_mod_params));

    // A box carrying BOTH macros resolves to ZMOD, so the wrapper row's
    // refutation must not reach it through the back door.
    CHECK_FALSE(status_refutes_persistence(
        printer_with_macros({"SAVE_ZMOD_DATA", "SET_GCODE_OFFSET"}), no_zoffset_key));

    // And a printer with no provider at all has nothing to refute.
    CHECK_FALSE(status_refutes_persistence(printer_with_macros({"START_PRINT"}), no_zoffset_key));
}

// ============================================================================
// Claiming the one-shot enable (prestonbrown/helixscreen#1432)
// ============================================================================

TEST_CASE("z-offset persistence: the enable is claimed once and never again",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    CHECK(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    // The command writes persistent firmware state, so a second discovery -
    // this launch or any later one - must not re-send it over a user who has
    // since turned the setting back off.
    CHECK_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: a send that never landed gives the claim back",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;
    using helix::zoffset::release_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    REQUIRE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    REQUIRE_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));

    // The claim is recorded before the gcode goes out, so a send that fails -
    // klippy not ready, socket dropped - would otherwise leave the firmware
    // never told and the one shot spent for the life of the install.
    release_persistence_enable(cfg);
    CHECK(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: releasing an unclaimed one-shot changes nothing",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;
    using helix::zoffset::release_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    // Idempotent in the direction that matters: a stray release must not hand
    // out a second send on a printer that was already told.
    release_persistence_enable(cfg);
    REQUIRE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    release_persistence_enable(cfg);
    release_persistence_enable(cfg);
    REQUIRE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    CHECK_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: the release reaches disk", "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;
    using helix::zoffset::release_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    REQUIRE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    release_persistence_enable(cfg);

    // Same reasoning as the claim: a release held only in memory is no release
    // at all, and the next launch would read the spent claim back.
    reload_from_disk(cfg);
    CHECK(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: the claim survives a restart", "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    REQUIRE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));

    // A record held only in the in-memory document is no record at all: the
    // next launch reads settings.json and would send the command again.
    reload_from_disk(cfg);
    CHECK_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: a print does not consume the claim",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    CHECK_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/true));
    // Refusing to inject gcode into a running job must not cost the printer its
    // one send - the next idle discovery still gets it.
    CHECK(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: an already-enabled firmware does not consume the claim",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});

    json already_on = forge_x_enable_frame(true);
    CHECK_FALSE(claim_persistence_enable(cfg, hw, &already_on, /*print_active=*/false));

    // Standing down because the firmware agrees is not the same as having sent
    // anything, so a printer that later reports it off is still owed its send.
    json now_off = forge_x_enable_frame(false);
    CHECK(claim_persistence_enable(cfg, hw, &now_off, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: a printer with no enable command never claims",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    Config* cfg = fresh_config();
    json status = forge_x_enable_frame(0);

    CHECK_FALSE(claim_persistence_enable(cfg, printer_with_macros({"START_PRINT", "END_PRINT"}),
                                         &status, /*print_active=*/false));
    // Helper-Script persists the offset but has nothing to switch on.
    CHECK_FALSE(claim_persistence_enable(cfg, printer_with_macros({"SET_GCODE_OFFSET"}), &status,
                                         /*print_active=*/false));
    // A null config is a caller with nothing to record into.
    CHECK_FALSE(claim_persistence_enable(nullptr, printer_with_macros({"SET_MOD"}), &status,
                                         /*print_active=*/false));
}

TEST_CASE("z-offset persistence: no status frame is no news, and still allows the send",
          "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    // A discovery that carries no frame says nothing about the firmware's
    // setting. Treating that silence as "already on" would leave the feature
    // permanently off on a printer that needs it.
    Config* cfg = fresh_config();
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});

    CHECK(claim_persistence_enable(cfg, hw, nullptr, /*print_active=*/false));
    CHECK_FALSE(claim_persistence_enable(cfg, hw, nullptr, /*print_active=*/false));
}

TEST_CASE("z-offset persistence: each printer gets its own claim", "[zoffset][persistence][1432]") {
    using helix::zoffset::claim_persistence_enable;

    // The setting lives in a printer's firmware, so the record is scoped to the
    // active printer. One install driving two printers owes each of them a send.
    PrinterDiscovery hw = printer_with_macros({"SET_MOD"});
    json status = forge_x_enable_frame(0);

    Config* cfg = fresh_config("printer_a");
    REQUIRE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    REQUIRE_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));

    helix::ConfigTestAccess::active_printer_id(*cfg) = "printer_b";
    CHECK(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
    CHECK_FALSE(claim_persistence_enable(cfg, hw, &status, /*print_active=*/false));
}
