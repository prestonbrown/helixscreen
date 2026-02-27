// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_parser.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using Catch::Approx;

TEST_CASE("GCodeParser - Basic movement parsing", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Parse simple G1 move") {
        parser.parse_line("G1 X10 Y20 Z0.2");
        auto file = parser.finalize();

        // Debug: print all layers
        for (size_t i = 0; i < file.layers.size(); i++) {
            std::cerr << "DEBUG: Layer " << i << " Z=" << file.layers[i].z_height
                      << " segments=" << file.layers[i].segments.size() << std::endl;
        }

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

TEST_CASE("GCodeParser - Move type differentiation", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("G0 commands are travel moves") {
        parser.parse_line("G0 X10 Y10 Z0.2"); // Travel move (no extrusion)
        parser.parse_line("G0 X20 Y20");      // Another travel

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 2);
        REQUIRE(file.layers[0].segment_count_travel == 2);
        REQUIRE(file.layers[0].segment_count_extrusion == 0);
        REQUIRE(file.layers[0].segments[0].is_extrusion == false);
        REQUIRE(file.layers[0].segments[1].is_extrusion == false);
    }

    SECTION("G1 without E parameter is travel move") {
        parser.parse_line("G1 X10 Y10 Z0.2"); // No E = travel
        parser.parse_line("G1 X20 Y20");      // No E = travel

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 2);
        REQUIRE(file.layers[0].segment_count_travel == 2);
        REQUIRE(file.layers[0].segment_count_extrusion == 0);
    }

    SECTION("G1 with E parameter is extrusion move") {
        parser.parse_line("G1 X10 Y10 Z0.2 E0.5"); // Has E = extrusion
        parser.parse_line("G1 X20 Y20 E1.0");      // Has E = extrusion

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 2);
        REQUIRE(file.layers[0].segment_count_extrusion == 2);
        REQUIRE(file.layers[0].segment_count_travel == 0);
        REQUIRE(file.layers[0].segments[0].is_extrusion == true);
        REQUIRE(file.layers[0].segments[1].is_extrusion == true);
    }

    SECTION("G1 with decreasing E during movement is retraction (travel move)") {
        parser.parse_line("M82");                  // Absolute extrusion mode
        parser.parse_line("G1 X10 Y10 Z0.2 E1.0"); // Extrusion
        parser.parse_line("G1 X15 Y15 E0.5");      // Move with retraction (E decreases)
        parser.parse_line("G1 X20 Y20");           // Travel after retraction

        auto file = parser.finalize();

        // First move is extrusion, second has negative E delta (retraction), third is travel
        REQUIRE(file.total_segments == 3);
        REQUIRE(file.layers[0].segments[0].is_extrusion == true);
        REQUIRE(file.layers[0].segments[1].is_extrusion == false); // Negative E = retraction
        REQUIRE(file.layers[0].segments[2].is_extrusion == false); // Travel
    }

    SECTION("Mixed G0 and G1 commands") {
        parser.parse_line("G1 X10 Y10 Z0.2 E1.0"); // G1 extrusion
        parser.parse_line("G0 X20 Y20");           // G0 travel
        parser.parse_line("G1 X30 Y30 E2.0");      // G1 extrusion
        parser.parse_line("G0 X0 Y0");             // G0 travel

        auto file = parser.finalize();

        REQUIRE(file.total_segments == 4);
        REQUIRE(file.layers[0].segment_count_extrusion == 2);
        REQUIRE(file.layers[0].segment_count_travel == 2);

        // Verify specific segment types
        REQUIRE(file.layers[0].segments[0].is_extrusion == true);  // G1 E1.0
        REQUIRE(file.layers[0].segments[1].is_extrusion == false); // G0
        REQUIRE(file.layers[0].segments[2].is_extrusion == true);  // G1 E2.0
        REQUIRE(file.layers[0].segments[3].is_extrusion == false); // G0
    }
}

TEST_CASE("GCodeParser - Extrusion amount tracking", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Track extrusion delta in absolute mode (M82)") {
        parser.parse_line("M82"); // Absolute extrusion
        parser.parse_line("G1 X10 Y10 Z0.2 E1.0");
        parser.parse_line("G1 X20 Y20 E3.0"); // Delta = 3.0 - 1.0 = 2.0

        auto file = parser.finalize();

        REQUIRE(file.layers[0].segments[0].extrusion_amount == Approx(1.0f));
        REQUIRE(file.layers[0].segments[1].extrusion_amount == Approx(2.0f));
    }

    SECTION("Track extrusion delta in relative mode (M83)") {
        parser.parse_line("M83");                  // Relative extrusion
        parser.parse_line("G1 X10 Y10 Z0.2 E1.5"); // Delta = 1.5
        parser.parse_line("G1 X20 Y20 E2.0");      // Delta = 2.0

        auto file = parser.finalize();

        REQUIRE(file.layers[0].segments[0].extrusion_amount == Approx(1.5f));
        REQUIRE(file.layers[0].segments[1].extrusion_amount == Approx(2.0f));
    }

    SECTION("Retraction has negative extrusion amount") {
        parser.parse_line("M82"); // Absolute extrusion
        parser.parse_line("G1 X10 Y10 Z0.2 E5.0");
        parser.parse_line("G1 X15 Y15 E3.0"); // Move with retract: delta = 3.0 - 5.0 = -2.0

        auto file = parser.finalize();

        REQUIRE(file.layers[0].segments[0].extrusion_amount == Approx(5.0f));
        REQUIRE(file.layers[0].segments[1].extrusion_amount == Approx(-2.0f));
    }
}

