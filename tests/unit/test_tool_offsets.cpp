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

/// A MedusaHC-style machine running klipper-toolchanger beside its controller,
/// with the unprefixed TOOL_OFFSET macro an older config spells.
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

/// A stock MedusaHC: the controller and its dock sensors, `_TOOL_OFFSET`, one
/// extruder per hot end, and no klipper-toolchanger at all.
PrinterDiscovery stock_medusahc_printer(bool with_plain_macro_too = false) {
    json objects =
        json::array({"gcode_move", "toolhead", "extruder", "extruder1", "extruder2", "extruder3",
                     "pin_watch io", "medusahc", "save_variables", "gcode_macro _TOOL_OFFSET"});
    if (with_plain_macro_too) {
        objects.push_back("gcode_macro TOOL_OFFSET");
    }
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
    // Same trap on the read path. A MedusaHC publishes both, and they can
    // legitimately disagree - klipper-toolchanger's copy is not what the
    // machine prints with. The reader resolved from discovery knows which.
    const to_::OffsetReader reader = to_::resolve_reader(tool_offset_macro_printer());
    json status = json{
        {"gcode_macro TOOL_OFFSET", json{{"t1_off_z", -0.20}}},
        {"tool T1", json{{"gcode_z_offset", -0.05}}},
    };

    auto microns = to_::read_tool_offset_microns(reader, status, Axis::Z, 1, "T1");
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
    // Moonraker's notify_status_update carries only what changed. Reading by
    // the resolved model, the tool object is never consulted on a MedusaHC
    // machine, whatever the frame carries.
    json delta = json{{"tool T0", json{{"gcode_z_offset", -0.05}}}};

    const to_::OffsetReader medusa = to_::resolve_reader(tool_offset_macro_printer());
    CHECK_FALSE(to_::read_tool_offset_microns(medusa, delta, Axis::Z, 0, "T0").has_value());

    // The same frame on a stock klipper-toolchanger IS that store's news.
    const to_::OffsetReader stock = to_::resolve_reader(toolchanger_printer());
    CHECK(*to_::read_tool_offset_microns(stock, delta, Axis::Z, 0, "T0") == -50);

    // And a frame where the macro simply has nothing for this tool.
    json macro_frame = json{
        {"gcode_macro TOOL_OFFSET", json{{"t1_off_z", -0.20}}},
        {"tool T0", json{{"gcode_z_offset", -0.05}}},
    };
    CHECK_FALSE(to_::read_tool_offset_microns(medusa, macro_frame, Axis::Z, 0, "T0").has_value());
    CHECK(*to_::read_tool_offset_microns(medusa, macro_frame, Axis::Z, 1, "T1") == -200);
}

TEST_CASE("tool offsets: an axis the macro frame does not carry reads nothing", "[tool_offsets]") {
    // An X read on the macro model must be "no news" when the macro said
    // nothing about X, even though klipper-toolchanger's tool object in the
    // same frame carries an X value.
    const to_::OffsetReader medusa = to_::resolve_reader(tool_offset_macro_printer());
    json frame = json{
        {"gcode_macro TOOL_OFFSET", json{{"t1_off_z", -0.20}}},
        {"tool T1", json{{"gcode_x_offset", 0.12}, {"gcode_z_offset", -0.05}}},
    };

    CHECK_FALSE(to_::read_tool_offset_microns(medusa, frame, Axis::X, 1, "T1").has_value());
    CHECK(*to_::read_tool_offset_microns(medusa, frame, Axis::Z, 1, "T1") == -200);
}

TEST_CASE("tool offsets: a printer without per-tool offsets reads nothing", "[tool_offsets]") {
    const to_::OffsetReader none = to_::resolve_reader(plain_printer());
    json frame = json{{"tool T0", json{{"gcode_z_offset", -0.05}}}};

    CHECK_FALSE(to_::read_tool_offset_microns(none, frame, Axis::Z, 0, "T0").has_value());
    CHECK_FALSE(
        to_::read_tool_offset_microns(to_::OffsetReader{}, frame, Axis::Z, 0, "T0").has_value());
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
    // The reader carries the status key spelled as printer.cfg spells the
    // section - the key Klipper actually publishes - so the read is a direct
    // find with no case-folding scan of the frame.
    const to_::OffsetReader reader = to_::resolve_reader(mixed_case_macro_printer());
    CHECK(reader.store_object == "gcode_macro Tool_Offset");
    json status = json{{"gcode_macro Tool_Offset", json{{"t2_off_z", -0.15}}}};

    auto microns = to_::read_tool_offset_microns(reader, status, Axis::Z, 2, "T2");
    REQUIRE(microns.has_value());
    CHECK(*microns == -150);
}

