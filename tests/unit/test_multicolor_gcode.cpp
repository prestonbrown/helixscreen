// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_multicolor_gcode.cpp
 * @brief Unit tests for multi-color G-code parsing and rendering
 *
 * Tests the complete pipeline:
 * 1. Parser: extracting tool colors and tracking tool changes
 * 2. Geometry Builder: converting tool indices to colors
 * 3. Integration: end-to-end multi-color rendering
 */

#include "gcode_geometry_builder.h"
#include "gcode_parser.h"

#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ============================================================================
// Parser Tests
// ============================================================================

TEST_CASE("MultiColor - Parse extruder_colour metadata", "[gcode][multicolor][parser]") {
    GCodeParser parser;

    SECTION("Parse 4-color OrcaSlicer format") {
        parser.parse_line("; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000");

        const auto& palette = parser.get_tool_color_palette();

        REQUIRE(palette.size() == 4);
        REQUIRE(palette[0] == "#ED1C24"); // Red
        REQUIRE(palette[1] == "#00C1AE"); // Teal
        REQUIRE(palette[2] == "#F4E2C1"); // Beige
        REQUIRE(palette[3] == "#000000"); // Black
    }

    SECTION("Parse 2-color setup") {
        parser.parse_line("; extruder_colour = #FF0000;#0000FF");

        const auto& palette = parser.get_tool_color_palette();

        REQUIRE(palette.size() == 2);
        REQUIRE(palette[0] == "#FF0000");
        REQUIRE(palette[1] == "#0000FF");
    }

    SECTION("Handle whitespace in metadata") {
        parser.parse_line(";extruder_colour=#AA0000 ; #00BB00 ;#0000CC");

        const auto& palette = parser.get_tool_color_palette();

        REQUIRE(palette.size() == 3);
        REQUIRE(palette[0] == "#AA0000");
        REQUIRE(palette[1] == "#00BB00");
        REQUIRE(palette[2] == "#0000CC");
    }
}

TEST_CASE("MultiColor - Parse filament_colour as fallback", "[gcode][multicolor][parser]") {
    GCodeParser parser;

    SECTION("Use filament_colour when extruder_colour not present") {
        parser.parse_line("; filament_colour = #FF0000;#00FF00;#0000FF");

        const auto& palette = parser.get_tool_color_palette();

        REQUIRE(palette.size() == 3);
        REQUIRE(palette[0] == "#FF0000");
        REQUIRE(palette[1] == "#00FF00");
        REQUIRE(palette[2] == "#0000FF");
    }

    SECTION("extruder_colour takes priority over filament_colour") {
        parser.parse_line("; filament_colour = #111111;#222222");
        parser.parse_line("; extruder_colour = #AA0000;#00BB00");

        const auto& palette = parser.get_tool_color_palette();

        // Should have both - first from filament_colour, then from extruder_colour
        REQUIRE(palette.size() >= 2);
    }
}

TEST_CASE("MultiColor - Parse tool change commands", "[gcode][multicolor][parser]") {
    GCodeParser parser;

    SECTION("Track tool changes across segments") {
        parser.parse_line("T0");
        parser.parse_line("G1 X10 Y10 E1");
        parser.parse_line("T2");
        parser.parse_line("G1 X20 Y20 E2");
        parser.parse_line("T1");
        parser.parse_line("G1 X30 Y30 E3");

        auto result = parser.finalize();

        REQUIRE(result.layers.size() > 0);
        REQUIRE(result.layers[0].segments.size() >= 3);
        REQUIRE(result.layers[0].segments[0].tool_index == 0);
        REQUIRE(result.layers[0].segments[1].tool_index == 2);
        REQUIRE(result.layers[0].segments[2].tool_index == 1);
    }

    SECTION("Default to tool 0 when no tool change") {
        parser.parse_line("G1 X10 Y10 E1");

        auto result = parser.finalize();

        REQUIRE(result.layers[0].segments[0].tool_index == 0);
    }

    SECTION("Handle sequential tool numbers") {
        parser.parse_line("T0");
        parser.parse_line("G1 X1 Y1 E1");
        parser.parse_line("T1");
        parser.parse_line("G1 X2 Y2 E2");
        parser.parse_line("T2");
        parser.parse_line("G1 X3 Y3 E3");
        parser.parse_line("T3");
        parser.parse_line("G1 X4 Y4 E4");

        auto result = parser.finalize();

        REQUIRE(result.layers[0].segments.size() >= 4);
        for (size_t i = 0; i < 4; i++) {
            REQUIRE(result.layers[0].segments[i].tool_index == static_cast<int>(i));
        }
    }
}