TEST_CASE("GCodeParser - Travel move characteristics", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Travel moves create segments with start and end positions") {
        parser.parse_line("G0 X10 Y10 Z0.2"); // Move to (10,10,0.2)
        parser.parse_line("G0 X100 Y100");    // Travel to (100,100)

        auto file = parser.finalize();

        // Verify both travel moves created segments
        REQUIRE(file.layers[0].segments.size() == 2);

        // Check first segment: from (0,0,0) to (10,10,0.2)
        auto& seg1 = file.layers[0].segments[0];
        REQUIRE(seg1.end.x == Approx(10.0f));
        REQUIRE(seg1.end.y == Approx(10.0f));
        REQUIRE(seg1.is_extrusion == false);

        // Check second segment: from (10,10,0.2) to (100,100,0.2)
        auto& seg2 = file.layers[0].segments[1];
        REQUIRE(seg2.start.x == Approx(10.0f));
        REQUIRE(seg2.start.y == Approx(10.0f));
        REQUIRE(seg2.end.x == Approx(100.0f));
        REQUIRE(seg2.end.y == Approx(100.0f));
        REQUIRE(seg2.is_extrusion == false);
    }

    SECTION("Z-only travel moves (layer changes)") {
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        parser.parse_line("G0 Z0.4"); // Z-hop / layer change
        parser.parse_line("G1 X10 Y10 E2");

        auto file = parser.finalize();

        REQUIRE(file.layers.size() == 2);
        REQUIRE(file.layers[0].z_height == Approx(0.2f));
        REQUIRE(file.layers[1].z_height == Approx(0.4f));
    }
}

TEST_CASE("GCodeParser - Extrusion move characteristics", "[gcode][parser]") {
    GCodeParser parser;

    SECTION("Extrusion moves have non-zero E delta") {
        parser.parse_line("G1 X10 Y10 Z0.2 E1.5");
        parser.parse_line("G1 X20 Y20 E3.0");

        auto file = parser.finalize();

        REQUIRE(file.layers[0].segments[0].extrusion_amount > 0);
        REQUIRE(file.layers[0].segments[1].extrusion_amount > 0);
    }

    SECTION("Extrusion width calculated from E delta and distance") {
        parser.parse_line("; layer_height = 0.2"); // Set layer height metadata
        parser.parse_line("G1 X0 Y0 Z0.2 E0");
        parser.parse_line("G1 X10 Y0 E1.5"); // 10mm move with 1.5mm³ extrusion

        auto file = parser.finalize();

        // Width should be calculated (implementation-specific formula)
        // Just verify it's set to a reasonable value if calculated
        auto& seg = file.layers[0].segments[1];
        if (seg.width > 0) {
            REQUIRE(seg.width > 0.1f); // Minimum reasonable width
            REQUIRE(seg.width < 2.0f); // Maximum reasonable width
        }
    }
}

// ============================================================================
// Metadata Extraction Tests
// ============================================================================