TEST_CASE("tool offsets: a mixed-case macro still owns its frame", "[tool_offsets]") {
    // A Tool_Offset machine is that model whatever the casing; the tool object
    // underneath never answers for it.
    const to_::OffsetReader reader = to_::resolve_reader(mixed_case_macro_printer());
    json frame = json{
        {"gcode_macro Tool_Offset", json{{"t1_off_z", -0.20}}},
        {"tool T0", json{{"gcode_z_offset", -0.05}}},
    };

    CHECK_FALSE(to_::read_tool_offset_microns(reader, frame, Axis::Z, 0, "T0").has_value());
    CHECK(*to_::read_tool_offset_microns(reader, frame, Axis::Z, 1, "T1") == -200);
}

// ============================================================================
// Reading
// ============================================================================

TEST_CASE("tool offsets: klipper-toolchanger reads off the tool's own object", "[tool_offsets]") {
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    CHECK(reader.store_object.empty()); // one object per tool, nothing to resolve
    json status = json{{"tool T2", json{{"active", true}, {"gcode_z_offset", -0.15}}}};

    auto microns = to_::read_tool_offset_microns(reader, status, Axis::Z, 2, "T2");
    REQUIRE(microns.has_value());
    CHECK(*microns == -150);
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro reads off the tool index", "[tool_offsets]") {
    // All four tools live on one macro, keyed by number rather than by object.
    const to_::OffsetReader reader = to_::resolve_reader(tool_offset_macro_printer());
    json status = json{{"gcode_macro TOOL_OFFSET",
                        json{{"t0_off_z", 0.10}, {"t1_off_z", -0.20}, {"t2_off_z", 0.0}}}};

    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::Z, 0, "T0") == 100);
    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::Z, 1, "T1") == -200);
    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::Z, 2, "T2") == 0);
    // A tool the macro does not carry is unknown, not zero.
    CHECK_FALSE(to_::read_tool_offset_microns(reader, status, Axis::Z, 3, "T3").has_value());
}

TEST_CASE("tool offsets: a float that lands just short still rounds", "[tool_offsets]") {
    // The value round-trips through a float, so a nominal -0.150 arrives as
    // -0.1499999. Truncating would report -149 and the UI would drift by a
    // micron every time it echoed the value back.
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    json status = json{{"tool T0", json{{"gcode_z_offset", -0.1499999}}}};

    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::Z, 0, "T0") == -150);
}

TEST_CASE("tool offsets: zero is a value, not an absence", "[tool_offsets]") {
    // A tool genuinely at 0.000 must read as 0, not as "nothing reported" -
    // that distinction is why the UI carries a separate validity subject.
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    json status = json{{"tool T0", json{{"gcode_z_offset", 0.0}}}};

    auto microns = to_::read_tool_offset_microns(reader, status, Axis::Z, 0, "T0");
    REQUIRE(microns.has_value());
    CHECK(*microns == 0);
}

TEST_CASE("tool offsets: a frame without the field is no news", "[tool_offsets]") {
    // Moonraker republishes only what CHANGED, so a frame that carries other
    // tool state and not this one must not be read as a reset to zero.
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    json status = json{{"tool T0", json{{"active", true}, {"mounted", true}}}};

    CHECK_FALSE(to_::read_tool_offset_microns(reader, status, Axis::Z, 0, "T0").has_value());
}

TEST_CASE("tool offsets: a malformed or empty frame is ignored", "[tool_offsets]") {
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    CHECK_FALSE(to_::read_tool_offset_microns(
                    reader, json{{"tool T0", json{{"gcode_z_offset", "oops"}}}}, Axis::Z, 0, "T0")
                    .has_value());
    CHECK_FALSE(
        to_::read_tool_offset_microns(reader, json::object(), Axis::Z, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(reader, json::array(), Axis::Z, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(reader, json(nullptr), Axis::Z, 0, "T0").has_value());
    // No name to key the object off.
    CHECK_FALSE(to_::read_tool_offset_microns(
                    reader, json{{"tool T0", json{{"gcode_z_offset", -0.1}}}}, Axis::Z, 0, "")
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
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    json status = json{
        {"tool T2",
         json{{"gcode_x_offset", 0.125}, {"gcode_y_offset", -0.05}, {"gcode_z_offset", -0.15}}}};

    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::X, 2, "T2") == 125);
    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::Y, 2, "T2") == -50);
    CHECK(*to_::read_tool_offset_microns(reader, status, Axis::Z, 2, "T2") == -150);
}