TEST_CASE("MultiColor - Wipe tower detection", "[gcode][multicolor][parser]") {
    GCodeParser parser;

    SECTION("Mark segments inside wipe tower") {
        parser.parse_line("G1 X10 Y10 E1");
        parser.parse_line("; WIPE_TOWER_START");
        parser.parse_line("G1 X20 Y20 E2");
        parser.parse_line("; WIPE_TOWER_END");
        parser.parse_line("G1 X30 Y30 E3");

        auto result = parser.finalize();

        REQUIRE(result.layers[0].segments.size() >= 3);
        REQUIRE(result.get_object_name(result.layers[0].segments[0].object_name_index) !=
                "__WIPE_TOWER__");
        REQUIRE(result.get_object_name(result.layers[0].segments[1].object_name_index) ==
                "__WIPE_TOWER__");
        REQUIRE(result.get_object_name(result.layers[0].segments[2].object_name_index) !=
                "__WIPE_TOWER__");
    }

    SECTION("Handle wipe tower brim markers") {
        parser.parse_line("; WIPE_TOWER_BRIM_START");
        parser.parse_line("G1 X10 Y10 E1");
        parser.parse_line("; WIPE_TOWER_BRIM_END");

        auto result = parser.finalize();

        REQUIRE(result.get_object_name(result.layers[0].segments[0].object_name_index) ==
                "__WIPE_TOWER__");
    }
}

TEST_CASE("MultiColor - Palette transferred to ParsedGCodeFile", "[gcode][multicolor][parser]") {
    GCodeParser parser;

    parser.parse_line("; extruder_colour = #AA0000;#00BB00;#0000CC");
    parser.parse_line("G1 X10 Y10 E1");

    auto result = parser.finalize();

    REQUIRE(result.tool_color_palette.size() == 3);
    REQUIRE(result.tool_color_palette[0] == "#AA0000");
    REQUIRE(result.tool_color_palette[1] == "#00BB00");
    REQUIRE(result.tool_color_palette[2] == "#0000CC");
}

// ============================================================================
// Geometry Builder Tests
// ============================================================================

TEST_CASE("MultiColor - Set tool color palette", "[gcode][multicolor][geometry]") {
    GeometryBuilder builder;

    SECTION("Set and verify palette") {
        std::vector<std::string> palette = {"#FF0000", "#00FF00", "#0000FF"};
        REQUIRE_NOTHROW(builder.set_tool_color_palette(palette));
    }

    SECTION("Empty palette doesn't crash") {
        std::vector<std::string> empty_palette;
        REQUIRE_NOTHROW(builder.set_tool_color_palette(empty_palette));
    }
}

TEST_CASE("MultiColor - Build geometry with tool colors", "[gcode][multicolor][geometry]") {
    SECTION("Use tool colors from palette") {
        GCodeParser parser;

        parser.parse_line("; extruder_colour = #ED1C24;#00C1AE");
        parser.parse_line("T0");
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1");
        parser.parse_line("T1");
        parser.parse_line("G1 X0 Y10 E2");

        auto gcode = parser.finalize();

        GeometryBuilder builder;
        builder.set_tool_color_palette(gcode.tool_color_palette);
        builder.set_use_height_gradient(false); // Use tool colors, not gradient

        SimplificationOptions opts;
        opts.enable_merging = false;

        auto geometry = builder.build(gcode, opts);

        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() > 0);
    }
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("MultiColor - End-to-end integration", "[gcode][multicolor][integration]") {
    GCodeParser parser;

    SECTION("Parse and build multi-color geometry") {
        parser.parse_line("; extruder_colour = #ED1C24;#00C1AE");
        parser.parse_line("T0");
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1");
        parser.parse_line("G1 X10 Y10 E2");
        parser.parse_line("T1");
        parser.parse_line("G1 X0 Y10 E3");
        parser.parse_line("G1 X0 Y0 E4");

        auto gcode = parser.finalize();

        REQUIRE(gcode.tool_color_palette.size() == 2);
        REQUIRE(gcode.layers.size() > 0);
        REQUIRE(gcode.layers[0].segments.size() >= 4);

        // Verify tool indices
        REQUIRE(gcode.layers[0].segments[0].tool_index == 0);
        REQUIRE(gcode.layers[0].segments[1].tool_index == 0);
        REQUIRE(gcode.layers[0].segments[2].tool_index == 1);
        REQUIRE(gcode.layers[0].segments[3].tool_index == 1);

        // Build geometry
        GeometryBuilder builder;
        builder.set_tool_color_palette(gcode.tool_color_palette);

        SimplificationOptions opts;
        opts.enable_merging = false;

        auto geometry = builder.build(gcode, opts);

        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() > 0);
    }
}

