// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_tool_offsets.cpp
 * @brief Tests for the per-tool offset abstraction.
 *
 * A tool changer carries X, Y and Z offsets per toolhead, independent of the
 * live gcode_move offset the rest of the UI tunes, and the firmwares disagree
 * on where they live, what writes them, which axes are editable at all, and
 * whether persisting restarts Klipper. helix::tool_offsets owns all of that. Generic code asks
 * these questions and never names a firmware, so these tests are also the guard on that boundary:
 * they exercise the capability API and reach a vendor only through a printer's
 * object list.
 */

#include "printer_discovery.h"
#include "tool_offsets.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::Axis;
using helix::PrinterDiscovery;
using nlohmann::json;
namespace to_ = helix::tool_offsets;

namespace {

/// A plain single-toolhead printer.
PrinterDiscovery plain_printer() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"gcode_move", "toolhead", "extruder"}));
    return hw;
}

/// Objects for a klipper-toolchanger machine with @p tool_count toolheads.
json toolchanger_objects(int tool_count) {
    json objects = json::array({"gcode_move", "toolhead", "extruder", "toolchanger"});
    for (int i = 0; i < tool_count; ++i) {
        objects.push_back("tool T" + std::to_string(i));
    }
    return objects;
}

/// A stock klipper-toolchanger printer.
PrinterDiscovery toolchanger_printer(int tool_count = 4) {
    PrinterDiscovery hw;
    hw.parse_objects(toolchanger_objects(tool_count));
    return hw;
}

/// A single-toolhead printer that happens to define a TOOL_OFFSET macro.
PrinterDiscovery macro_but_one_tool() {
    json objects = json::array({"gcode_move", "toolhead", "extruder"});
    objects.push_back("gcode_macro TOOL_OFFSET");
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// A MedusaHC-style machine: klipper-toolchanger PLUS the TOOL_OFFSET macro.
/// This is the real shape - MedusaHC ships [toolchanger] and [tool T0..T3].
PrinterDiscovery tool_offset_macro_printer(int tool_count = 4) {
    json objects = toolchanger_objects(tool_count);
    objects.push_back("gcode_macro TOOL_OFFSET");
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

/// The same machine, but printer.cfg spells the section `[gcode_macro
/// Tool_Offset]`. Klipper accepts any casing and uppercases only the COMMAND
/// alias, so this is a legal config that behaves identically on the printer.
PrinterDiscovery mixed_case_macro_printer(int tool_count = 4) {
    json objects = toolchanger_objects(tool_count);
    objects.push_back("gcode_macro Tool_Offset");
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

bool vec_has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

// ============================================================================
// Detection
// ============================================================================

TEST_CASE("tool offsets: a single-toolhead printer has none", "[tool_offsets]") {
    // This is what gates the tune panel's tool selector. A printer with one
    // nozzle must answer no, or the selector appears with nothing to select.
    PrinterDiscovery hw = plain_printer();

    CHECK_FALSE(to_::supports_per_tool_offsets(hw));
    CHECK(to_::provider_name(hw).empty());
    CHECK(to_::required_status_objects(hw).empty());
}

TEST_CASE("tool offsets: a tool changer has one per tool", "[tool_offsets]") {
    PrinterDiscovery hw = toolchanger_printer();

    CHECK(to_::supports_per_tool_offsets(hw));
    CHECK(to_::provider_name(hw) == "klipper-toolchanger");
}

TEST_CASE("tool offsets: klipper-toolchanger needs no extra subscription", "[tool_offsets]") {
    // The offsets ride on the `tool T*` objects the tool-changer subscription
    // already requests, so asking for more would be dead bandwidth.
    CHECK(to_::required_status_objects(toolchanger_printer()).empty());
}

// ============================================================================
// Precedence: the trap that makes row order load-bearing
// ============================================================================

TEST_CASE("tool offsets: the TOOL_OFFSET macro outranks klipper-toolchanger", "[tool_offsets]") {
    // A MedusaHC ships [toolchanger] and [tool T0..T3], so it matches BOTH
    // rows. Its macros print off the TOOL_OFFSET variables and never read
    // klipper-toolchanger's own offset, so resolving to the second row would
    // write a store nothing consumes - the adjustment would silently do
    // nothing on the machine.
    PrinterDiscovery hw = tool_offset_macro_printer();

    CHECK(to_::provider_name(hw) == "TOOL_OFFSET macro");
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 1, -50).rfind("SET_GCODE_VARIABLE", 0) == 0);
    CHECK(to_::required_status_objects(hw).size() == 1);
    CHECK(vec_has(to_::required_status_objects(hw), "gcode_macro TOOL_OFFSET"));
}

TEST_CASE("tool offsets: a frame carrying both schemas reads the authoritative one",
          "[tool_offsets]") {
    // Same trap on the read path, which has no PrinterDiscovery and resolves by
    // schema. A MedusaHC publishes both, and they can legitimately disagree -
    // klipper-toolchanger's copy is not what the machine prints with.
    json status = json{
        {"gcode_macro TOOL_OFFSET", json{{"t1_off_z", -0.20}}},
        {"tool T1", json{{"gcode_z_offset", -0.05}}},
    };

    auto microns = to_::read_tool_offset_microns(status, Axis::Z, 1, "T1");
    REQUIRE(microns.has_value());
    CHECK(*microns == -200);
}

TEST_CASE("tool offsets: a TOOL_OFFSET macro alone is not a tool changer", "[tool_offsets]") {
    // supports_per_tool_offsets() gates the tune panel's selector, and its contract is
    // "false on every single-toolhead printer". TOOL_OFFSET is a plausible name
    // for a hand-written macro, and matching on it alone put the selector on a
    // single-extruder machine and aimed its buttons at `t0_off_z` in a macro
    // with no such variable.
    PrinterDiscovery hw = macro_but_one_tool();

    CHECK_FALSE(to_::supports_per_tool_offsets(hw));
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 0, -50).empty());
}

