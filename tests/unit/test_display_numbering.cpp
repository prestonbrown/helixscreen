// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_numbering.cpp
 * @brief Unit tests for display_numbering - the single home of index-to-label conversion
 */

#include "display_numbering.h"
#include "lvgl.h"

#include <optional>
#include <vector>

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

TEST_CASE("noun_text covers every LaneNoun", "[numbering]") {
    // A new enumerator with no case falls through to Slot, which would be a
    // silent wrong word rather than a build failure, so pin all six.
    // With no translation loaded lv_tr() returns the key, which is why these
    // are literals: test_filament_mapper.cpp already relies on the same
    // behaviour when it asserts "Slot 3: PLA".
    CHECK(noun_text(LaneNoun::Slot) == "Slot");
    CHECK(noun_text(LaneNoun::Lane) == "Lane");
    CHECK(noun_text(LaneNoun::Gate) == "Gate");
    CHECK(noun_text(LaneNoun::Tool) == "Tool");
    CHECK(noun_text(LaneNoun::Feeder) == "Feeder");
    CHECK(noun_text(LaneNoun::Toolhead) == "Toolhead");
}

TEST_CASE("backup_lane_text names the Endless Spool field in each noun", "[numbering]") {
    CHECK(backup_lane_text(LaneNoun::Slot) == "Backup Slot");
    CHECK(backup_lane_text(LaneNoun::Lane) == "Backup Lane");
    CHECK(backup_lane_text(LaneNoun::Gate) == "Backup Gate");
    CHECK(backup_lane_text(LaneNoun::Tool) == "Backup Tool");
    CHECK(backup_lane_text(LaneNoun::Feeder) == "Backup Feeder");
    CHECK(backup_lane_text(LaneNoun::Toolhead) == "Backup Toolhead");
}

TEST_CASE("lane_range_label spells a span with the plural noun", "[numbering]") {
    CHECK(lane_range_label(LaneNoun::Slot, 0, 3) == "Slots 1-4");
    CHECK(lane_range_label(LaneNoun::Lane, 0, 3) == "Lanes 1-4");
    CHECK(lane_range_label(LaneNoun::Gate, 4, 7) == "Gates 5-8");
    CHECK(lane_range_label(LaneNoun::Tool, 0, 1) == "Tools 1-2");
    CHECK(lane_range_label(LaneNoun::Feeder, 0, 1) == "Feeders 1-2");
    CHECK(lane_range_label(LaneNoun::Toolhead, 0, 1) == "Toolheads 1-2");
}

TEST_CASE("a plural is one fixed word, never agreed with the count", "[numbering]") {
    // Russian numerals take three forms (1 слот / 2-4 слота / 5+ слотов) and a range
    // agrees with neither end of itself, so the header carries the nominative plural
    // whatever the span covers. Pinned on the base locale, where every plural is the
    // key: a span of two and a span of ten read the same word.
    CHECK(lane_range_label(LaneNoun::Slot, 0, 1) == "Slots 1-2");
    CHECK(lane_range_label(LaneNoun::Slot, 0, 9) == "Slots 1-10");
    CHECK(lane_range_label(LaneNoun::Lane, 0, 1).rfind("Lanes ", 0) == 0);
    CHECK(lane_range_label(LaneNoun::Lane, 0, 9).rfind("Lanes ", 0) == 0);
}

TEST_CASE("lane_count_label counts positions in the backend's own noun", "[numbering]") {
    // A unit card on an AFC rig reads "4 lanes", not "4 slots".
    CHECK(lane_count_label(LaneNoun::Slot, 4) == "4 slots");
    CHECK(lane_count_label(LaneNoun::Lane, 4) == "4 lanes");
    CHECK(lane_count_label(LaneNoun::Gate, 12) == "12 gates");
    CHECK(lane_count_label(LaneNoun::Tool, 2) == "2 tools");
    CHECK(lane_count_label(LaneNoun::Feeder, 4) == "4 feeders");
    CHECK(lane_count_label(LaneNoun::Toolhead, 4) == "4 toolheads");
}

TEST_CASE("a position count is a fixed form, never agreed with the count", "[numbering]") {
    // The count is interpolated and the word is not chosen from it, so one and
    // many read the same word in the base locale. A locale whose numerals
    // inflect picks the one form it wants to see on a card header.
    CHECK(lane_count_label(LaneNoun::Lane, 1) == "1 lanes");
    CHECK(lane_count_label(LaneNoun::Lane, 0) == "0 lanes");
}

TEST_CASE("a range with no valid end produces no label", "[numbering]") {
    // Same contract as lane_label(): no position, no text, rather than a range
    // running off a sentinel.
    CHECK(lane_range_label(LaneNoun::Slot, -1, 3).empty());
    CHECK(lane_range_label(LaneNoun::Slot, 0, -1).empty());
}

