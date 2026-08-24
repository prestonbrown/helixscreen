// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_layer_index.h"

#include <fstream>
#include <sstream>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using Catch::Approx;

// Helper to create a temporary G-code file
class TempGCodeFile {
  public:
    explicit TempGCodeFile(const std::string& content) {
        path_ = "/tmp/test_layer_index_" + std::to_string(rand()) + ".gcode";
        std::ofstream file(path_);
        file << content;
        file.close();
    }

    ~TempGCodeFile() {
        std::remove(path_.c_str());
    }

    const std::string& path() const {
        return path_;
    }

  private:
    std::string path_;
};

TEST_CASE("GCodeLayerIndex - Basic indexing", "[gcode][layer_index]") {
    SECTION("Build index from simple G-code") {
        std::string gcode = R"(
G1 Z0.2 F1000
G1 X10 Y10 E1
G1 X20 Y20 E2
G1 Z0.4 F1000
G1 X30 Y30 E3
G1 X40 Y40 E4
G1 Z0.6 F1000
G1 X50 Y50 E5
)";

        TempGCodeFile file(gcode);
        GCodeLayerIndex index;

        REQUIRE(index.build_from_file(file.path()));
        REQUIRE(index.get_layer_count() == 3);
        REQUIRE(index.is_valid());
    }

    SECTION("Get layer entry") {
        std::string gcode = R"(
G1 Z0.2 E0.1
G1 X10 E0.2
G1 Z0.4 E0.3
G1 X20 E0.4
)";

        TempGCodeFile file(gcode);
        GCodeLayerIndex index;
        index.build_from_file(file.path());

        auto entry0 = index.get_entry(0);
        REQUIRE(entry0.is_valid());
        REQUIRE(entry0.z_height == Approx(0.2f));

        auto entry1 = index.get_entry(1);
        REQUIRE(entry1.is_valid());
        REQUIRE(entry1.z_height == Approx(0.4f));

        // Out of range
        auto entry_invalid = index.get_entry(100);
        REQUIRE(!entry_invalid.is_valid());
    }

    SECTION("Find layer at Z") {
        std::string gcode = R"(
G1 Z0.2 E0.1
G1 Z0.4 E0.2
G1 Z0.6 E0.3
G1 Z0.8 E0.4
)";

        TempGCodeFile file(gcode);
        GCodeLayerIndex index;
        index.build_from_file(file.path());

        REQUIRE(index.find_layer_at_z(0.2f) == 0);
        REQUIRE(index.find_layer_at_z(0.4f) == 1);
        REQUIRE(index.find_layer_at_z(0.6f) == 2);
        REQUIRE(index.find_layer_at_z(0.8f) == 3);

        // Closest layer lookup - 0.3 is equidistant, algorithm picks higher
        REQUIRE(index.find_layer_at_z(0.25f) == 0); // Closer to 0.2
        REQUIRE(index.find_layer_at_z(0.35f) == 1); // Closer to 0.4
        REQUIRE(index.find_layer_at_z(0.5f) == 1);  // Equidistant, picks 0.4
    }
}

TEST_CASE("GCodeLayerIndex - Statistics", "[gcode][layer_index]") {
    std::string gcode = R"(
G1 Z0.2 E0.1
G1 X10 Y10 E0.5
G1 X20 Y20 E1.0
G0 X0 Y0
G1 Z0.4 E1.1
G1 X30 Y30 E1.5
G0 X10 Y10
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    index.build_from_file(file.path());

    const auto& stats = index.get_stats();
    REQUIRE(stats.total_layers == 2);
    REQUIRE(stats.min_z == Approx(0.2f));
    REQUIRE(stats.max_z == Approx(0.4f));
    REQUIRE(stats.extrusion_moves > 0);
    REQUIRE(stats.travel_moves > 0);
    REQUIRE(stats.build_time_ms > 0);
}