TEST_CASE("tool offsets: a delta frame does not fall through to the other store",
          "[tool_offsets]") {
    // The by-schema read resolves in table order, which is right when both
    // schemas are in ONE frame. Moonraker's notify_status_update carries only
    // what changed, so a MedusaHC frame with just `tool T0` used to fall past
    // the authoritative TOOL_OFFSET row and answer from klipper-toolchanger's
    // copy — the store this module says is NOT the authority. That value then
    // overwrote the real one and became the base for the next adjustment.
    json delta = json{{"tool T0", json{{"gcode_z_offset", -0.05}}}};

    // No TOOL_OFFSET object in this frame at all -> the macro provider is not
    // the owner here, so answering from the tool object is correct.
    CHECK(*to_::read_tool_offset_microns(delta, Axis::Z, 0, "T0") == -50);

    // But once the macro IS present and simply has nothing for this tool, the
    // frame belongs to it and the tool object must not be consulted.
    json macro_frame = json{
        {"gcode_macro TOOL_OFFSET", json{{"t1_off_z", -0.20}}},
        {"tool T0", json{{"gcode_z_offset", -0.05}}},
    };
    CHECK_FALSE(to_::read_tool_offset_microns(macro_frame, Axis::Z, 0, "T0").has_value());
    CHECK(*to_::read_tool_offset_microns(macro_frame, Axis::Z, 1, "T1") == -200);
}

// ============================================================================
// Config casing: detection is case-insensitive, Klipper's keys are not
// ============================================================================

TEST_CASE("tool offsets: a mixed-case macro is subscribed as printer.cfg spells it",
          "[tool_offsets]") {
    // has_macro() is case-insensitive because the callable command is the
    // UPPERCASED alias. The status object key is not: Klipper publishes the
    // config section verbatim, and an object it cannot look up is silently
    // absent from every frame rather than an error. So subscribing to the
    // uppercased name on this machine reads empty forever - the selector shows
    // nothing, and the reader falls through to klipper-toolchanger's copy, the
    // store this module's own table says is not the authority.
    PrinterDiscovery hw = mixed_case_macro_printer();

    REQUIRE(to_::supports_per_tool_offsets(hw));
    CHECK(to_::provider_name(hw) == "TOOL_OFFSET macro");
    CHECK(vec_has(to_::required_status_objects(hw), "gcode_macro Tool_Offset"));
    CHECK_FALSE(vec_has(to_::required_status_objects(hw), "gcode_macro TOOL_OFFSET"));
}

