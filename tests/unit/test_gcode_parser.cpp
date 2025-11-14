// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 HelixScreen Contributors
 */

#include "gcode_parser.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <sstream>

using namespace gcode;
using Catch::Matchers::WithinAbs;

TEST_CASE("GCodeParser - Basic movement parsing", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Parse simple G1 move") {
        parser.parse_line("G1 X10 Y20 Z0.2");
        auto file = parser.finalize();

        REQUIRE(file.layers.size() == 1);
        REQUIRE(file.layers[0].z_height == Approx(0.2f));
    }

    SECTION("Parse movement with extrusion") {
        parser.parse_line("G1 X10 Y20 Z0.2 E1.5");
        auto file = parser.finalize();

        REQUIRE(file.layers.size() == 1);
        REQUIRE(file.total_segments == 1);
        REQUIRE(file.layers[0].segments[0].is_extrusion == true);
    }

    SECTION("Parse travel move (no extrusion)") {
        parser.parse_line("G0 X10 Y20 Z0.2");
        auto file = parser.finalize();

        REQUIRE(file.total_segments == 1);
        REQUIRE(file.layers[0].segments[0].is_extrusion == false);
    }
}

TEST_CASE("GCodeParser - Layer detection", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Detect Z-axis layer changes") {
        parser.parse_line("G1 X0 Y0 Z0.2 E1");
        parser.parse_line("G1 X10 Y10 E2");
        parser.parse_line("G1 X0 Y0 Z0.4 E3"); // New layer
        parser.parse_line("G1 X20 Y20 E4");

        auto file = parser.finalize();

        REQUIRE(file.layers.size() == 2);
        REQUIRE(file.layers[0].z_height == Approx(0.2f));
        REQUIRE(file.layers[1].z_height == Approx(0.4f));
    }

    SECTION("Find layer by Z height") {
        parser.parse_line("G1 X0 Y0 Z0.2");
        parser.parse_line("G1 X0 Y0 Z0.4");
        parser.parse_line("G1 X0 Y0 Z0.6");

        auto file = parser.finalize();

        REQUIRE(file.find_layer_at_z(0.2f) == 0);
        REQUIRE(file.find_layer_at_z(0.4f) == 1);
        REQUIRE(file.find_layer_at_z(0.6f) == 2);
        REQUIRE(file.find_layer_at_z(0.3f) == 0); // Closest to 0.2
    }
}

TEST_CASE("GCodeParser - Coordinate extraction", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Extract X, Y, Z coordinates") {
        parser.parse_line("G1 X10.5 Y-20.3 Z0.2");
        parser.parse_line("G1 X15.5 Y-15.3"); // Move from previous position

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 2);
        auto& seg1 = file.layers[0].segments[0];
        REQUIRE(seg1.start.x == Approx(0.0f));
        REQUIRE(seg1.start.y == Approx(0.0f));
        REQUIRE(seg1.end.x == Approx(10.5f));
        REQUIRE(seg1.end.y == Approx(-20.3f));

        auto& seg2 = file.layers[0].segments[1];
        REQUIRE(seg2.start.x == Approx(10.5f));
        REQUIRE(seg2.start.y == Approx(-20.3f));
        REQUIRE(seg2.end.x == Approx(15.5f));
        REQUIRE(seg2.end.y == Approx(-15.3f));
    }
}

TEST_CASE("GCodeParser - Comments and whitespace", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Ignore comments") {
        parser.parse_line("G1 X10 Y20 ; This is a comment");
        auto file = parser.finalize();

        REQUIRE(file.total_segments == 1);
    }

    SECTION("Handle blank lines") {
        parser.parse_line("");
        parser.parse_line("   ");
        parser.parse_line("\t");
        auto file = parser.finalize();

        REQUIRE(file.total_segments == 0);
    }

    SECTION("Trim leading/trailing whitespace") {
        parser.parse_line("  G1 X10 Y20  ");
        auto file = parser.finalize();

        REQUIRE(file.total_segments == 1);
    }
}

