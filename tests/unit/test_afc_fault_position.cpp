// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_afc_fault_position.cpp
 * @brief The AFC fault-message -> filament-position contract (#1184).
 *
 * Run with: ./build/bin/helix-tests "[afc][fault][position]"
 *
 * Every message below is VERBATIM from the AFC-Klipper-Add-On source read off a live
 * BoxTurtle — the five (and only five) sites that ship a position diagram:
 *
 *   AFC.py:1294, AFC.py:1345, AFC.py:1370, AFC.py:1469, AFC_BoxTurtle.py:527
 *
 * The art is a hardcoded literal per site, so we map the SENTENCE, never the bars.
 * The two toolhead-sensor sites emit byte-identical art for different faults — that
 * disambiguation is the whole reason this module exists, and it is asserted below.
 *
 * The contract is documented in
 * `docs/devel/FILAMENT_MANAGEMENT.md` § "AFC console response contract".
 *
 * Mutation checks (each must break the listed test):
 *   - collapse the pre/post extruder-gear rules onto one segment
 *     -> "identical art, different positions" fails
 *   - make afc_strip_position_diagram() strip unconditionally (drop the
 *     afc_fault_position() gate) -> "an unrecognised message is returned untouched" fails
 *   - drop the word-boundary check in contains_word()
 *     -> "unrelated console text is not a fault position" fails on the filename line
 */

#include "afc_fault_position.h"
#include "fault_path_position.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::LaneSensor;
using helix::afc::afc_fault_lane_sensor;
using helix::afc::afc_fault_position;
using helix::afc::afc_strip_position_diagram;
using helix::faultpath::lane_sensor_at;

namespace {

// ---------------------------------------------------------------------------
// The five producers, verbatim (Python literals rendered with real newlines).
// ---------------------------------------------------------------------------

// AFC.py:1294
const std::string kMsgHub = "filament did not trigger hub sensor, CHECK FILAMENT PATH\n"
                            "||=====||==>--||-----||\n"
                            "TRG   LOAD   HUB   TOOL.";

// AFC.py:1345
const std::string kMsgPreGear =
    "filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH\n"
    "||=====||====||==>--||\n"
    "TRG   LOAD   HUB   TOOL";

// AFC.py:1370
const std::string kMsgPostGear =
    "filament failed to trigger post extruder gear toolhead sensor, CHECK FILAMENT PATH\n"
    "||=====||====||==>--||\n"
    "TRG   LOAD   HUB   TOOL";

// AFC.py:1469
const std::string kMsgNotLoaded = "Current lane not loaded, LOAD TRIGGER NOT TRIGGERED\n"
                                  "||==>--||----||-----||\n"
                                  "TRG   LOAD   HUB   TOOL";

// AFC_BoxTurtle.py:527
const std::string kMsgTrigger = " FAILED TO LOAD, CHECK FILAMENT AT TRIGGER\n"
                                "||==>--||----||------||\n"
                                "TRG   LOAD   HUB    TOOL";

/// As it really arrives: AFC prefixes the lane name onto the sentence.
const std::string kMsgPreGearWithLane =
    "lane1 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH\n"
    "||=====||====||==>--||\n"
    "TRG   LOAD   HUB   TOOL";

} // namespace

// ============================================================================
// Mapping
// ============================================================================

TEST_CASE("every AFC diagram-bearing message maps to a position", "[afc][fault][position]") {
    struct Case {
        const char* name;
        const std::string* message;
        helix::PathSegment expected;
    };

    // Table-driven over all five real producers plus the lane-prefixed form.
    const Case cases[] = {
        {"AFC.py:1469 lane not loaded", &kMsgNotLoaded, helix::PathSegment::SPOOL},
        {"AFC_BoxTurtle.py:527 failed at trigger", &kMsgTrigger, helix::PathSegment::SPOOL},
        {"AFC.py:1294 no hub sensor", &kMsgHub, helix::PathSegment::HUB},
        {"AFC.py:1345 pre extruder gear", &kMsgPreGear, helix::PathSegment::OUTPUT},
        {"AFC.py:1370 post extruder gear", &kMsgPostGear, helix::PathSegment::TOOLHEAD},
        {"AFC.py:1345 with lane prefix", &kMsgPreGearWithLane, helix::PathSegment::OUTPUT},
    };

    for (const auto& c : cases) {
        INFO(c.name);
        auto got = afc_fault_position(*c.message);
        REQUIRE(got.has_value());
        REQUIRE(static_cast<int>(*got) == static_cast<int>(c.expected));
    }
}

TEST_CASE("identical art, different positions", "[afc][fault][position]") {
    // Both sites emit the byte-identical bar row `||=====||====||==>--||`. A parser
    // over the art cannot tell them apart; the sentence can, and must.
    REQUIRE(kMsgPreGear.find("||=====||====||==>--||") != std::string::npos);
    REQUIRE(kMsgPostGear.find("||=====||====||==>--||") != std::string::npos);

    auto pre = afc_fault_position(kMsgPreGear);
    auto post = afc_fault_position(kMsgPostGear);
    REQUIRE(pre.has_value());
    REQUIRE(post.has_value());
    REQUIRE(static_cast<int>(*pre) != static_cast<int>(*post));
    REQUIRE(*pre == helix::PathSegment::OUTPUT);    // past hub, short of the toolhead
    REQUIRE(*post == helix::PathSegment::TOOLHEAD); // at toolhead, short of the gears
}

TEST_CASE("the `!!` prefix and case do not change the position", "[afc][fault][position]") {
    REQUIRE(afc_fault_position("!! lane1 filament did not trigger hub sensor, CHECK FILAMENT PATH")
                .value() == helix::PathSegment::HUB);
    REQUIRE(afc_fault_position("LANE1 FILAMENT DID NOT TRIGGER HUB SENSOR").value() ==
            helix::PathSegment::HUB);
    REQUIRE(
        afc_fault_position("lane1 Current lane not loaded, load trigger not triggered").value() ==
        helix::PathSegment::SPOOL);
}

TEST_CASE("unrelated console text is not a fault position", "[afc][fault][position]") {
    // The bare console channel carries user-controlled filenames and open-ended
    // chatter; none of it may resolve to a position.
    const char* noise[] = {
        "",
        "lane1 is now loaded in toolhead t:0",
        "Tool Change - lane1 -> lane2",
        "B:60.0 /60.0 T0:215.1 /215.0",
        "File opened: check filament at triggering.gcode Size: 91234",
        "// AFC_Brush: Clean Nozzle",
        "!! Extruder temperature 21.0C below minimum extrude temperature 170.0C",
        "hub sensor triggered",
        // Word-boundary guard: the needle only counts as whole words.
        "reload trigger not triggeredness detected",
    };

    for (const char* line : noise) {
        INFO(line);
        REQUIRE_FALSE(afc_fault_position(line).has_value());
    }
}

// ============================================================================
// Stripping
// ============================================================================

TEST_CASE("the art rows are stripped and the sentence survives byte-for-byte",
          "[afc][fault][position]") {
    REQUIRE(afc_strip_position_diagram(kMsgHub) ==
            "filament did not trigger hub sensor, CHECK FILAMENT PATH");
    REQUIRE(afc_strip_position_diagram(kMsgPreGear) ==
            "filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH");
    REQUIRE(afc_strip_position_diagram(kMsgPostGear) ==
            "filament failed to trigger post extruder gear toolhead sensor, CHECK FILAMENT PATH");
    REQUIRE(afc_strip_position_diagram(kMsgNotLoaded) ==
            "Current lane not loaded, LOAD TRIGGER NOT TRIGGERED");
    // Leading space preserved exactly as AFC_BoxTurtle.py emits it.
    REQUIRE(afc_strip_position_diagram(kMsgTrigger) ==
            " FAILED TO LOAD, CHECK FILAMENT AT TRIGGER");
    REQUIRE(afc_strip_position_diagram(kMsgPreGearWithLane) ==
            "lane1 filament failed to trigger pre extruder gear toolhead sensor, CHECK "
            "FILAMENT PATH");

    // No stray newline is left where the art used to be.
    for (const std::string* m : {&kMsgHub, &kMsgPreGear, &kMsgPostGear, &kMsgNotLoaded,
                                 &kMsgTrigger, &kMsgPreGearWithLane}) {
        INFO(*m);
        REQUIRE(afc_strip_position_diagram(*m).find('\n') == std::string::npos);
    }
}

TEST_CASE("an unrecognised message is returned untouched", "[afc][fault][position]") {
    // The safety property: anything we did not recognise is not ours to edit, even
    // when it happens to contain something art-shaped.
    const char* untouched[] = {
        "!! Move out of range: 300.000 0.000 10.000 [0.000]",
        "Printer is shutdown\n||=====||====||==>--||\nTRG   LOAD   HUB   TOOL",
        "some future AFC wording we have never seen\n||==>--||----||-----||",
        "",
        "a plain sentence",
    };

    for (const char* line : untouched) {
        INFO(line);
        REQUIRE(afc_strip_position_diagram(line) == std::string(line));
    }
}

TEST_CASE("stripping never eats a sentence that merely mentions the labels",
          "[afc][fault][position]") {
    // A recognised fault whose sentence contains the label words must keep them.
    const std::string msg = "lane1 filament did not trigger hub sensor, CHECK FILAMENT PATH\n"
                            "TRG is the trigger, LOAD is the lane, HUB is the hub, TOOL is the "
                            "toolhead\n"
                            "||=====||==>--||-----||\n"
                            "TRG   LOAD   HUB   TOOL.";
    const std::string out = afc_strip_position_diagram(msg);
    REQUIRE(out == "lane1 filament did not trigger hub sensor, CHECK FILAMENT PATH\n"
                   "TRG is the trigger, LOAD is the lane, HUB is the hub, TOOL is the toolhead");
}

TEST_CASE("CRLF art rows are still recognised", "[afc][fault][position]") {
    const std::string msg = "filament did not trigger hub sensor, CHECK FILAMENT PATH\r\n"
                            "||=====||==>--||-----||\r\n"
                            "TRG   LOAD   HUB   TOOL.";
    REQUIRE(afc_strip_position_diagram(msg) ==
            "filament did not trigger hub sensor, CHECK FILAMENT PATH\r");
}

// ============================================================================
// The lane sensor at fault
// ============================================================================

TEST_CASE("AFC's two lane load faults are at the load sensor", "[afc][fault][sensor]") {
    // Both fire with the prep switch made and the load switch never reached.
    CHECK(afc_fault_lane_sensor(kMsgTrigger) == LaneSensor::LOAD);
    CHECK(afc_fault_lane_sensor(kMsgNotLoaded) == LaneSensor::LOAD);
    // Faults past the lane are at no lane sensor.
    CHECK(afc_fault_lane_sensor(kMsgHub) == LaneSensor::NONE);
    CHECK(afc_fault_lane_sensor(kMsgPreGear) == LaneSensor::NONE);
    CHECK(afc_fault_lane_sensor(kMsgPostGear) == LaneSensor::NONE);
}

TEST_CASE("a lane fault reaches only the lane its lines name", "[afc][fault][sensor]") {
    const std::vector<std::string> lanes{"lane1", "lane2", "lane10"};
    const std::string msg = "lane1" + kMsgTrigger;
    CHECK(lane_sensor_at(msg, "lane1", false, lanes) == LaneSensor::LOAD);
    // Not lane2's, even as the current lane, and not a lane whose name merely contains it.
    CHECK(lane_sensor_at(msg, "lane2", true, lanes) == LaneSensor::NONE);
    CHECK(lane_sensor_at("lane10" + kMsgTrigger, "lane1", false, lanes) == LaneSensor::NONE);
    // Several lanes, one line each: each lane reads its own line.
    const std::string both = "lane1 filament did not trigger hub sensor\nlane2" + kMsgTrigger;
    CHECK(lane_sensor_at(both, "lane1", false, lanes) == LaneSensor::NONE);
    CHECK(lane_sensor_at(both, "lane2", false, lanes) == LaneSensor::LOAD);
}

TEST_CASE("a lane fault naming no lane is the current lane's", "[afc][fault][sensor]") {
    const std::vector<std::string> lanes{"lane1", "lane2"};
    CHECK(lane_sensor_at(kMsgNotLoaded, "lane2", true, lanes) == LaneSensor::LOAD);
    CHECK(lane_sensor_at(kMsgNotLoaded, "lane1", false, lanes) == LaneSensor::NONE);
}