TEST_CASE("a range that runs backwards produces no label", "[numbering]") {
    // The span is ordered low to high. A caller handing the ends over reversed
    // has no span to show, and "Slots 6-3" reads as a real one.
    CHECK(lane_range_label(LaneNoun::Slot, 5, 2).empty());
    CHECK(lane_range_label(LaneNoun::Gate, 1, 0).empty());
    // One below the boundary on each side still spans.
    CHECK(lane_range_label(LaneNoun::Gate, 0, 1) == "Gates 1-2");
}

TEST_CASE("a one-position span is spelled as one position", "[numbering]") {
    // Not "Slots 3-3": a plural header over a single position is wrong in every
    // locale that inflects, and the range is degenerate rather than absent.
    CHECK(lane_range_label(LaneNoun::Slot, 2, 2) == "Slot 3");
    CHECK(lane_range_label(LaneNoun::Lane, 0, 0) == "Lane 1");
    CHECK(lane_range_label(LaneNoun::Toolhead, 3, 3) == "Toolhead 4");
}

TEST_CASE("is_generated_tool_name separates a gcode identity from a chosen name", "[numbering]") {
    // The T<n> form tool_label() produces may be renumbered for display; a name
    // its owner wrote into printer.cfg is what the machine is labeled with.
    CHECK(is_generated_tool_name("T0"));
    CHECK(is_generated_tool_name("T15"));
    CHECK(is_generated_tool_name(tool_label(7)));
    CHECK_FALSE(is_generated_tool_name("Left"));
    CHECK_FALSE(is_generated_tool_name("T"));
    CHECK_FALSE(is_generated_tool_name(""));
    CHECK_FALSE(is_generated_tool_name("t0"));
    CHECK_FALSE(is_generated_tool_name("T0a"));
    CHECK_FALSE(is_generated_tool_name("Tool 1"));
}

TEST_CASE("Feeder and Toolhead compose like every other noun", "[numbering]") {
    // Snapmaker U1 is the one backend where the filament-entry noun and the
    // printing-end noun differ, so both need the ordinary lane_label() path.
    CHECK(lane_label(LaneNoun::Feeder, 0) == "Feeder 1");
    CHECK(lane_label(LaneNoun::Toolhead, 0) == "Toolhead 1");
    CHECK(lane_label(LaneNoun::Feeder, 3) == "Feeder 4");
    CHECK(lane_label(LaneNoun::Toolhead, 3) == "Toolhead 4");
}

TEST_CASE("tool badges hide when every tool feeds the lane of its own number",
          "[numbering][tool_badge]") {
    // Index space: T0 -> slot 0 is identity, though slot 0 displays as lane 1.
    CHECK(lane_number(0) == 1);
    CHECK_FALSE(tool_badges_add_information({0, 1, 2, 3}, 4));
    CHECK_FALSE(tool_badges_add_information({0, 1, 2, 3, 4, 5, 6, 7}, 8));
    // Fewer tools than lanes: the spare lanes carry no tool, so no badge repeats anything.
    CHECK_FALSE(tool_badges_add_information({0, 1}, 4));
}

TEST_CASE("tool badges show when a tool feeds another lane", "[numbering][tool_badge]") {
    CHECK(tool_badges_add_information({1, 0, 2, 3}, 4));
    // Off by one in display terms (T0 -> lane 1 shifted to T0 -> slot 1) is a remap.
    CHECK(tool_badges_add_information({1, 2, 3, 0}, 4));
    CHECK(tool_badges_add_information({0, 1, 2, 2}, 4));
    // Only the last tool moved: every entry is checked, not just the first.
    CHECK(tool_badges_add_information({0, 1, 2, 3, 4, 5, 6, 0}, 8));
}

TEST_CASE("tool badges show when a tool has no lane", "[numbering][tool_badge]") {
    CHECK(tool_badges_add_information({0, -1, 2, 3}, 4));
    // T4 names a lane this system does not have.
    CHECK(tool_badges_add_information({0, 1, 2, 3, 4}, 4));
}

TEST_CASE("an empty tool map is not proof of identity", "[numbering][tool_badge]") {
    CHECK(tool_badges_add_information({}, 4));
    CHECK(tool_badges_add_information({}, 0));
}