TEST_CASE("extract_header_metadata - OrcaSlicer file footer parsing", "[gcode][metadata]") {
    // Note: OrcaSlicer places print time and filament usage at the END of the file,
    // not in the header. The extract_header_metadata function must scan both
    // header and footer to get complete metadata.

    SECTION("Parse estimated print time from footer (format: NNm NNs)") {
        // Create a temp file with footer-style metadata
        std::string temp_path = "/tmp/test_metadata_time.gcode";
        std::ofstream out(temp_path);
        out << "; generated by OrcaSlicer 2.3.1\n";
        out << "; total layer number: 100\n";
        out << "G1 X10 Y10 Z0.2\n"; // Some G-code to simulate content
        out << "G1 X20 Y20\n";
        // Footer metadata (like OrcaSlicer produces)
        out << "; estimated printing time (normal mode) = 36m 25s\n";
        out << "; total filament used [g] = 10.98\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // 36m 25s = 36*60 + 25 = 2185 seconds
        REQUIRE(metadata.estimated_time_seconds == Approx(2185.0));
        REQUIRE(metadata.filament_used_g == Approx(10.98));

        std::remove(temp_path.c_str());
    }

    SECTION("Parse estimated print time with hours (format: Nh NNm NNs)") {
        std::string temp_path = "/tmp/test_metadata_time_hours.gcode";
        std::ofstream out(temp_path);
        out << "; generated by PrusaSlicer\n";
        out << "G1 X10 Y10 Z0.2\n";
        out << "; estimated printing time (normal mode) = 2h 30m 15s\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // 2h 30m 15s = 2*3600 + 30*60 + 15 = 9015 seconds
        REQUIRE(metadata.estimated_time_seconds == Approx(9015.0));

        std::remove(temp_path.c_str());
    }

    SECTION("Parse slicer info from header") {
        std::string temp_path = "/tmp/test_metadata_slicer.gcode";
        std::ofstream out(temp_path);
        out << "; generated by: OrcaSlicer 2.3.1\n";
        out << "; slicer_version = 2.3.1\n";
        out << "; total layer number: 240\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.slicer == "OrcaSlicer 2.3.1");
        REQUIRE(metadata.layer_count == 240);

        std::remove(temp_path.c_str());
    }

    SECTION("Parse filament weight with decimal") {
        std::string temp_path = "/tmp/test_metadata_filament.gcode";
        std::ofstream out(temp_path);
        out << "G1 X10 Y10 Z0.2\n";
        out << "; total filament used [g] = 25.73\n";
        out << "; filament used [mm] = 8532.5\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_used_g == Approx(25.73));
        REQUIRE(metadata.filament_used_mm == Approx(8532.5));

        std::remove(temp_path.c_str());
    }

    SECTION("Parse height fields from OrcaSlicer footer") {
        std::string temp_path = "/tmp/test_metadata_height_fields.gcode";
        std::ofstream out(temp_path);
        out << "; generated by OrcaSlicer 2.3.1\n";
        out << "G1 X10 Y10 Z0.2\n";
        // Footer metadata (OrcaSlicer places these at end of file)
        out << "; layer_height = 0.24\n";
        out << "; first_layer_height = 0.300\n";
        out << "; max_z_height: 52.62\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.layer_height == Approx(0.24));
        REQUIRE(metadata.first_layer_height == Approx(0.300));
        REQUIRE(metadata.object_height == Approx(52.62));

        std::remove(temp_path.c_str());
    }

    SECTION("Handle file with metadata only in footer (large file simulation)") {
        std::string temp_path = "/tmp/test_metadata_footer_only.gcode";
        std::ofstream out(temp_path);

        // Header with basic info
        out << "; generated by: TestSlicer 1.0\n";
        out << "; total layer number: 50\n";

        // Simulate a lot of G-code (header scanner will stop at G-code)
        for (int i = 0; i < 1000; i++) {
            out << "G1 X" << (i % 100) << " Y" << (i % 100) << " E" << (i * 0.1) << "\n";
        }

        // Footer metadata - should be picked up by footer scanner
        out << "; estimated printing time (normal mode) = 1h 5m 30s\n";
        out << "; total filament used [g] = 15.5\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // Verify footer metadata was found
        // 1h 5m 30s = 3600 + 300 + 30 = 3930 seconds
        REQUIRE(metadata.estimated_time_seconds == Approx(3930.0));
        REQUIRE(metadata.filament_used_g == Approx(15.5));

        // Verify header metadata was also found
        REQUIRE(metadata.slicer == "TestSlicer 1.0");
        REQUIRE(metadata.layer_count == 50);

        std::remove(temp_path.c_str());
    }
}

TEST_CASE("extract_header_metadata - Cura format parsing", "[gcode][metadata]") {
    // Cura places metadata at the BEGINNING of the file with different syntax

    SECTION("Parse Cura slicer info") {
        std::string temp_path = "/tmp/test_metadata_cura_slicer.gcode";
        std::ofstream out(temp_path);
        out << ";Generated with Cura_SteamEngine 5.6.0\n";
        out << ";TIME:7036\n";
        out << ";Filament used: 1.20047m\n";
        out << ";Layer height: 0.12\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.slicer == "Cura_SteamEngine 5.6.0");
        REQUIRE(metadata.layer_height == Approx(0.12));
        std::remove(temp_path.c_str());
    }

    SECTION("Parse Cura time in seconds") {
        std::string temp_path = "/tmp/test_metadata_cura_time.gcode";
        std::ofstream out(temp_path);
        out << ";TIME:7036\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // 7036 seconds = ~117 minutes = ~1h 57m
        REQUIRE(metadata.estimated_time_seconds == Approx(7036.0));
        std::remove(temp_path.c_str());
    }

    SECTION("Parse Cura filament in meters") {
        std::string temp_path = "/tmp/test_metadata_cura_filament.gcode";
        std::ofstream out(temp_path);
        out << ";Filament used: 1.20047m\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // 1.20047m = 1200.47mm
        REQUIRE(metadata.filament_used_mm == Approx(1200.47));
        // Should also estimate grams based on PLA density
        REQUIRE(metadata.filament_used_g > 0);
        std::remove(temp_path.c_str());
    }
}

