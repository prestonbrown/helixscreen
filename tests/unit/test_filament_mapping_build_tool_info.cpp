// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_mapping_build_tool_info.cpp
 * @brief Unit tests for FilamentMappingCard::build_tool_info (the stateless
 *        per-tool-info assembler).
 *
 * build_tool_info is the single source that derives per-tool GcodeToolInfo from
 * the slicer palette (colors + materials). It was promoted to a public static
 * method so pre-flight validation and the remap modal can source per-tool info
 * directly from current_filament_colors_/materials — the same Moonraker-metadata
 * data the swatches use, populated on ALL platforms — instead of from the
 * FilamentMappingCard INSTANCE's tool_info_, which is empty on the U1/headless
 * path (the "0 tools" → "Nothing to remap" coupling bug this decoupling fixes).
 *
 * These tests pin the assembler's contract: one entry per tool, tool_index = i,
 * color parsed from hex (grey fallback on empty/invalid), material from the
 * materials vector, count = max(colors, materials).
 */

#include "ui_filament_mapping_card.h"

#include "../catch_amalgamated.hpp"

using helix::ui::FilamentMappingCard;

TEST_CASE("build_tool_info: empty inputs produce no tools", "[filament][mapping][tool_info]") {
    auto tools = FilamentMappingCard::build_tool_info({}, {});
    CHECK(tools.empty());
}

TEST_CASE("build_tool_info: one entry per tool with sequential indices",
          "[filament][mapping][tool_info]") {
    auto tools = FilamentMappingCard::build_tool_info({"#FF0000", "#00FF00", "#0000FF"},
                                                      {"PLA", "PETG", "ABS"});
    REQUIRE(tools.size() == 3);
    CHECK(tools[0].tool_index == 0);
    CHECK(tools[1].tool_index == 1);
    CHECK(tools[2].tool_index == 2);
    CHECK(tools[0].color_rgb == 0xFF0000);
    CHECK(tools[1].color_rgb == 0x00FF00);
    CHECK(tools[2].color_rgb == 0x0000FF);
    CHECK(tools[0].material == "PLA");
    CHECK(tools[1].material == "PETG");
    CHECK(tools[2].material == "ABS");
}

TEST_CASE("build_tool_info: count is max(colors, materials)", "[filament][mapping][tool_info]") {
    SECTION("more colors than materials") {
        auto tools =
            FilamentMappingCard::build_tool_info({"#111111", "#222222", "#333333"}, {"PLA"});
        REQUIRE(tools.size() == 3);
        CHECK(tools[0].material == "PLA");
        CHECK(tools[1].material.empty());
        CHECK(tools[2].material.empty());
    }
    SECTION("more materials than colors") {
        auto tools = FilamentMappingCard::build_tool_info({"#111111"}, {"PLA", "PETG"});
        REQUIRE(tools.size() == 2);
        CHECK(tools[0].color_rgb == 0x111111);
        // Tool 1 has no color → grey fallback.
        CHECK(tools[1].color_rgb == 0x808080);
        CHECK(tools[1].material == "PETG");
    }
}

TEST_CASE("build_tool_info: empty or invalid color falls back to grey",
          "[filament][mapping][tool_info]") {
    auto tools = FilamentMappingCard::build_tool_info({"", "not-a-color"}, {"PLA", "PETG"});
    REQUIRE(tools.size() == 2);
    CHECK(tools[0].color_rgb == 0x808080);
    CHECK(tools[1].color_rgb == 0x808080);
}

// --- Unknown colors are not a color -----------------------------------------
//
// Moonraker does not always report filament_colors. Every OrcaSlicer file on a
// K2 Plus comes back with filament_type and no color key at all, so the palette
// arrives empty and count is driven entirely by the materials vector. The
// assembler still has to produce one entry per tool (the card is keyed off
// them), but the color it puts there is a stand-in, not the file's answer.
//
// Flagging that is what keeps a stand-in from being read as a claim: the
// mapping pill paints a solid grey dot from color_rgb, and compute_defaults
// runs a nearest-color search against the real lane colors. Both were acting on
// 0x808080 as though the slicer had chosen it. (The regression this pins: a
// re-opened 133MB print showed "grey dot -> black dot" on a K2 Plus.)

TEST_CASE("build_tool_info: a tool with no palette entry is color-unknown",
          "[filament][mapping][tool_info]") {
    auto tools = FilamentMappingCard::build_tool_info({}, {"PLA", "ASA", "ASA"});
    REQUIRE(tools.size() == 3);
    for (const auto& t : tools) {
        CHECK_FALSE(t.color_known);
    }
    // The materials still land — they are the half Moonraker DID report.
    CHECK(tools[0].material == "PLA");
    CHECK(tools[1].material == "ASA");
}

TEST_CASE("build_tool_info: a real color is color-known", "[filament][mapping][tool_info]") {
    auto tools = FilamentMappingCard::build_tool_info({"#FF0000", "#00FF00"}, {"PLA", "PETG"});
    REQUIRE(tools.size() == 2);
    CHECK(tools[0].color_known);
    CHECK(tools[1].color_known);
}

TEST_CASE("build_tool_info: an empty or unparsable entry is color-unknown",
          "[filament][mapping][tool_info]") {
    // A slicer that emits `filament_colour = ;#00FF00;` leaves real gaps in an
    // otherwise usable palette — per-tool, not all-or-nothing.
    auto tools = FilamentMappingCard::build_tool_info({"", "#00FF00", "not-a-color"},
                                                      {"PLA", "PETG", "ABS"});
    REQUIRE(tools.size() == 3);
    CHECK_FALSE(tools[0].color_known);
    CHECK(tools[1].color_known);
    CHECK(tools[1].color_rgb == 0x00FF00);
    CHECK_FALSE(tools[2].color_known);
}