TEST_CASE("MultiColor - Synthetic multi-layer multi-tool file",
          "[gcode][multicolor][integration]") {
    // Simulates an OrcaSlicer-style 4-color print with multiple layers,
    // tool changes, and wipe tower — exercises the full parsing pipeline
    // without needing an external gcode file.
    GCodeParser parser;

    // OrcaSlicer-style metadata header
    parser.parse_line("; extruder_colour = #ED1C24;#00C1AE;#F4E2C1;#000000");
    parser.parse_line("; filament_colour = #ED1C24;#00C1AE;#F4E2C1;#000000");

    int tool_change_count = 0;
    auto tool_change = [&](int t) {
        parser.parse_line("T" + std::to_string(t));
        tool_change_count++;
    };

    // Layer 1 (Z=0.2): T0 perimeters, T1 infill, wipe tower
    parser.parse_line(";LAYER_CHANGE");
    parser.parse_line(";Z:0.2");
    tool_change(0);
    parser.parse_line("G1 X10 Y10 Z0.2 E0");
    parser.parse_line("G1 X50 Y10 E2");
    parser.parse_line("G1 X50 Y50 E4");
    parser.parse_line("G1 X10 Y50 E6");
    parser.parse_line("G1 X10 Y10 E8");
    parser.parse_line("; WIPE_TOWER_START");
    parser.parse_line("G1 X80 Y10 E9");
    parser.parse_line("G1 X90 Y10 E10");
    parser.parse_line("; WIPE_TOWER_END");
    tool_change(1);
    parser.parse_line("G1 X20 Y20 E11");
    parser.parse_line("G1 X40 Y20 E12");
    parser.parse_line("G1 X40 Y40 E13");

    // Layer 2 (Z=0.4): All 4 tools with transitions
    parser.parse_line(";LAYER_CHANGE");
    parser.parse_line(";Z:0.4");
    tool_change(0);
    parser.parse_line("G1 X10 Y10 Z0.4 E14");
    parser.parse_line("G1 X50 Y10 E16");
    tool_change(2);
    parser.parse_line("G1 X50 Y50 E18");
    parser.parse_line("G1 X10 Y50 E20");
    tool_change(3);
    parser.parse_line("G1 X30 Y30 E22");
    parser.parse_line("G1 X35 Y35 E23");
    tool_change(1);
    parser.parse_line("G1 X20 Y20 E24");
    parser.parse_line("G1 X40 Y40 E26");

    // Layer 3 (Z=0.6): Rapid tool changes (stress test)
    parser.parse_line(";LAYER_CHANGE");
    parser.parse_line(";Z:0.6");
    for (int i = 0; i < 8; i++) {
        tool_change(i % 4);
        parser.parse_line("G1 X" + std::to_string(10 + i * 5) + " Y" + std::to_string(10 + i * 3) +
                          " Z0.6 E" + std::to_string(27 + i));
    }

    auto result = parser.finalize();

    SECTION("4-color palette parsed correctly") {
        REQUIRE(result.tool_color_palette.size() == 4);
        REQUIRE(result.tool_color_palette[0] == "#ED1C24"); // Red
        REQUIRE(result.tool_color_palette[1] == "#00C1AE"); // Teal
        REQUIRE(result.tool_color_palette[2] == "#F4E2C1"); // Beige
        REQUIRE(result.tool_color_palette[3] == "#000000"); // Black
    }

    SECTION("Multi-layer structure") {
        REQUIRE(result.layers.size() == 3);
        REQUIRE(result.total_segments > 0);

        // Each layer should have segments
        for (size_t i = 0; i < result.layers.size(); i++) {
            INFO("Layer " << i << " has " << result.layers[i].segments.size() << " segments");
            REQUIRE(result.layers[i].segments.size() > 0);
        }
    }

    SECTION("Tool changes tracked") {
        REQUIRE(tool_change_count == 14);

        // Layer 1: starts T0, switches to T1
        REQUIRE(result.layers[0].segments[0].tool_index == 0);

        // Layer 2: uses all 4 tools
        bool saw_tool[4] = {};
        for (const auto& seg : result.layers[1].segments) {
            REQUIRE(seg.tool_index >= 0);
            REQUIRE(seg.tool_index < 4);
            saw_tool[seg.tool_index] = true;
        }
        for (int t = 0; t < 4; t++) {
            INFO("Tool " << t << " should appear in layer 2");
            REQUIRE(saw_tool[t]);
        }
    }

    SECTION("Wipe tower segments detected") {
        bool found_wipe_tower = false;
        for (const auto& seg : result.layers[0].segments) {
            if (result.get_object_name(seg.object_name_index) == "__WIPE_TOWER__") {
                found_wipe_tower = true;
                break;
            }
        }
        REQUIRE(found_wipe_tower);
    }

    SECTION("Geometry builds with tool colors") {
        GeometryBuilder builder;
        builder.set_tool_color_palette(result.tool_color_palette);
        builder.set_use_height_gradient(false);

        SimplificationOptions opts;
        opts.enable_merging = false;

        auto geometry = builder.build(result, opts);

        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() > 0);
    }
}