TEST_CASE("GCodeParser - EXCLUDE_OBJECT commands", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Parse EXCLUDE_OBJECT_DEFINE") {
        parser.parse_line("EXCLUDE_OBJECT_DEFINE NAME=cube_1 CENTER=50,75 "
                          "POLYGON=[[45,70],[55,70],[55,80],[45,80]]");
        auto file = parser.finalize();

        REQUIRE(file.objects.size() == 1);
        REQUIRE(file.objects.count("cube_1") == 1);

        auto& obj = file.objects["cube_1"];
        REQUIRE(obj.name == "cube_1");
        REQUIRE(obj.center.x == Approx(50.0f));
        REQUIRE(obj.center.y == Approx(75.0f));
        REQUIRE(obj.polygon.size() == 4);
    }

    SECTION("Track segments by object") {
        parser.parse_line("EXCLUDE_OBJECT_DEFINE NAME=part1 CENTER=10,10");
        parser.parse_line("EXCLUDE_OBJECT_START NAME=part1");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        parser.parse_line("G1 X20 Y10 E2");
        parser.parse_line("EXCLUDE_OBJECT_END NAME=part1");
        parser.parse_line("G1 X30 Y30 E3"); // Not in object

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 3);
        REQUIRE(file.layers[0].segments[0].object_name == "part1");
        REQUIRE(file.layers[0].segments[1].object_name == "part1");
        REQUIRE(file.layers[0].segments[2].object_name == "");
    }
}

TEST_CASE("GCodeParser - Bounding box calculation", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Calculate global bounding box") {
        parser.parse_line("G1 X-10 Y-10 Z0.2");
        parser.parse_line("G1 X100 Y50 Z10.5");

        auto file = parser.finalize();

        REQUIRE(file.global_bounding_box.min.x == Approx(-10.0f));
        REQUIRE(file.global_bounding_box.min.y == Approx(-10.0f));
        REQUIRE(file.global_bounding_box.min.z == Approx(0.2f));
        REQUIRE(file.global_bounding_box.max.x == Approx(100.0f));
        REQUIRE(file.global_bounding_box.max.y == Approx(50.0f));
        REQUIRE(file.global_bounding_box.max.z == Approx(10.5f));

        auto center = file.global_bounding_box.center();
        REQUIRE(center.x == Approx(45.0f));
        REQUIRE(center.y == Approx(20.0f));
    }
}

TEST_CASE("GCodeParser - Positioning modes", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Absolute positioning (G90, default)") {
        parser.parse_line("G90"); // Absolute mode
        parser.parse_line("G1 X10 Y10 Z0.2");
        parser.parse_line("G1 X20 Y20"); // Absolute coordinates

        auto file = parser.finalize();

        REQUIRE(file.layers[0].segments[1].end.x == Approx(20.0f));
        REQUIRE(file.layers[0].segments[1].end.y == Approx(20.0f));
    }

    SECTION("Relative positioning (G91)") {
        parser.parse_line("G91"); // Relative mode
        parser.parse_line("G1 X10 Y10 Z0.2");
        parser.parse_line("G1 X5 Y5"); // Relative offset

        auto file = parser.finalize();

        REQUIRE(file.layers[0].segments[1].end.x == Approx(15.0f));
        REQUIRE(file.layers[0].segments[1].end.y == Approx(15.0f));
    }
}

TEST_CASE("GCodeParser - Statistics", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Count segments by type") {
        parser.parse_line("G1 X10 Y10 Z0.2 E1"); // Extrusion
        parser.parse_line("G0 X20 Y20");         // Travel
        parser.parse_line("G1 X30 Y30 E2");      // Extrusion

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 3);
        REQUIRE(file.layers[0].segment_count_extrusion == 2);
        REQUIRE(file.layers[0].segment_count_travel == 1);
    }
}

TEST_CASE("GCodeParser - Real-world G-code snippet", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Parse typical slicer output") {
        std::vector<std::string> gcode = {
            "; Layer 0",
            "G1 Z0.2 F7800",
            "G1 X95.3 Y95.3",
            "G1 X95.3 Y104.7 E0.5",
            "G1 X104.7 Y104.7 E1.0",
            "G1 X104.7 Y95.3 E1.5",
            "G1 X95.3 Y95.3 E2.0",
            "; Layer 1",
            "G1 Z0.4 F7800",
            "G1 X95.3 Y95.3",
            "G1 X95.3 Y104.7 E2.5",
            "G1 X104.7 Y104.7 E3.0",
        };

        for (const auto& line : gcode) {
            parser.parse_line(line);
        }

        auto file = parser.finalize();

        REQUIRE(file.layers.size() == 2);
        REQUIRE(file.layers[0].z_height == Approx(0.2f));
        REQUIRE(file.layers[1].z_height == Approx(0.4f));
        REQUIRE(file.total_segments > 0);
    }
}