TEST_CASE("tool offsets: an axis missing from a delta frame is no news for that axis",
          "[tool_offsets]") {
    // Moonraker republishes only what CHANGED, per field: a frame carrying a
    // new X for a tool says nothing about its Y or Z, and reading either as
    // zero would yank a displayed value to 0 mid-adjustment.
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    json delta = json{{"tool T0", json{{"gcode_x_offset", 0.125}}}};

    CHECK(*to_::read_tool_offset_microns(reader, delta, Axis::X, 0, "T0") == 125);
    CHECK_FALSE(to_::read_tool_offset_microns(reader, delta, Axis::Y, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(reader, delta, Axis::Z, 0, "T0").has_value());
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro writes X and Y to their own variables",
          "[tool_offsets]") {
    // t<n>_off_<axis> at runtime, t<n>_gcode_<axis>_offset in save_variables:
    // the names the controller restores at startup.
    PrinterDiscovery hw = tool_offset_macro_printer();

    CHECK(to_::set_tool_offset_gcode(hw, Axis::X, 1, -50) ==
          "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t1_off_x VALUE=-0.050");
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Y, 2, 125) ==
          "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t2_off_y VALUE=0.125");
    CHECK(to_::save_tool_offset_gcode(hw, Axis::X, 1, -50) ==
          "SAVE_VARIABLE VARIABLE=t1_gcode_x_offset VALUE=-0.050\n"
          "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t1_off_x VALUE=-0.050");
    CHECK(to_::save_tool_offset_gcode(hw, Axis::Y, 3, 10) ==
          "SAVE_VARIABLE VARIABLE=t3_gcode_y_offset VALUE=0.010\n"
          "SET_GCODE_VARIABLE MACRO=TOOL_OFFSET VARIABLE=t3_off_y VALUE=0.010");
}

TEST_CASE("tool offsets: the TOOL_OFFSET macro reads X and Y off the tool index",
          "[tool_offsets]") {
    const to_::OffsetReader reader = to_::resolve_reader(tool_offset_macro_printer());
    json status = json{{"gcode_macro TOOL_OFFSET",
                        json{{"t1_off_x", 0.12}, {"t1_off_y", -0.045}, {"t1_off_z", 0.03}}}};

    CHECK(to_::read_tool_offset_microns(reader, status, Axis::X, 1, "T1") == 120);
    CHECK(to_::read_tool_offset_microns(reader, status, Axis::Y, 1, "T1") == -45);
    CHECK(to_::read_tool_offset_microns(reader, status, Axis::Z, 1, "T1") == 30);
}

TEST_CASE("tool offsets: a stock MedusaHC uses the _TOOL_OFFSET macro",
          "[tool_offsets][medusahc]") {
    // No klipper-toolchanger: the tools are the extruder heaters, and the
    // offset macro carries the `_` prefix the controller resolves first.
    PrinterDiscovery hw = stock_medusahc_printer();

    REQUIRE(to_::supports_per_tool_offsets(hw));
    CHECK(to_::provider_name(hw) == "TOOL_OFFSET macro");
    CHECK(vec_has(to_::required_status_objects(hw), "gcode_macro _TOOL_OFFSET"));
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 1, -50) ==
          "SET_GCODE_VARIABLE MACRO=_TOOL_OFFSET VARIABLE=t1_off_z VALUE=-0.050");

    const to_::OffsetReader reader = to_::resolve_reader(hw);
    CHECK(reader.store_object == "gcode_macro _TOOL_OFFSET");
    json status = json{{"gcode_macro _TOOL_OFFSET", json{{"t2_off_y", 0.2}}}};
    CHECK(to_::read_tool_offset_microns(reader, status, Axis::Y, 2, "T2") == 200);
}