TEST_CASE("a generated tool name shows its display number beside the G-code name",
          "[numbering][1769]") {
    // Klipper and the slicer say T1 for the second tool; the machine says Tool 2.
    // Where there is room the label carries both, so neither reads as a typo.
    CHECK(tool_label_in_form(LaneNoun::Tool, "T0", 0, ToolLabelForm::Full) == "Tool 1 (T0)");
    CHECK(tool_label_in_form(LaneNoun::Tool, "T1", 1, ToolLabelForm::Full) == "Tool 2 (T1)");
    CHECK(tool_label_in_form(LaneNoun::Tool, "T1", 1, ToolLabelForm::Code) == "T1");
    CHECK(tool_label_in_form(LaneNoun::Tool, tool_label(11), 11, ToolLabelForm::Code) == "T11");
}

TEST_CASE("the full tool label speaks the printer's own noun", "[numbering][1769]") {
    CHECK(tool_label_in_form(LaneNoun::Toolhead, "T2", 2, ToolLabelForm::Full) ==
          "Toolhead 3 (T2)");
    CHECK(tool_label_in_form(LaneNoun::Toolhead, "T2", 2, ToolLabelForm::Code) == "T2");
}

TEST_CASE("a configured tool name is its label, beside its G-code name", "[numbering][1769]") {
    // "Left" is what the machine is labeled with; the narrow form is the
    // G-code name it answers to, as for a generated name.
    CHECK(tool_label_in_form(LaneNoun::Tool, "Left", 0, ToolLabelForm::Full) == "Left (T0)");
    CHECK(tool_label_in_form(LaneNoun::Tool, "Right", 1, ToolLabelForm::Full) == "Right (T1)");
    CHECK(tool_label_in_form(LaneNoun::Tool, "Left", 0, ToolLabelForm::Code) == "T0");
    CHECK(tool_label_in_form(LaneNoun::Tool, "Right", 1, ToolLabelForm::Code) == "T1");
    // The noun names a position, which a configured name replaces.
    CHECK(tool_label_in_form(LaneNoun::Toolhead, "Left", 0, ToolLabelForm::Full) == "Left (T0)");
    // Not the generated spelling: lowercase, or a T with no digits after it.
    CHECK(tool_label_in_form(LaneNoun::Tool, "t0", 0, ToolLabelForm::Full) == "t0 (T0)");
    CHECK(tool_label_in_form(LaneNoun::Tool, "t0", 0, ToolLabelForm::Code) == "T0");
    CHECK(tool_label_in_form(LaneNoun::Tool, "T", 0, ToolLabelForm::Full) == "T (T0)");
}

TEST_CASE("tool_name_label is the configured name, else the physical position",
          "[numbering][1769]") {
    CHECK(tool_name_label(LaneNoun::Tool, "Left", 0) == "Left");
    CHECK(tool_name_label(LaneNoun::Tool, "T0", 0) == "Tool 1");
    CHECK(tool_name_label(LaneNoun::Toolhead, "T2", 2) == "Toolhead 3");
    CHECK(tool_name_label(LaneNoun::Tool, "Left", -1).empty());
    CHECK(tool_name_label(LaneNoun::Tool, "T0", -1).empty());
}

TEST_CASE("a tool with no position has no label in any form", "[numbering][1769]") {
    CHECK(tool_label_in_form(LaneNoun::Tool, "T0", -1, ToolLabelForm::Full).empty());
    CHECK(tool_label_in_form(LaneNoun::Tool, "T0", -1, ToolLabelForm::Code).empty());
    CHECK(tool_label_in_form(LaneNoun::Tool, "Left", -1, ToolLabelForm::Full).empty());
}

TEST_CASE("pick_tool_label_form takes the most informative form that fits", "[numbering][1769]") {
    std::vector<ToolLabelForm> asked;
    auto fits_only = [&asked](std::optional<ToolLabelForm> accepted) {
        asked.clear();
        return [&asked, accepted](ToolLabelForm f) {
            asked.push_back(f);
            return accepted && f == *accepted;
        };
    };

    SECTION("room for the full label") {
        CHECK(pick_tool_label_form([](ToolLabelForm) { return true; }) == ToolLabelForm::Full);
    }
    SECTION("only the G-code name fits") {
        CHECK(pick_tool_label_form(fits_only(ToolLabelForm::Code)) == ToolLabelForm::Code);
        // Full was tried first and refused; Code was then asked.
        CHECK(asked == std::vector<ToolLabelForm>{ToolLabelForm::Full, ToolLabelForm::Code});
    }
    SECTION("nothing fits: the shortest form, never nothing") {
        CHECK(pick_tool_label_form(fits_only(std::nullopt)) == ToolLabelForm::Code);
    }
    SECTION("the first accepted form wins without asking the rest") {
        CHECK(pick_tool_label_form(fits_only(ToolLabelForm::Full)) == ToolLabelForm::Full);
        CHECK(asked == std::vector<ToolLabelForm>{ToolLabelForm::Full});
    }
}