TEST_CASE("tool offsets: a mixed-case macro's writes use the config-case mux key",
          "[tool_offsets]") {
    // SET_GCODE_VARIABLE's MACRO= is a mux key registered on the config-case
    // name (klippy/extras/gcode_macro.py registers `name`, not `self.alias`),
    // so a capitalised MACRO= is REJECTED - the adjustment errors outright
    // rather than quietly missing.
    PrinterDiscovery hw = mixed_case_macro_printer();

    const std::string set = to_::set_tool_offset_gcode(hw, Axis::Z, 1, -50);
    CHECK(set.find("MACRO=Tool_Offset ") != std::string::npos);
    CHECK(set.find("MACRO=TOOL_OFFSET") == std::string::npos);

    // The save carries the runtime half, so it is subject to the same trap.
    const std::string save = to_::save_tool_offset_gcode(hw, Axis::Z, 1, -50);
    CHECK(save.find("MACRO=Tool_Offset ") != std::string::npos);
    CHECK(save.find("SAVE_VARIABLE VARIABLE=t1_gcode_z_offset") != std::string::npos);
}

TEST_CASE("tool offsets: a mixed-case macro object is still read", "[tool_offsets]") {
    // The read path has no PrinterDiscovery to resolve the casing through, so
    // it scans. Two sections differing only in case would register the same
    // command alias and Klipper would refuse to start, so the scan cannot be
    // ambiguous.
    json status = json{{"gcode_macro Tool_Offset", json{{"t2_off_z", -0.15}}}};

    auto microns = to_::read_tool_offset_microns(status, Axis::Z, 2, "T2");
    REQUIRE(microns.has_value());
    CHECK(*microns == -150);
}

TEST_CASE("tool offsets: a mixed-case macro still owns its frame", "[tool_offsets]") {
    // The fallthrough guard keys off the macro being PRESENT. Matching that
    // case-sensitively would make a Tool_Offset frame look like it had no macro
    // at all, and the tool object underneath would answer for it.
    json frame = json{
        {"gcode_macro Tool_Offset", json{{"t1_off_z", -0.20}}},
        {"tool T0", json{{"gcode_z_offset", -0.05}}},
    };

    CHECK_FALSE(to_::read_tool_offset_microns(frame, Axis::Z, 0, "T0").has_value());
    CHECK(*to_::read_tool_offset_microns(frame, Axis::Z, 1, "T1") == -200);
}

// ============================================================================
// Reading
// ============================================================================

TEST_CASE("tool offsets: klipper-toolchanger reads off the tool's own object", "[tool_offsets]") {
    json status = json{{"tool T2", json{{"active", true}, {"gcode_z_offset", -0.15}}}};

    auto microns = to_::read_tool_offset_microns(status, Axis::Z, 2, "T2");
    REQUIRE(microns.has_value());
    CHECK(*microns == -150);
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro reads off the tool index", "[tool_offsets]") {
    // All four tools live on one macro, keyed by number rather than by object.
    json status = json{{"gcode_macro TOOL_OFFSET",
                        json{{"t0_off_z", 0.10}, {"t1_off_z", -0.20}, {"t2_off_z", 0.0}}}};

    CHECK(*to_::read_tool_offset_microns(status, Axis::Z, 0, "T0") == 100);
    CHECK(*to_::read_tool_offset_microns(status, Axis::Z, 1, "T1") == -200);
    CHECK(*to_::read_tool_offset_microns(status, Axis::Z, 2, "T2") == 0);
    // A tool the macro does not carry is unknown, not zero.
    CHECK_FALSE(to_::read_tool_offset_microns(status, Axis::Z, 3, "T3").has_value());
}