TEST_CASE("extract_header_metadata - Real OrcaSlicer file", "[gcode][metadata][integration]") {
    // Test with actual test gcode file if it exists
    std::string test_file = "assets/test_gcodes/3DBenchy.gcode";

    std::ifstream check(test_file);
    if (!check.good()) {
        SKIP("Test G-code file not found: " << test_file);
    }
    check.close();

    auto metadata = extract_header_metadata(test_file);

    SECTION("Parses slicer information") {
        // 3DBenchy.gcode header: "; generated by OrcaSlicer 2.3.1"
        REQUIRE_FALSE(metadata.slicer.empty());
        REQUIRE(metadata.slicer.find("OrcaSlicer") != std::string::npos);
    }

    SECTION("Parses layer count from header") {
        // "; total layer number: 240"
        REQUIRE(metadata.layer_count == 240);
    }

    SECTION("Parses estimated print time from footer") {
        // "; estimated printing time (normal mode) = 36m 25s" at line ~126292
        // 36m 25s = 2185 seconds
        REQUIRE(metadata.estimated_time_seconds > 0);
        REQUIRE(metadata.estimated_time_seconds == Approx(2185.0));
    }

    SECTION("Parses filament weight from footer") {
        // "; total filament used [g] = 10.98" at line ~126289
        REQUIRE(metadata.filament_used_g > 0);
        REQUIRE(metadata.filament_used_g == Approx(10.98));
    }

    SECTION("Parses filament type from header") {
        // "; filament_type = PLA" in header
        REQUIRE(metadata.filament_type == "PLA");
    }

    SECTION("Parses height fields from footer") {
        // "; layer_height = 0.2" and "; first_layer_height = 0.200" in footer
        // "; max_z_height: 48.00" in header
        REQUIRE(metadata.layer_height == Approx(0.2));
        REQUIRE(metadata.first_layer_height == Approx(0.200));
        REQUIRE(metadata.object_height == Approx(48.00));
    }
}

TEST_CASE("extract_header_metadata - Real exclude_object_test file",
          "[gcode][metadata][gcode_parser][integration]") {
    std::string test_file = "assets/test_gcodes/exclude_object_test.gcode";

    std::ifstream check(test_file);
    if (!check.good()) {
        SKIP("Test G-code file not found: " << test_file);
    }
    check.close();

    auto metadata = extract_header_metadata(test_file);

    SECTION("Parses slicer information") {
        REQUIRE_FALSE(metadata.slicer.empty());
        REQUIRE(metadata.slicer.find("OrcaSlicer") != std::string::npos);
        // Version 2.3 embedded in slicer string
        REQUIRE(metadata.slicer.find("2.3") != std::string::npos);
    }

    SECTION("Parses layer count") {
        REQUIRE(metadata.layer_count == 218);
    }

    SECTION("Parses layer height fields") {
        REQUIRE(metadata.layer_height == Approx(0.24));
        REQUIRE(metadata.first_layer_height == Approx(0.300));
    }

    SECTION("Parses object height from max_z_height") {
        REQUIRE(metadata.object_height == Approx(52.62));
    }

    SECTION("Parses estimated print time") {
        REQUIRE(metadata.estimated_time_seconds > 0);
    }

    SECTION("Parses filament usage") {
        REQUIRE(metadata.filament_used_mm > 0);
    }
}

// ============================================================================
// Filament Type Detection Tests
// ============================================================================

TEST_CASE("extract_header_metadata - Filament type parsing", "[gcode][metadata][filament_type]") {
    SECTION("Parse simple filament_type (PLA)") {
        std::string temp_path = "/tmp/test_filament_type_pla.gcode";
        std::ofstream out(temp_path);
        out << "; generated by TestSlicer 1.0\n";
        out << "; filament_type = PLA\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "PLA");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse semicolon-separated multi-extruder filament_type (extracts first type)") {
        // OrcaSlicer/PrusaSlicer format for multi-extruder: "PLA;PLA;PLA;PLA"
        std::string temp_path = "/tmp/test_filament_type_multi.gcode";
        std::ofstream out(temp_path);
        out << "; generated by PrusaSlicer 2.6\n";
        out << "; filament_type = PLA;PLA;PLA;PLA\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // Should extract just the first type "PLA", not the full string
        REQUIRE(metadata.filament_type == "PLA");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse PETG filament type") {
        std::string temp_path = "/tmp/test_filament_type_petg.gcode";
        std::ofstream out(temp_path);
        out << "; filament_type = PETG\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "PETG");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse ABS filament type") {
        std::string temp_path = "/tmp/test_filament_type_abs.gcode";
        std::ofstream out(temp_path);
        out << "; filament_type = ABS\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "ABS");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse TPU filament type") {
        std::string temp_path = "/tmp/test_filament_type_tpu.gcode";
        std::ofstream out(temp_path);
        out << "; filament_type = TPU\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "TPU");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse ASA filament type") {
        std::string temp_path = "/tmp/test_filament_type_asa.gcode";
        std::ofstream out(temp_path);
        out << "; filament_type = ASA\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "ASA");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse Nylon filament type") {
        std::string temp_path = "/tmp/test_filament_type_nylon.gcode";
        std::ofstream out(temp_path);
        out << "; filament_type = Nylon\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "Nylon");

        std::remove(temp_path.c_str());
    }

    SECTION("Parse PC (Polycarbonate) filament type") {
        std::string temp_path = "/tmp/test_filament_type_pc.gcode";
        std::ofstream out(temp_path);
        out << "; filament_type = PC\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        REQUIRE(metadata.filament_type == "PC");

        std::remove(temp_path.c_str());
    }

    SECTION("Handle missing filament_type (remains empty)") {
        std::string temp_path = "/tmp/test_filament_type_missing.gcode";
        std::ofstream out(temp_path);
        out << "; generated by TestSlicer 1.0\n";
        out << "; total layer number: 100\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // filament_type should be empty when not present in file
        REQUIRE(metadata.filament_type.empty());

        std::remove(temp_path.c_str());
    }

    SECTION("Parse mixed multi-extruder filament types (extracts first type)") {
        // Different materials for different extruders
        std::string temp_path = "/tmp/test_filament_type_mixed.gcode";
        std::ofstream out(temp_path);
        out << "; generated by OrcaSlicer 2.3.1\n";
        out << "; filament_type = PETG;PLA;ABS\n";
        out << "G1 X10 Y10 Z0.2\n";
        out.close();

        auto metadata = extract_header_metadata(temp_path);

        // Should extract just the first type "PETG"
        REQUIRE(metadata.filament_type == "PETG");

        std::remove(temp_path.c_str());
    }
}