TEST_CASE("GCodeLayerIndex - Memory usage", "[gcode][layer_index]") {
    std::string gcode = R"(
G1 Z0.2 E0.1
G1 Z0.4 E0.2
G1 Z0.6 E0.3
G1 Z0.8 E0.4
G1 Z1.0 E0.5
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    index.build_from_file(file.path());

    // Memory should be small (~24 bytes per layer + vector/struct overhead)
    size_t mem = index.memory_usage_bytes();
    size_t per_layer = index.get_layer_count() > 0 ? mem / index.get_layer_count() : 0;
    INFO("Memory usage: " << mem << " bytes for " << index.get_layer_count() << " layers");
    INFO("Per-layer overhead: " << per_layer << " bytes");
    // With vector capacity and struct overhead, expect < 10KB for small indices
    REQUIRE(mem < 10 * 1024);
}

TEST_CASE("GCodeLayerIndex - LAYER_CHANGE markers", "[gcode][layer_index]") {
    std::string gcode = R"(
;LAYER_CHANGE
G1 Z0.2 E0.1
G1 X10 E0.2
;LAYER_CHANGE
G1 Z0.4 E0.3
G1 X20 E0.4
;LAYER_CHANGE
G1 Z0.6 E0.5
G1 X30 E0.6
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    index.build_from_file(file.path());

    REQUIRE(index.get_layer_count() == 3);
    REQUIRE(index.get_layer_z(0) == Approx(0.2f));
    REQUIRE(index.get_layer_z(1) == Approx(0.4f));
    REQUIRE(index.get_layer_z(2) == Approx(0.6f));
}

TEST_CASE("GCodeLayerIndex - OrcaSlicer BEFORE/AFTER_LAYER_CHANGE not counted",
          "[gcode][layer_index]") {
    // OrcaSlicer 2.3 emits three markers per layer transition:
    //   ;LAYER_CHANGE           (real)
    //   ;BEFORE_LAYER_CHANGE    (start of user before-layer macro)
    //   ;AFTER_LAYER_CHANGE     (end of user before-layer macro)
    // The user macro often contains a Z-hop (G1 Zn) which consumes the
    // pending_layer_start flag, then ;AFTER_LAYER_CHANGE re-arms it and the
    // real Z move creates a duplicate entry — doubling the layer count and
    // splitting per-layer byte ranges so the renderer draws only the user-macro
    // moves. Regression: 2D streaming renderer on Snapmaker U1.
    std::string gcode = R"(
;LAYER_CHANGE
;Z:0.2
;BEFORE_LAYER_CHANGE
G1 Z0.4 F600
;AFTER_LAYER_CHANGE
G1 Z0.2 F1000
G1 X10 Y10 E0.1
;LAYER_CHANGE
;Z:0.4
;BEFORE_LAYER_CHANGE
G1 Z0.6 F600
;AFTER_LAYER_CHANGE
G1 Z0.4 F1000
G1 X20 Y20 E0.2
;LAYER_CHANGE
;Z:0.6
;BEFORE_LAYER_CHANGE
G1 Z0.8 F600
;AFTER_LAYER_CHANGE
G1 Z0.6 F1000
G1 X30 Y30 E0.3
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));

    // Primary regression: layer count must equal real ;LAYER_CHANGE count,
    // not 2× (one entry per real marker + one per AFTER_LAYER_CHANGE rearm).
    REQUIRE(index.get_layer_count() == 3);
    // Byte ranges must cover each layer's full content (including the user
    // macro). The first layer must start at offset 0 (or near it) so the
    // ;LAYER_CHANGE marker and following moves are inside layer 0, not skipped.
    REQUIRE(index.get_entry(0).file_offset < index.get_entry(1).file_offset);
    REQUIRE(index.get_entry(1).file_offset < index.get_entry(2).file_offset);
}

TEST_CASE("GCodeLayerIndex - LAYER_CHANGE_HOOK is not a marker", "[gcode][layer_index]") {
    // Locks in the terminator check in is_layer_marker: a comment whose
    // name starts with LAYER_CHANGE but continues into a longer identifier
    // (e.g. a custom ;LAYER_CHANGE_HOOK macro) must not flip the indexer
    // into marker-based mode.
    //
    // Pre-fix behavior (no terminator check): ;LAYER_CHANGE_HOOK matches,
    // use_layer_markers becomes true, and subsequent Z-changes are ignored
    // because no real marker re-arms pending_layer_start. The whole file
    // collapses to a single layer.
    //
    // Post-fix behavior: ;LAYER_CHANGE_HOOK is rejected, the indexer stays
    // in Z-change mode, and we get one entry per Z transition (3 here).
    std::string gcode = R"(
;LAYER_CHANGE_HOOK
G1 Z0.2 F1000
G1 X10 Y10 E0.1
G1 Z0.4 F1000
G1 X20 Y20 E0.2
G1 Z0.6 F1000
G1 X30 Y30 E0.3
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 3);
}