TEST_CASE("tool offsets: a float that lands just short still rounds", "[tool_offsets]") {
    // The value round-trips through a float, so a nominal -0.150 arrives as
    // -0.1499999. Truncating would report -149 and the UI would drift by a
    // micron every time it echoed the value back.
    json status = json{{"tool T0", json{{"gcode_z_offset", -0.1499999}}}};

    CHECK(*to_::read_tool_offset_microns(status, Axis::Z, 0, "T0") == -150);
}

TEST_CASE("tool offsets: zero is a value, not an absence", "[tool_offsets]") {
    // A tool genuinely at 0.000 must read as 0, not as "nothing reported" -
    // that distinction is why the UI carries a separate validity subject.
    json status = json{{"tool T0", json{{"gcode_z_offset", 0.0}}}};

    auto microns = to_::read_tool_offset_microns(status, Axis::Z, 0, "T0");
    REQUIRE(microns.has_value());
    CHECK(*microns == 0);
}

TEST_CASE("tool offsets: a frame without the field is no news", "[tool_offsets]") {
    // Moonraker republishes only what CHANGED, so a frame that carries other
    // tool state and not this one must not be read as a reset to zero.
    json status = json{{"tool T0", json{{"active", true}, {"mounted", true}}}};

    CHECK_FALSE(to_::read_tool_offset_microns(status, Axis::Z, 0, "T0").has_value());
}

TEST_CASE("tool offsets: a malformed or empty frame is ignored", "[tool_offsets]") {
    CHECK_FALSE(to_::read_tool_offset_microns(json{{"tool T0", json{{"gcode_z_offset", "oops"}}}},
                                              Axis::Z, 0, "T0")
                    .has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(json::object(), Axis::Z, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(json::array(), Axis::Z, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(json(nullptr), Axis::Z, 0, "T0").has_value());
    // No name to key the object off.
    CHECK_FALSE(to_::read_tool_offset_microns(json{{"tool T0", json{{"gcode_z_offset", -0.1}}}},
                                              Axis::Z, 0, "")
                    .has_value());
}

// ============================================================================
// Writing and persisting
// ============================================================================

TEST_CASE("tool offsets: the klipper-toolchanger write is a bare decimal", "[tool_offsets]") {
    // klipper-toolchanger runs VALUE= through Python's ast.literal_eval(), so
    // the value must be a plain numeric literal. Anything the UI would show a
    // user - "+0.050mm", "-0.05 mm" - raises instead of setting the offset.
    PrinterDiscovery hw = toolchanger_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 1, -50) ==
          "SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.050");
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 0, 125) ==
          "SET_TOOL_PARAMETER T=0 PARAMETER=gcode_z_offset VALUE=0.125");
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 3, 0) ==
          "SET_TOOL_PARAMETER T=3 PARAMETER=gcode_z_offset VALUE=0.000");
}

TEST_CASE("tool offsets: the klipper-toolchanger save sets then persists", "[tool_offsets]") {
    // SAVE_TOOL_PARAMETER persists whatever the tool currently holds and takes
    // no value of its own, so the set has to lead or the save stores the old
    // number.
    PrinterDiscovery hw = toolchanger_printer();

    CHECK(to_::save_tool_offset_gcode(hw, Axis::Z, 1, -50) ==
          "SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.050\n"
          "SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset");
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro write targets the macro variable",
          "[tool_offsets]") {
    PrinterDiscovery hw = tool_offset_macro_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 2, -75) ==
          "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t2_off_z VALUE=-0.075");
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro save writes both stores", "[tool_offsets]") {
    // Durable copy AND the runtime variable the machine actually prints with -
    // writing only save_variables would not change this print.
    PrinterDiscovery hw = tool_offset_macro_printer();

    CHECK(to_::save_tool_offset_gcode(hw, Axis::Z, 2, -75) ==
          "SAVE_VARIABLE VARIABLE=t2_gcode_z_offset VALUE=-0.075\n"
          "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t2_off_z VALUE=-0.075");
}