TEST_CASE("extract_header_metadata - Real multi-extruder file", "[gcode][metadata][integration]") {
    // Test with actual multi-extruder G-code file if it exists
    std::string test_file = "assets/test_gcodes/Benchbin_MK4_MMU3.gcode";

    std::ifstream check(test_file);
    if (!check.good()) {
        SKIP("Test G-code file not found: " << test_file);
    }
    check.close();

    auto metadata = extract_header_metadata(test_file);

    SECTION("Parses filament type from multi-extruder file") {
        // File has "; filament_type = PLA;PLA;PLA;PLA"
        // Should extract just "PLA"
        REQUIRE_FALSE(metadata.filament_type.empty());
        REQUIRE(metadata.filament_type == "PLA");
    }
}

// ============================================================================
// Layer Counting Tests
// ============================================================================

TEST_CASE("GCodeParser - Layer counting with LAYER_CHANGE markers", "[gcode][parser][layers]") {
    GCodeParser parser;

    SECTION("Use LAYER_CHANGE markers when present") {
        // Simulate G-code with slicer layer markers - should count 3 layers
        parser.parse_line(";LAYER_CHANGE");
        parser.parse_line(";Z:0.2");
        parser.parse_line("G1 Z0.2 F3000");
        parser.parse_line("G1 X10 Y10 E1");
        parser.parse_line("G1 X20 Y10 E2");
        // Z-hop (should NOT create new layer)
        parser.parse_line("G1 Z0.5 F3000"); // z-hop up
        parser.parse_line("G0 X30 Y30");    // travel
        parser.parse_line("G1 Z0.2 F3000"); // z-hop down
        parser.parse_line("G1 X40 Y40 E3"); // continue extrusion
        parser.parse_line(";LAYER_CHANGE"); // Second layer marker
        parser.parse_line(";Z:0.4");
        parser.parse_line("G1 Z0.4 F3000");
        parser.parse_line("G1 X10 Y10 E4");
        parser.parse_line(";LAYER_CHANGE"); // Third layer marker
        parser.parse_line(";Z:0.6");
        parser.parse_line("G1 Z0.6 F3000");
        parser.parse_line("G1 X10 Y10 E5");

        auto file = parser.finalize();

        // Should have exactly 3 layers (from markers), not more from z-hops
        REQUIRE(file.layers.size() == 3);
        REQUIRE(file.layers[0].z_height == Approx(0.2f));
        REQUIRE(file.layers[1].z_height == Approx(0.4f));
        REQUIRE(file.layers[2].z_height == Approx(0.6f));
    }

    SECTION("Fall back to Z-based detection when no markers") {
        // G-code without slicer markers - must fall back to Z changes
        parser.parse_line("G1 Z0.2 F3000");
        parser.parse_line("G1 X10 Y10 E1");
        parser.parse_line("G1 Z0.4 F3000");
        parser.parse_line("G1 X20 Y20 E2");
        parser.parse_line("G1 Z0.6 F3000");
        parser.parse_line("G1 X30 Y30 E3");

        auto file = parser.finalize();

        // Without markers, falls back to Z-based counting
        REQUIRE(file.layers.size() == 3);
    }

    SECTION("LAYER:N format (alternative slicer syntax)") {
        parser.parse_line(";LAYER:0");
        parser.parse_line("G1 Z0.2 F3000");
        parser.parse_line("G1 X10 Y10 E1");
        parser.parse_line(";LAYER:1");
        parser.parse_line("G1 Z0.4 F3000");
        parser.parse_line("G1 X20 Y20 E2");

        auto file = parser.finalize();

        // Should recognize LAYER:N format
        REQUIRE(file.layers.size() == 2);
    }

    SECTION("Ignore LAYER_COUNT metadata (not a layer change)") {
        parser.parse_line("; total layer number = 100"); // Metadata, not layer change
        parser.parse_line(";LAYER_CHANGE");
        parser.parse_line("G1 Z0.2 E1");
        parser.parse_line(";LAYER_CHANGE");
        parser.parse_line("G1 Z0.4 E2");

        auto file = parser.finalize();

        // Should have 2 layers from markers, not confused by metadata
        REQUIRE(file.layers.size() == 2);
    }
}