TEST_CASE("GCodeLayerIndex - per-layer start position", "[gcode][layer_index]") {
    // Streaming mode parses each layer with a fresh GCodeParser, so without a
    // recorded starting position the first move of a layer is drawn from
    // (0,0) — producing stray travel/extrusion lines from origin to the real
    // print location on the 2D viewer. The indexer must snapshot the head
    // position at the start of each layer's byte range so load_layer can
    // seed the parser.
    std::string gcode = R"(
G1 X10 Y20 Z0.2 F1000
G1 X30 Y40 E0.1
G1 X50 Y60 Z0.4
G1 X70 Y80 E0.2
G1 X90 Y100 Z0.6
G1 X110 Y120 E0.3
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 3);

    // Layer 0 starts at file position 0 — head still at default origin.
    auto e0 = index.get_entry(0);
    REQUIRE(e0.start_x == Approx(0.0f));
    REQUIRE(e0.start_y == Approx(0.0f));

    // Layer 1 starts after layer 0's print move, so head is at the end of
    // that move: (30, 40), z=0.2.
    auto e1 = index.get_entry(1);
    REQUIRE(e1.start_x == Approx(30.0f));
    REQUIRE(e1.start_y == Approx(40.0f));
    REQUIRE(e1.start_z == Approx(0.2f));

    // Layer 2 similarly inherits the end of layer 1.
    auto e2 = index.get_entry(2);
    REQUIRE(e2.start_x == Approx(70.0f));
    REQUIRE(e2.start_y == Approx(80.0f));
    REQUIRE(e2.start_z == Approx(0.4f));
}

TEST_CASE("GCodeLayerIndex - Z-only move carries X/Y forward", "[gcode][layer_index]") {
    // A Z-only inter-layer move (common with retract+lift slicers) must not
    // reset X/Y on the next layer's snapshot — the head is still at the end
    // of the previous layer's print move.
    std::string gcode = R"(
G1 X10 Y20 Z0.2 F1000
G1 X30 Y40 E0.1
G1 Z0.4
G1 X50 Y60 E0.2
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 2);

    auto e1 = index.get_entry(1);
    // Z-only move triggers the layer-1 entry; head is still at (30,40) from
    // the previous extrusion move, NOT reset to (0,0) just because this line
    // has no X/Y.
    REQUIRE(e1.start_x == Approx(30.0f));
    REQUIRE(e1.start_y == Approx(40.0f));
    REQUIRE(e1.start_z == Approx(0.2f));
}

TEST_CASE("GCodeLayerIndex - axis extraction ignores comments", "[gcode][layer_index]") {
    // Defensive: a trailing ;X100 retract comment must not poison the X
    // tracked for the next layer's snapshot.
    std::string gcode = R"(
G1 X10 Y20 Z0.2 F1000 ; X999 retract
G1 X30 Y40 E0.1 ; comment with Y888
G1 X50 Y60 Z0.4
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 2);

    auto e1 = index.get_entry(1);
    REQUIRE(e1.start_x == Approx(30.0f));
    REQUIRE(e1.start_y == Approx(40.0f));
}

TEST_CASE("GCodeLayerIndex - per-layer start_feature_type", "[gcode][layer_index][feature_type]") {
    // Streaming mode parses each layer with a fresh GCodeParser. Without a
    // recorded starting feature type, the prologue's ;TYPE: comment is
    // invisible to load_layer (it lives before layer 0's byte_offset),
    // so purge segments get tagged Unknown rather than Custom — and the
    // bbox filter that excludes Custom from auto-fit can't fire.
    //
    // The indexer must snapshot the last-seen ;TYPE: into each new layer
    // entry so load_layer can seed the parser.
    std::string gcode = R"(
;TYPE:Custom
G1 X10 Y10 Z0.2 F600
G1 X200 Y10 E5
;TYPE:Brim
G1 X20 Y20 Z0.4
G1 X50 Y50 E0.1
;TYPE:Outer wall
G1 X30 Y30 Z0.6
G1 X60 Y60 E0.2
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));
    REQUIRE(index.get_layer_count() == 3);

    REQUIRE(index.get_entry(0).start_feature_type == FeatureType::Custom);
    REQUIRE(index.get_entry(1).start_feature_type == FeatureType::Brim);
    REQUIRE(index.get_entry(2).start_feature_type == FeatureType::OuterWall);
}

