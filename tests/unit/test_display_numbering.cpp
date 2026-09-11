// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_numbering.cpp
 * @brief Unit tests for display_numbering - the single home of index-to-label conversion
 */

#include "display_numbering.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

TEST_CASE("tool_label spells a gcode tool 0-based", "[numbering]") {
    CHECK(tool_label(0) == "T0");
    CHECK(tool_label(1) == "T1");
    CHECK(tool_label(15) == "T15");
}

TEST_CASE("lane_number is the only + 1", "[numbering]") {
    CHECK(lane_number(0) == 1);
    CHECK(lane_number(3) == 4);
    CHECK(lane_number_text(0) == "1");
    CHECK(lane_number_text(3) == "4");
}

TEST_CASE("lane_label composes noun and number", "[numbering]") {
    CHECK(lane_label(LaneNoun::Slot, 0) == "Slot 1");
    CHECK(lane_label(LaneNoun::Lane, 1) == "Lane 2");
    CHECK(lane_label(LaneNoun::Gate, 2) == "Gate 3");
}

TEST_CASE("LaneNoun::Tool is an ordinary noun, never T<n>", "[numbering]") {
    // A tool changer's positions are physical, so they count from 1 like any
    // other position. The T<n> spelling belongs to the gcode domain only.
    CHECK(lane_label(LaneNoun::Tool, 0) == "Tool 1");
    CHECK(lane_label(LaneNoun::Tool, 3) == "Tool 4");
    CHECK(lane_label(LaneNoun::Tool, 0) != tool_label(0));
}

TEST_CASE("lane_label with a unit prefixes the unit name", "[numbering]") {
    CHECK(lane_label(LaneNoun::Slot, "Turtle 1", 1) == "Turtle 1 \xc2\xb7 Slot 2");
    // An empty unit name degrades to the single-unit form rather than emitting
    // a leading separator.
    CHECK(lane_label(LaneNoun::Slot, "", 1) == "Slot 2");
}

TEST_CASE("out-of-range indices do not produce a label", "[numbering]") {
    // A negative index means "no lane". Callers must not paint "Slot 0".
    CHECK(lane_number(-1) == -1);
    CHECK(lane_number_text(-1).empty());
    CHECK(lane_label(LaneNoun::Slot, -1).empty());
    CHECK(lane_label(LaneNoun::Slot, "Turtle 1", -1).empty());
    CHECK(tool_label(-1).empty());
}