TEST_CASE("GCodeParser - Z-hop handling", "[gcode][parser][layers][zhop]") {
    GCodeParser parser;

    SECTION("Z-hop moves should not create new layers") {
        // This is the bug scenario: z-hop creates phantom layers
        parser.parse_line(";LAYER_CHANGE");
        parser.parse_line("G1 Z0.2 E1"); // Real layer
        parser.parse_line("G1 X10 Y10 E2");
        parser.parse_line("G1 Z0.6");    // Z-hop up (travel, no E)
        parser.parse_line("G0 X50 Y50"); // Travel move
        parser.parse_line("G1 Z0.2");    // Z-hop down
        parser.parse_line("G1 X60 Y60 E3");

        auto file = parser.finalize();

        // Should have only 1 layer - the z-hop should not create layers
        REQUIRE(file.layers.size() == 1);
        REQUIRE(file.layers[0].z_height == Approx(0.2f));
    }
}

// ============================================================================
// Thumbnail Extraction from Content Tests
// ============================================================================

TEST_CASE("extract_thumbnails_from_content - Basic extraction", "[gcode][thumbnail]") {
    SECTION("Extract thumbnail from minimal valid gcode content") {
        // Create minimal gcode with a tiny base64-encoded PNG
        // This is a minimal 1x1 PNG (smallest valid PNG)
        // PNG magic: 89 50 4E 47 0D 0A 1A 0A, followed by minimal chunks
        std::string minimal_png_base64 =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAA"
            "BJRU5ErkJggg==";

        std::stringstream gcode;
        gcode << "; generated by OrcaSlicer 2.3.1\n";
        gcode << "; thumbnail begin 1x1 " << minimal_png_base64.size() << "\n";
        // Split base64 into lines (like real gcode)
        gcode << "; " << minimal_png_base64 << "\n";
        gcode << "; thumbnail end\n";
        gcode << "G28 ; home\n";

        auto thumbnails = extract_thumbnails_from_content(gcode.str());

        REQUIRE(thumbnails.size() == 1);
        REQUIRE(thumbnails[0].width == 1);
        REQUIRE(thumbnails[0].height == 1);
        REQUIRE(!thumbnails[0].png_data.empty());

        // Verify PNG magic bytes
        REQUIRE(thumbnails[0].png_data.size() >= 8);
        REQUIRE(thumbnails[0].png_data[0] == 0x89);
        REQUIRE(thumbnails[0].png_data[1] == 'P');
        REQUIRE(thumbnails[0].png_data[2] == 'N');
        REQUIRE(thumbnails[0].png_data[3] == 'G');
    }

    SECTION("Returns empty for gcode without thumbnails") {
        std::string gcode = "; generated by TestSlicer\n"
                            "G28 ; home\n"
                            "G1 X10 Y10 Z0.2\n"
                            "G1 X20 Y20 E1.0\n";

        auto thumbnails = extract_thumbnails_from_content(gcode);

        REQUIRE(thumbnails.empty());
    }

    SECTION("Returns empty for empty content") {
        auto thumbnails = extract_thumbnails_from_content("");
        REQUIRE(thumbnails.empty());
    }

    SECTION("Handles incomplete thumbnail block gracefully") {
        std::string gcode = "; generated by TestSlicer\n"
                            "; thumbnail begin 48x48 100\n"
                            "; iVBORw0KGgoAAAANSUhEU\n"
                            // Missing "thumbnail end" - should not crash
                            "G28 ; home\n";

        auto thumbnails = extract_thumbnails_from_content(gcode);
        // Should handle gracefully - may return partial or empty
        // Just verify no crash
        REQUIRE(true);
    }
}