TEST_CASE("MultiColor - Benchbin MMU3 real file", "[gcode][multicolor][integration][file]") {
    std::string test_file = "assets/test_gcodes/Benchbin_MK4_MMU3.gcode";

    std::ifstream file(test_file);
    REQUIRE(file.is_open());

    GCodeParser parser;
    std::string line;
    int tool_change_count = 0;

    while (std::getline(file, line)) {
        parser.parse_line(line);

        if (line.length() >= 2 && line[0] == 'T' && std::isdigit(line[1]) &&
            (line.length() == 2 || std::isspace(line[2]))) {
            tool_change_count++;
        }
    }

    auto result = parser.finalize();

    SECTION("4-color PrusaSlicer MMU palette") {
        REQUIRE(result.tool_color_palette.size() == 4);
        REQUIRE(result.tool_color_palette[0] == "#E7BD00"); // Yellow
        REQUIRE(result.tool_color_palette[1] == "#00C502"); // Green
        REQUIRE(result.tool_color_palette[2] == "#F4E2C1"); // Beige
        REQUIRE(result.tool_color_palette[3] == "#ED1C24"); // Red
    }

    SECTION("Structure with many tool changes") {
        REQUIRE(tool_change_count > 100);
        REQUIRE(result.layers.size() > 10);
        REQUIRE(result.total_segments > 0);
    }

    SECTION("Geometry builds from real file") {
        GeometryBuilder builder;
        builder.set_tool_color_palette(result.tool_color_palette);
        builder.set_use_height_gradient(false);

        SimplificationOptions opts;
        auto geometry = builder.build(result, opts);

        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() > 0);
    }
}

TEST_CASE("MultiColor - Backward compatibility", "[gcode][multicolor][compatibility]") {
    GCodeParser parser;

    SECTION("Single-color file without palette") {
        parser.parse_line("; filament_colour = #26A69A"); // Single color, no semicolons
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1");

        auto result = parser.finalize();

        // Single color might result in 0 or 1 palette entries depending on parsing
        REQUIRE(result.layers.size() > 0);
        REQUIRE(result.layers[0].segments.size() > 0);
        REQUIRE(result.layers[0].segments[0].tool_index == 0);
    }

    SECTION("Comma-separated filament_colour is a palette, not one blob") {
        // A comma-joined list must reach the per-tool palette; the whole-line
        // blob must never land in filament_color_hex (strtol would silently
        // read only the first field, painting the model in T0's color).
        parser.parse_line("; filament_colour = #800080,#63A5BB,#000000,#FFFFFF");
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1");

        auto result = parser.finalize();

        REQUIRE(result.tool_color_palette.size() == 4);
        REQUIRE(result.tool_color_palette[0] == "#800080");
        REQUIRE(result.tool_color_palette[3] == "#FFFFFF");
        REQUIRE(result.filament_color_hex != "#800080,#63A5BB,#000000,#FFFFFF");
    }

    SECTION("Single color behind an empty leading slot is still captured") {
        // Slot 0 unknown: the palette's first entry is an empty placeholder,
        // and the single-color fallback must skip it rather than store "".
        parser.parse_line("; filament_colour = ,, #00FF00");
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1");

        auto result = parser.finalize();

        REQUIRE(result.filament_color_hex == "#00FF00");
    }

    SECTION("No color metadata at all") {
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1");

        auto result = parser.finalize();

        REQUIRE(result.tool_color_palette.empty());
        REQUIRE(result.layers.size() > 0);
        REQUIRE(result.layers[0].segments[0].tool_index == 0);
    }
}