TEST_CASE("tool offsets: only klipper-toolchanger's save restarts Klipper", "[tool_offsets]") {
    // SAVE_TOOL_PARAMETER stages a config change that SAVE_CONFIG commits, and
    // that restarts Klipper. SAVE_VARIABLE lands immediately. A UI that gets
    // this backwards either leaves the value uncommitted or sits waiting out a
    // restart that never comes.
    CHECK(to_::persist_requires_save_config(toolchanger_printer()));
    CHECK_FALSE(to_::persist_requires_save_config(tool_offset_macro_printer()));
    CHECK_FALSE(to_::persist_requires_save_config(plain_printer()));
}

TEST_CASE("tool offsets: a printer without the capability emits nothing", "[tool_offsets]") {
    // An empty string is the "this printer needs no such call" answer, matching
    // helix::zoffset. A caller that sends it blindly would inject a bare
    // newline, so the emptiness is the contract.
    PrinterDiscovery hw = plain_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 0, -50).empty());
    CHECK(to_::save_tool_offset_gcode(hw, Axis::Z, 0, -50).empty());
}

TEST_CASE("tool offsets: a negative tool index emits nothing", "[tool_offsets]") {
    // -1 is ToolState's "no active tool". Formatting it would send T=-1 or
    // t-1_off_z, which Klipper rejects with an error the caller only logs.
    PrinterDiscovery hw = toolchanger_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, -1, -50).empty());
    CHECK(to_::save_tool_offset_gcode(hw, Axis::Z, -1, -50).empty());
    CHECK(to_::set_tool_offset_gcode(tool_offset_macro_printer(), Axis::Z, -1, -50).empty());
}

// ============================================================================
// Axes: which of X / Y / Z each firmware keeps
// ============================================================================

TEST_CASE("tool offsets: klipper-toolchanger keeps all three axes", "[tool_offsets]") {
    // gcode_x/y/z_offset are three fields on the same object, written by the
    // same command, so a caller may loop kAllAxes without special cases.
    PrinterDiscovery hw = toolchanger_printer();

    for (Axis axis : helix::kAllAxes) {
        CAPTURE(helix::axis_letter(axis));
        CHECK(to_::supports_axis(hw, axis));
    }
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro keeps only Z", "[tool_offsets]") {
    // Not a capability of the machine, a gap in ours: only the Z variable
    // names have been checked against a MedusaHC. Claiming X/Y would guess a
    // name and write a value nothing reads. Once the names are confirmed this
    // test flips to expecting true - see supports_axis_tool_offset_macro().
    PrinterDiscovery hw = tool_offset_macro_printer();

    CHECK(to_::supports_axis(hw, Axis::Z));
    CHECK_FALSE(to_::supports_axis(hw, Axis::X));
    CHECK_FALSE(to_::supports_axis(hw, Axis::Y));
}

TEST_CASE("tool offsets: a printer without the capability supports no axis", "[tool_offsets]") {
    PrinterDiscovery hw = plain_printer();

    for (Axis axis : helix::kAllAxes) {
        CAPTURE(helix::axis_letter(axis));
        CHECK_FALSE(to_::supports_axis(hw, axis));
    }
}

TEST_CASE("tool offsets: klipper-toolchanger writes X and Y through their own parameter",
          "[tool_offsets]") {
    // One parameter per command: SET_TOOL_PARAMETER takes a single PARAMETER=,
    // so X and Y are separate writes, each naming its own field.
    PrinterDiscovery hw = toolchanger_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::X, 1, 125) ==
          "SET_TOOL_PARAMETER T=1 PARAMETER=gcode_x_offset VALUE=0.125");
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Y, 2, -50) ==
          "SET_TOOL_PARAMETER T=2 PARAMETER=gcode_y_offset VALUE=-0.050");
}

TEST_CASE("tool offsets: the klipper-toolchanger X save persists the X parameter",
          "[tool_offsets]") {
    // SAVE_TOOL_PARAMETER names the parameter it persists, so the save for X
    // must not stage Z (or vice versa) - and must still lead with the set.
    PrinterDiscovery hw = toolchanger_printer();

    CHECK(to_::save_tool_offset_gcode(hw, Axis::X, 1, 125) ==
          "SET_TOOL_PARAMETER T=1 PARAMETER=gcode_x_offset VALUE=0.125\n"
          "SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_x_offset");
    CHECK(to_::save_tool_offset_gcode(hw, Axis::Y, 3, 0) ==
          "SET_TOOL_PARAMETER T=3 PARAMETER=gcode_y_offset VALUE=0.000\n"
          "SAVE_TOOL_PARAMETER T=3 PARAMETER=gcode_y_offset");
}