TEST_CASE("extract_thumbnails_from_content - Multiple thumbnails", "[gcode][thumbnail]") {
    // Real slicers often embed multiple sizes (e.g., 48x48 for LCD, 300x300 for web)
    std::string small_png_base64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAA"
        "BJRU5ErkJggg==";
    std::string large_png_base64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEklEQVR42mNk+M9Qz8DAwMAAAA8ABMoCE/"
        "t5ZwAAAABJRU5ErkJggg==";

    std::stringstream gcode;
    gcode << "; generated by OrcaSlicer 2.3.1\n";
    // First (smaller) thumbnail
    gcode << "; thumbnail begin 1x1 " << small_png_base64.size() << "\n";
    gcode << "; " << small_png_base64 << "\n";
    gcode << "; thumbnail end\n";
    // Second (larger) thumbnail
    gcode << "; thumbnail begin 2x2 " << large_png_base64.size() << "\n";
    gcode << "; " << large_png_base64 << "\n";
    gcode << "; thumbnail end\n";
    gcode << "G28 ; home\n";

    auto thumbnails = extract_thumbnails_from_content(gcode.str());

    REQUIRE(thumbnails.size() == 2);

    // Should be sorted largest-first
    REQUIRE(thumbnails[0].width >= thumbnails[1].width);

    // Verify both have valid PNG data
    for (const auto& thumb : thumbnails) {
        REQUIRE(thumb.png_data.size() >= 8);
        REQUIRE(thumb.png_data[0] == 0x89);
        REQUIRE(thumb.png_data[1] == 'P');
    }
}

TEST_CASE("extract_thumbnails_from_content - Real-world format variations", "[gcode][thumbnail]") {
    SECTION("Handles multi-line base64 (split across lines)") {
        // Real slicers split base64 into ~78 char lines
        std::string line1 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk";
        std::string line2 = "+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";

        std::stringstream gcode;
        gcode << "; thumbnail begin 1x1 100\n";
        gcode << "; " << line1 << "\n";
        gcode << "; " << line2 << "\n";
        gcode << "; thumbnail end\n";

        auto thumbnails = extract_thumbnails_from_content(gcode.str());

        REQUIRE(thumbnails.size() == 1);
        // Should have decoded the concatenated base64
        REQUIRE(thumbnails[0].png_data.size() >= 8);
        REQUIRE(thumbnails[0].png_data[0] == 0x89);
    }

    SECTION("Ignores non-thumbnail comments") {
        std::string png_base64 =
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAA"
            "BJRU5ErkJggg==";

        std::stringstream gcode;
        gcode << "; generated by OrcaSlicer\n";
        gcode << "; filament_type = PLA\n";
        gcode << "; estimated printing time (normal mode) = 36m 25s\n";
        gcode << "; thumbnail begin 1x1 100\n";
        gcode << "; " << png_base64 << "\n";
        gcode << "; thumbnail end\n";
        gcode << "; total layer number: 240\n";

        auto thumbnails = extract_thumbnails_from_content(gcode.str());

        // Should find exactly one thumbnail, not confuse other comments
        REQUIRE(thumbnails.size() == 1);
    }
}

TEST_CASE("GCodeParser - Real 3DBenchy layer count", "[gcode][parser][layers][integration]") {
    // Integration test with real test file
    std::string test_file = "assets/test_gcodes/3DBenchy.gcode";

    std::ifstream check(test_file);
    if (!check.good()) {
        SKIP("Test G-code file not found: " << test_file);
    }
    check.close();

    // Parse the entire file line by line
    GCodeParser parser;
    std::ifstream file_stream(test_file);
    std::string line;
    while (std::getline(file_stream, line)) {
        parser.parse_line(line);
    }
    auto file = parser.finalize();

    SECTION("Layer count matches slicer metadata") {
        // 3DBenchy has 240 LAYER_CHANGE markers (confirmed via grep)
        // The parser should count 240 layers, not 2912
        INFO("Actual layer count: " << file.layers.size());
        INFO("Expected: ~240 (from slicer metadata, not 2912 from Z movements)");

        // Allow some tolerance for first layer/intro differences
        REQUIRE(file.layers.size() >= 230);
        REQUIRE(file.layers.size() <= 250);
    }

    SECTION("Layer count stored in metadata matches parsed count") {
        // The metadata layer count should match what we parsed
        INFO("Parsed layers: " << file.layers.size());
        INFO("Metadata layer count: " << file.total_layer_count);

        // If metadata has layer count, it should roughly match parsed
        if (file.total_layer_count > 0) {
            REQUIRE(file.layers.size() >= static_cast<size_t>(file.total_layer_count * 0.9));
            REQUIRE(file.layers.size() <= static_cast<size_t>(file.total_layer_count * 1.1));
        }
    }
}

// ============================================================================
// Extrusion width metadata parsing
// ============================================================================