TEST_CASE("GCodeLayerIndex - Real file", "[gcode][layer_index][integration]") {
    // Test with the real benchy file if it exists
    std::ifstream check("assets/test_gcodes/3DBenchy.gcode");
    if (!check.good()) {
        SKIP("Test G-code file not found (run from project root)");
    }
    check.close();

    GCodeLayerIndex index;
    REQUIRE(index.build_from_file("assets/test_gcodes/3DBenchy.gcode"));

    const auto& stats = index.get_stats();
    INFO("Benchy: " << stats.total_layers << " layers, " << stats.total_lines << " lines");
    INFO("Z range: [" << stats.min_z << ", " << stats.max_z << "]");
    INFO("Build time: " << stats.build_time_ms << "ms");
    INFO("Memory: " << index.memory_usage_bytes() << " bytes");

    REQUIRE(index.get_layer_count() > 10); // Benchy should have many layers
    REQUIRE(stats.min_z < 1.0f);           // First layer should be < 1mm
    REQUIRE(stats.max_z > 10.0f);          // Benchy is ~48mm tall
}

TEST_CASE("GCodeLayerIndex - Clear and reuse", "[gcode][layer_index]") {
    std::string gcode1 = "G1 Z0.2 E0.1\nG1 Z0.4 E0.2\n";
    std::string gcode2 = "G1 Z0.3 E0.1\nG1 Z0.6 E0.2\nG1 Z0.9 E0.3\n";

    TempGCodeFile file1(gcode1);
    TempGCodeFile file2(gcode2);

    GCodeLayerIndex index;

    // Build first index
    REQUIRE(index.build_from_file(file1.path()));
    REQUIRE(index.get_layer_count() == 2);

    // Clear and build second
    index.clear();
    REQUIRE(!index.is_valid());

    REQUIRE(index.build_from_file(file2.path()));
    REQUIRE(index.get_layer_count() == 3);
}

TEST_CASE("GCodeLayerIndex - Invalid file", "[gcode][layer_index]") {
    GCodeLayerIndex index;

    SECTION("Non-existent file") {
        REQUIRE(!index.build_from_file("/nonexistent/path/file.gcode"));
        REQUIRE(!index.is_valid());
    }

    SECTION("Empty file") {
        TempGCodeFile empty("");
        REQUIRE(!index.build_from_file(empty.path()));
        REQUIRE(!index.is_valid());
    }
}