TEST_CASE("tool offsets: klipper-toolchanger reads X and Y off the tool's object",
          "[tool_offsets]") {
    json status = json{
        {"tool T2",
         json{{"gcode_x_offset", 0.125}, {"gcode_y_offset", -0.05}, {"gcode_z_offset", -0.15}}}};

    CHECK(*to_::read_tool_offset_microns(status, Axis::X, 2, "T2") == 125);
    CHECK(*to_::read_tool_offset_microns(status, Axis::Y, 2, "T2") == -50);
    CHECK(*to_::read_tool_offset_microns(status, Axis::Z, 2, "T2") == -150);
}

TEST_CASE("tool offsets: an axis missing from a delta frame is no news for that axis",
          "[tool_offsets]") {
    // Moonraker republishes only what CHANGED, per field: a frame carrying a
    // new X for a tool says nothing about its Y or Z, and reading either as
    // zero would yank a displayed value to 0 mid-adjustment.
    json delta = json{{"tool T0", json{{"gcode_x_offset", 0.125}}}};

    CHECK(*to_::read_tool_offset_microns(delta, Axis::X, 0, "T0") == 125);
    CHECK_FALSE(to_::read_tool_offset_microns(delta, Axis::Y, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(delta, Axis::Z, 0, "T0").has_value());
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro emits nothing for X and Y", "[tool_offsets]") {
    // Declining is the contract, exactly like a negative tool index: an empty
    // string is "this printer needs no such call", and a caller that loops the
    // axes sends nothing rather than a guessed variable name.
    PrinterDiscovery hw = tool_offset_macro_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::X, 1, -50).empty());
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Y, 1, -50).empty());
    CHECK(to_::save_tool_offset_gcode(hw, Axis::X, 1, -50).empty());
    CHECK(to_::save_tool_offset_gcode(hw, Axis::Y, 1, -50).empty());
    // Z is untouched by that refusal.
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 1, -50).rfind("SET_GCODE_VARIABLE", 0) == 0);
}

TEST_CASE("tool offsets: a TOOL_OFFSET frame never answers X or Y from the tool object",
          "[tool_offsets]") {
    // The store_present guard is per frame, not per axis: once the macro owns
    // the frame, klipper-toolchanger's copy is not consulted for ANY axis -
    // otherwise X would read from the store this module says is not the
    // authority, while Z reads from the macro.
    json frame = json{
        {"gcode_macro TOOL_OFFSET", json{{"t0_off_z", -0.20}}},
        {"tool T0", json{{"gcode_x_offset", 0.125}, {"gcode_z_offset", -0.05}}},
    };

    CHECK_FALSE(to_::read_tool_offset_microns(frame, Axis::X, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(frame, Axis::Y, 0, "T0").has_value());
    CHECK(*to_::read_tool_offset_microns(frame, Axis::Z, 0, "T0") == -200);
}

TEST_CASE("tool offsets: a negative tool index emits nothing on any axis", "[tool_offsets]") {
    PrinterDiscovery hw = toolchanger_printer();

    for (Axis axis : helix::kAllAxes) {
        CAPTURE(helix::axis_letter(axis));
        CHECK(to_::set_tool_offset_gcode(hw, axis, -1, 50).empty());
        CHECK(to_::save_tool_offset_gcode(hw, axis, -1, 50).empty());
    }
}

TEST_CASE("tool offsets: a malformed X field is not a reading", "[tool_offsets]") {
    CHECK_FALSE(to_::read_tool_offset_microns(json{{"tool T0", json{{"gcode_x_offset", "oops"}}}},
                                              Axis::X, 0, "T0")
                    .has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(json{{"tool T0", json{{"gcode_x_offset", nullptr}}}},
                                              Axis::X, 0, "T0")
                    .has_value());
}