TEST_CASE("tool offsets: _TOOL_OFFSET outranks a plain TOOL_OFFSET beside it",
          "[tool_offsets][medusahc]") {
    // The controller reads `_TOOL_OFFSET` whenever it exists; a leftover
    // unprefixed macro is a store nothing prints with.
    PrinterDiscovery hw = stock_medusahc_printer(/*with_plain_macro_too=*/true);

    CHECK(vec_has(to_::required_status_objects(hw), "gcode_macro _TOOL_OFFSET"));
    CHECK_FALSE(vec_has(to_::required_status_objects(hw), "gcode_macro TOOL_OFFSET"));
    CHECK(to_::save_tool_offset_gcode(hw, Axis::X, 0, 5).find("MACRO=_TOOL_OFFSET ") !=
          std::string::npos);
}

TEST_CASE("tool offsets: the mounted tool's write is applied with MHC_TOOL_OFFSET",
          "[tool_offsets][medusahc]") {
    // The controller applies a tool's offsets only on a swap; MOVE=0 shifts
    // the coordinates without moving the head over the print.
    PrinterDiscovery hw = stock_medusahc_printer();

    CHECK(to_::activate_mounted_offset_gcode(hw, 2, 2) == "MHC_TOOL_OFFSET T=2 MOVE=0");
    // Another tool's offset applied now would shift the head by the wrong
    // correction.
    CHECK(to_::activate_mounted_offset_gcode(hw, 1, 2).empty());
    CHECK(to_::activate_mounted_offset_gcode(hw, 1, -1).empty());
    CHECK(to_::activate_mounted_offset_gcode(hw, -1, -1).empty());
}

TEST_CASE("tool offsets: models that apply a write on the next move need no activation",
          "[tool_offsets][medusahc]") {
    // klipper-toolchanger's transform reads the parameter per move; a
    // TOOL_OFFSET macro without the [medusahc] controller has no command.
    CHECK(to_::activate_mounted_offset_gcode(toolchanger_printer(), 1, 1).empty());
    CHECK(to_::activate_mounted_offset_gcode(tool_offset_macro_printer(), 1, 1).empty());
    CHECK(to_::activate_mounted_offset_gcode(plain_printer(), 0, 0).empty());
}

TEST_CASE("tool offsets: a TOOL_OFFSET frame never answers X or Y from the tool object",
          "[tool_offsets]") {
    // The model is per printer, not per axis: on a machine the macro owns,
    // klipper-toolchanger's copy is not consulted for ANY axis - otherwise X
    // would read from the store this module says is not the authority, while
    // Z reads from the macro.
    const to_::OffsetReader reader = to_::resolve_reader(tool_offset_macro_printer());
    json frame = json{
        {"gcode_macro TOOL_OFFSET", json{{"t0_off_z", -0.20}}},
        {"tool T0", json{{"gcode_x_offset", 0.125}, {"gcode_z_offset", -0.05}}},
    };

    CHECK_FALSE(to_::read_tool_offset_microns(reader, frame, Axis::X, 0, "T0").has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(reader, frame, Axis::Y, 0, "T0").has_value());
    CHECK(*to_::read_tool_offset_microns(reader, frame, Axis::Z, 0, "T0") == -200);
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
    const to_::OffsetReader reader = to_::resolve_reader(toolchanger_printer());
    CHECK_FALSE(to_::read_tool_offset_microns(
                    reader, json{{"tool T0", json{{"gcode_x_offset", "oops"}}}}, Axis::X, 0, "T0")
                    .has_value());
    CHECK_FALSE(to_::read_tool_offset_microns(
                    reader, json{{"tool T0", json{{"gcode_x_offset", nullptr}}}}, Axis::X, 0, "T0")
                    .has_value());
}

// ============================================================================
// Read-only models: offsets the printer measures and keeps itself
// ============================================================================

namespace {

/// A stock INDX printer: the host module plus its macro contract.
PrinterDiscovery indx_printer() {
    PrinterDiscovery hw;
    hw.parse_objects(json::array({"indx", "save_variables", "extruder", "toolhead", "gcode_move",
                                  "gcode_macro TOOL_POSITIONS", "gcode_macro _TOOLCHANGE_VARS",
                                  "gcode_macro CHANGE_TOOL", "gcode_macro PARK_TOOL",
                                  "gcode_macro T0", "gcode_macro T1", "gcode_macro T2"}));
    return hw;
}

json indx_frame() {
    return json{{"save_variables",
                 {{"variables",
                   {{"t1_offset_x", 0.12},
                    {"t1_offset_y", -0.045},
                    {"t1_offset_z", 0.03},
                    {"global_z_offset", -0.05}}}}}};
}

} // namespace