TEST_CASE("GCodeLayerIndex - Filament color metadata", "[gcode][layer_index]") {
    SECTION("Single-color filament_colour parses to single-entry palette") {
        std::string gcode = R"(; filament_colour = #26A69A
G1 Z0.2 F1000
G1 X10 Y10 E1
)";
        TempGCodeFile file(gcode);
        GCodeLayerIndex index;
        REQUIRE(index.build_from_file(file.path()));
        const auto& s = index.get_stats();
        REQUIRE(s.filament_color == "#26A69A");
        REQUIRE(s.filament_palette.size() == 1);
        REQUIRE(s.filament_palette[0] == "#26A69A");
    }

    SECTION("Semicolon-separated extruder_colour parses full palette") {
        // Slicer multi-color metadata: palette[0]=black, palette[1]=off-white.
        // Without palette parsing, the renderer fell back to palette[0] for
        // every print regardless of which tool the gcode actually used.
        std::string gcode = R"(; extruder_colour = #000000;#F3FDFD
T1
G1 Z0.2 F1000
G1 X10 Y10 E1
)";
        TempGCodeFile file(gcode);
        GCodeLayerIndex index;
        REQUIRE(index.build_from_file(file.path()));
        const auto& s = index.get_stats();
        REQUIRE(s.filament_palette.size() == 2);
        REQUIRE(s.filament_palette[0] == "#000000");
        REQUIRE(s.filament_palette[1] == "#F3FDFD");
        REQUIRE(s.initial_tool_index == 1);
        // Legacy single-color field still tracks palette[0]
        REQUIRE(s.filament_color == "#000000");
    }

    SECTION("Footer-only metadata (OrcaSlicer) is found via end-of-file scan") {
        // OrcaSlicer dumps slicer config at end of file. Build a file >1000 lines
        // so the header scan times out, then put metadata in the last 32KB.
        std::string gcode = "G1 Z0.2 F1000\n";
        for (int i = 0; i < 1100; ++i) {
            gcode += "G1 X" + std::to_string(i) + " Y0 E0.1\n";
        }
        gcode += "; extruder_colour = #AABBCC;#112233\n";
        TempGCodeFile file(gcode);
        GCodeLayerIndex index;
        REQUIRE(index.build_from_file(file.path()));
        const auto& s = index.get_stats();
        REQUIRE(s.filament_palette.size() == 2);
        REQUIRE(s.filament_palette[1] == "#112233");
    }
}

TEST_CASE("GCodeLayerIndex - prologue Z move before the first marker is not a layer",
          "[gcode][layer_index]") {
    // OrcaSlicer emits its START_PRINT prologue - including a tall Z clearance
    // move - before the first ;LAYER_CHANGE. use_layer_markers only latches on
    // that first marker, so until then the Z-change heuristic is live and
    // happily turns the prologue lift into layer 0.
    //
    // Two consequences, both observed on a K1C printing flowrate_0_PLA_36m5s:
    //   1. One phantom layer (11 entries for a 10-layer file).
    //   2. stats_.min_z latches the prologue height (3.0), while max_z ends up
    //      at the last layer's Z-hop (2.4). min_z > max_z makes the renderer's
    //      AABB report is_empty(), so compute_auto_fit() bails to its
    //      degenerate default (scale 1.0, offset 0,0,0) and the 2D preview
    //      renders unscaled around the world origin - clipped off the top of
    //      the viewport.
    std::string gcode = R"(
START_PRINT EXTRUDER_TEMP=220 BED_TEMP=55
G1 Z3 F600
G1 Z1 F600
;LAYER_CHANGE
;Z:0.2
G1 Z0.6 F30000
G1 Z0.2
G1 X10 Y10 E0.1
;LAYER_CHANGE
;Z:0.4
G1 Z0.8 F30000
G1 Z0.4
G1 X20 Y20 E0.2
;LAYER_CHANGE
;Z:0.6
G1 Z1.0 F30000
G1 Z0.6
G1 X30 Y30 E0.3
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));

    // The prologue lift must not become a layer.
    REQUIRE(index.get_layer_count() == 3);

    const auto& stats = index.get_stats();
    // Z bounds must stay orderable - an inverted pair silently disables
    // auto-fit for the whole model.
    REQUIRE(stats.min_z <= stats.max_z);
    // And must describe the model, not the prologue clearance height.
    REQUIRE(stats.min_z < 3.0f);
}

TEST_CASE("GCodeLayerIndex - Z stats stay ordered when layer Z decreases", "[gcode][layer_index]") {
    // Belt and braces for the pair above: whatever the detection mode decides
    // is a layer, min_z/max_z must bracket every layer Z seen rather than
    // recording "first" and "last".
    std::string gcode = R"(
;LAYER_CHANGE
G1 Z0.6 E0.1
G1 X10 E0.2
;LAYER_CHANGE
G1 Z0.4 E0.3
G1 X20 E0.4
;LAYER_CHANGE
G1 Z0.2 E0.5
G1 X30 E0.6
)";

    TempGCodeFile file(gcode);
    GCodeLayerIndex index;
    REQUIRE(index.build_from_file(file.path()));

    const auto& stats = index.get_stats();
    REQUIRE(stats.min_z <= stats.max_z);
    REQUIRE(stats.min_z == Approx(0.2f));
    REQUIRE(stats.max_z == Approx(0.6f));
}