TEST_CASE("GCodeParser - Extrusion width metadata parsing", "[gcode][parser][metadata][width]") {
    SECTION("Parse OrcaSlicer header width comments") {
        GCodeParser parser;
        parser.parse_line("; perimeters extrusion width = 0.45mm");
        parser.parse_line("; infill extrusion width = 0.50mm");
        parser.parse_line("; first layer extrusion width = 0.42mm");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        auto file = parser.finalize();

        REQUIRE(file.perimeter_extrusion_width_mm == Approx(0.45f));
        REQUIRE(file.infill_extrusion_width_mm == Approx(0.50f));
        REQUIRE(file.first_layer_extrusion_width_mm == Approx(0.42f));
    }

    SECTION("Parse width without mm suffix") {
        GCodeParser parser;
        parser.parse_line("; perimeters extrusion width = 0.40");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        auto file = parser.finalize();

        REQUIRE(file.perimeter_extrusion_width_mm == Approx(0.40f));
    }

    SECTION("Parse generic line_width as default extrusion width") {
        GCodeParser parser;
        parser.parse_line("; line_width = 0.4");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        auto file = parser.finalize();

        REQUIRE(file.extrusion_width_mm == Approx(0.4f));
    }

    SECTION("Percentage values are ignored — not parsed as mm") {
        GCodeParser parser;
        // Header with correct mm values (parsed first)
        parser.parse_line("; perimeters extrusion width = 0.40mm");
        parser.parse_line("; infill extrusion width = 0.45mm");
        parser.parse_line("; first layer extrusion width = 0.40mm");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        // OrcaSlicer settings dump at end with percentage values
        parser.parse_line("; line_width = 100%");
        parser.parse_line("; inner_wall_line_width = 100%");
        parser.parse_line("; sparse_infill_line_width = 112.5%");
        parser.parse_line("; initial_layer_line_width = 100%");
        parser.parse_line("; outer_wall_line_width = 100%");
        auto file = parser.finalize();

        // Percentage values must NOT overwrite the correct mm values
        INFO("extrusion_width_mm: " << file.extrusion_width_mm);
        INFO("perimeter_extrusion_width_mm: " << file.perimeter_extrusion_width_mm);
        INFO("infill_extrusion_width_mm: " << file.infill_extrusion_width_mm);
        INFO("first_layer_extrusion_width_mm: " << file.first_layer_extrusion_width_mm);
        REQUIRE(file.extrusion_width_mm < 1.0f);
        REQUIRE(file.perimeter_extrusion_width_mm == Approx(0.40f));
        REQUIRE(file.infill_extrusion_width_mm == Approx(0.45f));
        REQUIRE(file.first_layer_extrusion_width_mm == Approx(0.40f));
    }

    SECTION("Percentage-only file gets zero width (no valid mm values)") {
        GCodeParser parser;
        parser.parse_line("; line_width = 100%");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        auto file = parser.finalize();

        // No valid mm-based width was parsed, should remain 0
        REQUIRE(file.extrusion_width_mm == Approx(0.0f));
    }

    SECTION("Out-of-range values are rejected") {
        GCodeParser parser;
        // Value too large (>3.0mm)
        parser.parse_line("; line_width = 5.0");
        parser.parse_line("G1 X10 Y10 Z0.2 E1");
        auto file = parser.finalize();

        REQUIRE(file.extrusion_width_mm == Approx(0.0f));
    }
}

TEST_CASE("GCodeParser - Real OrcaSlicer SSR file metadata",
          "[gcode][parser][metadata][integration]") {
    // Integration test with the real file that triggered the 100mm width bug
    std::string test_file = "assets/test_gcodes/ssr_heat_sink_orca.gcode";

    std::ifstream check(test_file);
    if (!check.good()) {
        SKIP("Test G-code file not found: " << test_file);
    }
    check.close();

    GCodeParser parser;
    std::ifstream file_stream(test_file);
    std::string line;
    while (std::getline(file_stream, line)) {
        parser.parse_line(line);
    }
    auto file = parser.finalize();

    SECTION("Extrusion width is sane despite OrcaSlicer settings dump") {
        INFO("extrusion_width_mm: " << file.extrusion_width_mm);
        INFO("perimeter_extrusion_width_mm: " << file.perimeter_extrusion_width_mm);
        INFO("infill_extrusion_width_mm: " << file.infill_extrusion_width_mm);
        INFO("first_layer_extrusion_width_mm: " << file.first_layer_extrusion_width_mm);

        // All widths must be in sane range (not 100mm from percentage parsing)
        REQUIRE(file.extrusion_width_mm < 1.0f);
        REQUIRE(file.perimeter_extrusion_width_mm < 1.0f);
        REQUIRE(file.infill_extrusion_width_mm < 1.0f);
        REQUIRE(file.first_layer_extrusion_width_mm < 1.0f);

        // Should have parsed the correct header values
        // Note: "solid infill extrusion width = 0.40mm" overwrites "infill extrusion width =
        // 0.45mm" because both match the "infill" category (last-writer-wins)
        REQUIRE(file.perimeter_extrusion_width_mm == Approx(0.40f));
        REQUIRE(file.infill_extrusion_width_mm == Approx(0.40f));
        REQUIRE(file.first_layer_extrusion_width_mm == Approx(0.40f));
    }

    SECTION("Layer count is correct") {
        // File header says: ; total layer number: 153
        INFO("Parsed layers: " << file.layers.size());
        REQUIRE(file.layers.size() >= 150);
        REQUIRE(file.layers.size() <= 156);
    }
}