TEST_CASE("tool offsets: INDX offsets are read-only", "[tool_offsets]") {
    auto hw = indx_printer();
    CHECK(to_::reads_axis(hw, Axis::X));
    CHECK(to_::reads_axis(hw, Axis::Y));
    CHECK(to_::reads_axis(hw, Axis::Z));
    CHECK_FALSE(to_::supports_axis(hw, Axis::Z));
    CHECK_FALSE(to_::supports_axis(hw, Axis::X));
    CHECK_FALSE(to_::supports_per_tool_offsets(hw));
    CHECK(to_::set_tool_offset_gcode(hw, Axis::Z, 1, 50).empty());
    CHECK(to_::save_tool_offset_gcode(hw, Axis::Z, 1, 50).empty());
    CHECK(to_::required_status_objects(hw).empty());
    CHECK_FALSE(to_::persist_requires_save_config(hw));

    CHECK_FALSE(to_::reads_axis(plain_printer(), Axis::Z));
    CHECK(to_::reads_axis(toolchanger_printer(), Axis::X));
}

TEST_CASE("tool offsets: INDX reads save_variables", "[tool_offsets]") {
    auto reader = to_::resolve_reader(indx_printer());
    const json frame = indx_frame();
    CHECK(to_::read_tool_offset_microns(reader, frame, Axis::X, 1, "T1") == 120);
    CHECK(to_::read_tool_offset_microns(reader, frame, Axis::Y, 1, "T1") == -45);
    CHECK(to_::read_tool_offset_microns(reader, frame, Axis::Z, 1, "T1") == 30);
    CHECK(to_::read_global_z_offset_microns(reader, frame) == -50);
    CHECK_FALSE(to_::read_tool_offset_microns(reader, frame, Axis::Z, 2, "T2").has_value());

    const json other = {{"extruder", {{"temperature", 200.0}}}};
    CHECK_FALSE(to_::read_tool_offset_microns(reader, other, Axis::Z, 1, "T1").has_value());
    CHECK_FALSE(to_::read_global_z_offset_microns(reader, other).has_value());

    const json text = {
        {"save_variables", {{"variables", {{"t1_offset_x", "0.1"}, {"global_z_offset", "0.1"}}}}}};
    CHECK_FALSE(to_::read_tool_offset_microns(reader, text, Axis::X, 1, "T1").has_value());
    CHECK_FALSE(to_::read_global_z_offset_microns(reader, text).has_value());
}

TEST_CASE("tool offsets: models without a global Z read none", "[tool_offsets]") {
    auto reader = to_::resolve_reader(toolchanger_printer());
    CHECK_FALSE(to_::read_global_z_offset_microns(reader, indx_frame()).has_value());
    CHECK_FALSE(to_::read_global_z_offset_microns(to_::OffsetReader{}, indx_frame()).has_value());
}

TEST_CASE("tool offsets: only klipper-toolchanger's probe offset belongs to T0",
          "[tool_offsets][reference_tool]") {
    // Every move adds the mounted tool's own gcode offsets, measured against T0.
    CHECK(to_::probe_reference_tool(toolchanger_printer()) == std::optional<int>(0));
    CHECK_FALSE(to_::probe_reference_tool(plain_printer()).has_value());
    CHECK_FALSE(to_::probe_reference_tool(tool_offset_macro_printer()).has_value());
    CHECK_FALSE(to_::probe_reference_tool(stock_medusahc_printer()).has_value());
}

// ============================================================================
// Reforge (ff_toolchange): read like klipper-toolchanger, saved by SAVE_CONFIG
// ============================================================================

namespace {

/// A Creator 5 Pro on Reforge: klipper-toolchanger's objects plus ff_toolchange.
PrinterDiscovery reforge_printer() {
    json objects = toolchanger_objects(4);
    objects.push_back("ff_toolchange");
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

} // namespace

TEST_CASE("tool offsets: Reforge is followed and saved by SAVE_CONFIG, never written",
          "[tool_offsets][reforge]") {
    // Reforge registers neither SET_TOOL_PARAMETER nor SAVE_TOOL_PARAMETER,
    // and its offsets are derived from measured nozzle positions, so no axis
    // is editable here. Its calibration stages its own result, which makes
    // the save a bare SAVE_CONFIG: the axes are followed for Save.
    PrinterDiscovery hw = reforge_printer();

    CHECK(to_::provider_name(hw) == "Reforge ff_toolchange");
    CHECK_FALSE(to_::supports_per_tool_offsets(hw));
    CHECK(to_::persist_requires_save_config(hw));
    CHECK(to_::required_status_objects(hw).empty());
    CHECK(to_::probe_reference_tool(hw) == std::optional<int>(0));
    for (Axis axis : helix::kAllAxes) {
        CAPTURE(helix::axis_letter(axis));
        CHECK_FALSE(to_::supports_axis(hw, axis));
        CHECK(to_::reads_axis(hw, axis));
        CHECK(to_::firmware_stages_axis(hw, axis));
        CHECK(to_::set_tool_offset_gcode(hw, axis, 1, -50).empty());
        CHECK(to_::save_tool_offset_gcode(hw, axis, 1, -50).empty());
        CHECK(to_::activate_mounted_offset_gcode(hw, 1, 1).empty());
        // TOOL_Z_ADJUST moves Z live, staged only by SAVE=1; X/Y move only by calibration.
        CHECK(to_::stage_live_offset_gcode(hw, axis, 1) ==
              (axis == Axis::Z ? "TOOL_Z_ADJUST TOOL=1 ADJUST=0 SAVE=1" : ""));
    }
    CHECK(to_::stage_live_offset_gcode(hw, Axis::Z, -1).empty());

    // The derived offsets ride on `tool T<n>`, in klipper-toolchanger's names.
    auto reader = to_::resolve_reader(hw);
    const json frame = {
        {"tool T1",
         {{"gcode_x_offset", -0.1104}, {"gcode_y_offset", 0.0}, {"gcode_z_offset", 3.2}}}};
    CHECK(to_::read_tool_offset_microns(reader, frame, Axis::X, 1, "T1") == -110);
    CHECK(to_::read_tool_offset_microns(reader, frame, Axis::Y, 1, "T1") == 0);
    CHECK(to_::read_tool_offset_microns(reader, frame, Axis::Z, 1, "T1") == 3200);
}

TEST_CASE("tool offsets: only a tool changer running ff_toolchange is Reforge",
          "[tool_offsets][reforge]") {
    CHECK(to_::provider_name(toolchanger_printer()) == "klipper-toolchanger");
    CHECK(to_::provider_name(tool_offset_macro_printer()) == "TOOL_OFFSET macro");
    CHECK(to_::provider_name(indx_printer()) == "INDX save_variables");
    CHECK(to_::provider_name(plain_printer()).empty());

    // ff_toolchange without klipper-toolchanger's objects is no tool changer.
    PrinterDiscovery bare;
    bare.parse_objects(json::array({"gcode_move", "toolhead", "extruder", "ff_toolchange"}));
    CHECK(to_::provider_name(bare).empty());
    CHECK_FALSE(to_::firmware_stages_axis(bare, Axis::Z));
}

TEST_CASE("tool offsets: writable models keep all three axes and stage none", "[tool_offsets]") {
    // klipper-toolchanger's gcode_x/y/z_offset are three fields written by one
    // command, and MedusaHC's controller applies t<n>_off_x/y/z together, so a
    // caller may loop kAllAxes. Neither leaves a write to the firmware. INDX is
    // read-only and persists its own measurements: nothing staged for Save.
    for (Axis axis : helix::kAllAxes) {
        CAPTURE(helix::axis_letter(axis));
        CHECK(to_::supports_axis(toolchanger_printer(), axis));
        CHECK_FALSE(to_::firmware_stages_axis(toolchanger_printer(), axis));
        CHECK(to_::supports_axis(tool_offset_macro_printer(), axis));
        CHECK_FALSE(to_::firmware_stages_axis(tool_offset_macro_printer(), axis));
        CHECK_FALSE(to_::firmware_stages_axis(indx_printer(), axis));
        CHECK_FALSE(to_::supports_axis(plain_printer(), axis));
        CHECK_FALSE(to_::firmware_stages_axis(plain_printer(), axis));
        CHECK(to_::stage_live_offset_gcode(toolchanger_printer(), axis, 1).empty());
        CHECK(to_::stage_live_offset_gcode(tool_offset_macro_printer(), axis, 1).empty());
        CHECK(to_::stage_live_offset_gcode(indx_printer(), axis, 1).empty());
    }
}
