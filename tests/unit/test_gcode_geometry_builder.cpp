// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_geometry_builder.h"
#include "gcode_parser.h"

#include <glm/gtc/matrix_transform.hpp>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using Catch::Approx;

// ============================================================================
// QuantizationParams Tests
// ============================================================================

TEST_CASE("Geometry Builder: QuantizationParams - calculate scale from bounding box",
          "[gcode][geometry][quantization]") {
    QuantizationParams params;
    AABB bbox;
    bbox.min = glm::vec3(-100.0f, -100.0f, 0.0f);
    bbox.max = glm::vec3(100.0f, 100.0f, 100.0f);

    params.calculate_scale(bbox);

    REQUIRE(params.min_bounds.x == Approx(-100.0f));
    REQUIRE(params.min_bounds.y == Approx(-100.0f));
    REQUIRE(params.min_bounds.z == Approx(0.0f));
    REQUIRE(params.max_bounds.x == Approx(100.0f));
    REQUIRE(params.max_bounds.y == Approx(100.0f));
    REQUIRE(params.max_bounds.z == Approx(100.0f));
    REQUIRE(params.scale_factor > 0.0f);
}

TEST_CASE("Geometry Builder: QuantizationParams - quantize and dequantize round trip",
          "[gcode][geometry][quantization]") {
    QuantizationParams params;
    AABB bbox;
    bbox.min = glm::vec3(0.0f, 0.0f, 0.0f);
    bbox.max = glm::vec3(200.0f, 200.0f, 200.0f);
    params.calculate_scale(bbox);

    SECTION("Quantize single value") {
        float original = 100.0f;
        int16_t quantized = params.quantize(original, bbox.min.x);
        float dequantized = params.dequantize(quantized, bbox.min.x);

        // Should be very close (within quantization error)
        REQUIRE(dequantized == Approx(original).margin(0.01f));
    }

    SECTION("Quantize vec3") {
        glm::vec3 original(50.0f, 100.0f, 150.0f);
        QuantizedVertex quantized = params.quantize_vec3(original);
        glm::vec3 dequantized = params.dequantize_vec3(quantized);

        REQUIRE(dequantized.x == Approx(original.x).margin(0.01f));
        REQUIRE(dequantized.y == Approx(original.y).margin(0.01f));
        REQUIRE(dequantized.z == Approx(original.z).margin(0.01f));
    }

    SECTION("Quantize boundary values") {
        glm::vec3 min_point = bbox.min;
        glm::vec3 max_point = bbox.max;

        QuantizedVertex qmin = params.quantize_vec3(min_point);
        QuantizedVertex qmax = params.quantize_vec3(max_point);

        glm::vec3 dmin = params.dequantize_vec3(qmin);
        glm::vec3 dmax = params.dequantize_vec3(qmax);

        REQUIRE(dmin.x == Approx(min_point.x).margin(0.01f));
        REQUIRE(dmax.x == Approx(max_point.x).margin(0.01f));
    }
}

TEST_CASE("Geometry Builder: QuantizationParams - degenerate bounding box",
          "[gcode][geometry][quantization][edge]") {
    QuantizationParams params;
    AABB bbox;
    bbox.min = glm::vec3(0.0f, 0.0f, 0.0f);
    bbox.max = glm::vec3(0.0f, 0.0f, 0.0f); // Zero-size box

    params.calculate_scale(bbox);

    // Should fall back to default scale factor
    REQUIRE(params.scale_factor == Approx(1000.0f));
}

TEST_CASE("Geometry Builder: QuantizationParams - large build volume",
          "[gcode][geometry][quantization]") {
    QuantizationParams params;
    AABB bbox;
    bbox.min = glm::vec3(-150.0f, -150.0f, 0.0f);
    bbox.max = glm::vec3(150.0f, 150.0f, 300.0f); // 300x300x300mm
    params.calculate_scale(bbox);

    // Test corners
    glm::vec3 corner1 = bbox.min;
    glm::vec3 corner2 = bbox.max;

    QuantizedVertex q1 = params.quantize_vec3(corner1);
    QuantizedVertex q2 = params.quantize_vec3(corner2);

    glm::vec3 d1 = params.dequantize_vec3(q1);
    glm::vec3 d2 = params.dequantize_vec3(q2);

    REQUIRE(d1.x == Approx(corner1.x).margin(0.02f));
    REQUIRE(d2.z == Approx(corner2.z).margin(0.02f));
}

// ============================================================================
// SimplificationOptions Tests
// ============================================================================

TEST_CASE("Geometry Builder: SimplificationOptions - validate clamps values",
          "[gcode][geometry][simplification]") {
    SimplificationOptions options;

    SECTION("Tolerance too small") {
        options.tolerance_mm = 0.0001f;
        options.validate();
        REQUIRE(options.tolerance_mm == Approx(0.001f)); // Clamped to min
    }

    SECTION("Tolerance too large") {
        options.tolerance_mm = 10.0f;
        options.validate();
        REQUIRE(options.tolerance_mm == Approx(5.0f)); // Clamped to max (5.0mm)
    }

    SECTION("Valid tolerance") {
        options.tolerance_mm = 0.15f;
        options.validate();
        REQUIRE(options.tolerance_mm == Approx(0.15f)); // Unchanged
    }

    SECTION("Min segment length too small") {
        options.min_segment_length_mm = 0.00001f;
        options.validate();
        REQUIRE(options.min_segment_length_mm == Approx(0.0001f)); // Clamped to min
    }
}

// ============================================================================
// RibbonGeometry Tests
// ============================================================================

TEST_CASE("Geometry Builder: RibbonGeometry - construction and destruction",
          "[gcode][geometry][ribbon]") {
    RibbonGeometry geometry;

    REQUIRE(geometry.vertices.empty());
    REQUIRE(geometry.indices.empty());
    REQUIRE(geometry.strips.empty());
    REQUIRE(geometry.strip_color_index.empty());
    REQUIRE(geometry.color_palette.empty());
    REQUIRE(geometry.color_cache != nullptr);
}

TEST_CASE("Geometry Builder: RibbonVertex is 8 bytes", "[gcode][geometry][ribbon]") {
    // The vertex pool dominates geometry memory on large gcode (millions of
    // entries), so every byte here is multiplied by ~5N per extrusion segment.
    // 6 B quantized position + 2 B octahedral normal, no padding at align 2.
    // Adding a per-vertex color_index back would pad this to 10 B for no gain —
    // color lives in RibbonGeometry::strip_color_index instead.
    STATIC_REQUIRE(sizeof(RibbonVertex) == 8);
    STATIC_REQUIRE(alignof(RibbonVertex) == 2);
    STATIC_REQUIRE(offsetof(RibbonVertex, normal) == 6);
}

TEST_CASE("Geometry Builder: RibbonGeometry - move semantics", "[gcode][geometry][ribbon]") {
    RibbonGeometry geom1;
    geom1.vertices.push_back({{100, 200, 300}, {0, 0}});
    geom1.extrusion_triangle_count = 42;

    RibbonGeometry geom2(std::move(geom1));

    REQUIRE(geom2.vertices.size() == 1);
    REQUIRE(geom2.extrusion_triangle_count == 42);
    REQUIRE(geom2.color_cache != nullptr);
}

TEST_CASE("Geometry Builder: RibbonGeometry - clear", "[gcode][geometry][ribbon]") {
    RibbonGeometry geometry;
    geometry.vertices.push_back({{100, 200, 300}, {0, 0}});
    geometry.strips.push_back({0, 1, 2, 3});
    geometry.strip_color_index.push_back(0);
    geometry.color_palette.push_back(0xFF0000);
    geometry.extrusion_triangle_count = 10;

    geometry.clear();

    REQUIRE(geometry.vertices.empty());
    REQUIRE(geometry.strips.empty());
    REQUIRE(geometry.strip_color_index.empty());
    REQUIRE(geometry.color_palette.empty());
    REQUIRE(geometry.extrusion_triangle_count == 0);
}

TEST_CASE("Geometry Builder: RibbonGeometry - memory usage", "[gcode][geometry][ribbon]") {
    RibbonGeometry geometry;

    size_t empty_memory = geometry.memory_usage();
    REQUIRE(empty_memory == 0);

    // Add some data
    geometry.vertices.push_back({{100, 200, 300}, {0, 0}});
    geometry.strips.push_back({0, 1, 2, 3});
    geometry.strip_color_index.push_back(0);
    geometry.color_palette.push_back(0xFF0000);

    size_t used_memory = geometry.memory_usage();
    REQUIRE(used_memory > empty_memory);

    // strip_color_index must be accounted for, not silently omitted.
    size_t before_colors = geometry.memory_usage();
    geometry.strip_color_index.resize(1000, 0);
    REQUIRE(geometry.memory_usage() == before_colors + 999);
}

// ============================================================================
// GeometryBuilder - Color Tests
// ============================================================================

TEST_CASE("Geometry Builder: Color computation - hex parsing", "[gcode][geometry][color]") {
    // Helper to create minimal G-code for color testing
    auto make_single_segment_gcode = []() {
        ParsedGCodeFile gcode;
        gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
        gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

        Layer layer;
        layer.z_height = 0.2f;
        ToolpathSegment seg;
        seg.start = glm::vec3(0, 0, 0.2f);
        seg.end = glm::vec3(10, 0, 0.2f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 1.0f;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
        gcode.layers.push_back(layer);

        return gcode;
    };

    SimplificationOptions options;
    options.enable_merging = false;

    SECTION("Parse with # prefix") {
        GeometryBuilder builder;
        builder.set_filament_color("#26A69A"); // OrcaSlicer teal

        auto gcode = make_single_segment_gcode();
        RibbonGeometry geometry = builder.build(gcode, options);

        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() >= 1);

        // Verify the teal color (0x26A69A) is in the palette
        uint32_t expected_color = 0x26A69A;
        bool found_expected = false;
        for (uint32_t color : geometry.color_palette) {
            if (color == expected_color) {
                found_expected = true;
                break;
            }
        }
        REQUIRE(found_expected);
    }

    SECTION("Parse without # prefix") {
        GeometryBuilder builder;
        builder.set_filament_color("FF0000"); // Red

        auto gcode = make_single_segment_gcode();
        RibbonGeometry geometry = builder.build(gcode, options);

        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() >= 1);

        // Verify red (0xFF0000) is in the palette
        uint32_t expected_color = 0xFF0000;
        bool found_expected = false;
        for (uint32_t color : geometry.color_palette) {
            if (color == expected_color) {
                found_expected = true;
                break;
            }
        }
        REQUIRE(found_expected);
    }

    SECTION("Invalid color string defaults to black") {
        GeometryBuilder builder;
        builder.set_filament_color("XYZ"); // Invalid hex

        auto gcode = make_single_segment_gcode();
        RibbonGeometry geometry = builder.build(gcode, options);

        // Should not crash and should produce geometry
        REQUIRE(geometry.vertices.size() > 0);
        REQUIRE(geometry.color_palette.size() >= 1);

        // strtol("XYZ", nullptr, 16) returns 0, so expect black (0x000000)
        uint32_t expected_color = 0x000000;
        bool found_expected = false;
        for (uint32_t color : geometry.color_palette) {
            if (color == expected_color) {
                found_expected = true;
                break;
            }
        }
        REQUIRE(found_expected);
    }
}

TEST_CASE("Geometry Builder: Color computation - Z-height gradient", "[gcode][geometry][color]") {
    GeometryBuilder builder;
    builder.set_use_height_gradient(true);

    // Create a simple G-code file with two layers
    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer1;
    layer1.z_height = 0.2f;
    ToolpathSegment seg1;
    seg1.start = glm::vec3(0, 0, 0.2f);
    seg1.end = glm::vec3(10, 0, 0.2f);
    seg1.is_extrusion = true;
    seg1.extrusion_amount = 1.0f;
    seg1.width = 0.4f;
    layer1.segments.push_back(seg1);
    gcode.layers.push_back(layer1);

    Layer layer2;
    layer2.z_height = 5.0f;
    ToolpathSegment seg2;
    seg2.start = glm::vec3(0, 0, 5.0f);
    seg2.end = glm::vec3(10, 0, 5.0f);
    seg2.is_extrusion = true;
    seg2.extrusion_amount = 1.0f;
    seg2.width = 0.4f;
    layer2.segments.push_back(seg2);
    gcode.layers.push_back(layer2);

    SimplificationOptions options;
    options.enable_merging = false;

    RibbonGeometry geometry = builder.build(gcode, options);

    // Should have generated geometry
    REQUIRE(geometry.vertices.size() > 0);
    REQUIRE(geometry.color_palette.size() > 0);
}

TEST_CASE("Geometry Builder: Color computation - solid filament color",
          "[gcode][geometry][color]") {
    GeometryBuilder builder;
    builder.set_filament_color("#ED1C24"); // Red
    builder.set_use_height_gradient(false);

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;
    ToolpathSegment seg;
    seg.start = glm::vec3(0, 0, 0.2f);
    seg.end = glm::vec3(10, 0, 0.2f);
    seg.is_extrusion = true;
    seg.extrusion_amount = 1.0f;
    seg.width = 0.4f;
    layer.segments.push_back(seg);
    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = false;

    RibbonGeometry geometry = builder.build(gcode, options);

    // Should use solid color (fewer palette entries than gradient)
    REQUIRE(geometry.color_palette.size() >= 1);
}

// ============================================================================
// GeometryBuilder - Segment Simplification Tests
// ============================================================================

TEST_CASE("Geometry Builder: Segment simplification - collinear merging",
          "[gcode][geometry][simplification]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    // Three collinear segments that should merge
    ToolpathSegment seg1;
    seg1.start = glm::vec3(0, 0, 0.2f);
    seg1.end = glm::vec3(10, 0, 0.2f);
    seg1.is_extrusion = true;
    seg1.extrusion_amount = 1.0f;
    seg1.width = 0.4f;
    layer.segments.push_back(seg1);

    ToolpathSegment seg2;
    seg2.start = glm::vec3(10, 0, 0.2f);
    seg2.end = glm::vec3(20, 0, 0.2f);
    seg2.is_extrusion = true;
    seg2.extrusion_amount = 1.0f;
    seg2.width = 0.4f;
    layer.segments.push_back(seg2);

    ToolpathSegment seg3;
    seg3.start = glm::vec3(20, 0, 0.2f);
    seg3.end = glm::vec3(30, 0, 0.2f);
    seg3.is_extrusion = true;
    seg3.extrusion_amount = 1.0f;
    seg3.width = 0.4f;
    layer.segments.push_back(seg3);

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = true;
    options.tolerance_mm = 0.1f;

    RibbonGeometry geometry = builder.build(gcode, options);

    // Check statistics
    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 3);
    REQUIRE(stats.output_segments < stats.input_segments); // Should have merged
}

TEST_CASE("Geometry Builder: Segment simplification - non-collinear preservation",
          "[gcode][geometry][simplification]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    // Two segments at 90 degrees - should NOT merge
    ToolpathSegment seg1;
    seg1.start = glm::vec3(0, 0, 0.2f);
    seg1.end = glm::vec3(10, 0, 0.2f);
    seg1.is_extrusion = true;
    seg1.extrusion_amount = 1.0f;
    seg1.width = 0.4f;
    layer.segments.push_back(seg1);

    ToolpathSegment seg2;
    seg2.start = glm::vec3(10, 0, 0.2f);
    seg2.end = glm::vec3(10, 10, 0.2f); // 90 degree turn
    seg2.is_extrusion = true;
    seg2.extrusion_amount = 1.0f;
    seg2.width = 0.4f;
    layer.segments.push_back(seg2);

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = true;
    options.tolerance_mm = 0.1f;

    RibbonGeometry geometry = builder.build(gcode, options);

    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 2);
    REQUIRE(stats.output_segments == 2); // Should NOT merge
}

TEST_CASE("Geometry Builder: Segment simplification - disabled",
          "[gcode][geometry][simplification]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    for (int i = 0; i < 5; i++) {
        ToolpathSegment seg;
        seg.start = glm::vec3(i * 10.0f, 0, 0.2f);
        seg.end = glm::vec3((i + 1) * 10.0f, 0, 0.2f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 1.0f;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = false; // DISABLED

    RibbonGeometry geometry = builder.build(gcode, options);

    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 5);
    REQUIRE(stats.output_segments == 5); // No simplification
    REQUIRE(stats.simplification_ratio == Approx(0.0f));
}

// ============================================================================
// GeometryBuilder - Geometry Generation Tests
// ============================================================================

TEST_CASE("Geometry Builder: Geometry generation - single segment",
          "[gcode][geometry][generation]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    ToolpathSegment seg;
    seg.start = glm::vec3(0, 0, 0.2f);
    seg.end = glm::vec3(10, 0, 0.2f);
    seg.is_extrusion = true;
    seg.extrusion_amount = 1.0f;
    seg.width = 0.4f;
    layer.segments.push_back(seg);

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    // Should have generated vertices and triangles
    REQUIRE(geometry.vertices.size() > 0);
    REQUIRE(geometry.strips.size() > 0);
    REQUIRE(geometry.strip_color_index.size() == geometry.strips.size());
    REQUIRE(geometry.color_palette.size() > 0);
}

TEST_CASE("Geometry Builder: strips and strip_color_index stay in lockstep after build()",
          "[gcode][geometry][generation]") {
    // The two vectors are pushed from three separate sites in
    // generate_ribbon_vertices (side faces, start cap fan, end cap fan). A
    // missed push on any of them silently shifts every later strip's color.
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    // Several layers with several segments each, so both cap paths and the
    // vertex-sharing path between connected segments are all exercised.
    for (int l = 0; l < 4; ++l) {
        Layer layer;
        layer.z_height = 0.2f * static_cast<float>(l + 1);
        for (int s = 0; s < 5; ++s) {
            ToolpathSegment seg;
            seg.start = {10.0f + static_cast<float>(s) * 5.0f, 10.0f, layer.z_height};
            seg.end = {15.0f + static_cast<float>(s) * 5.0f, 10.0f, layer.z_height};
            seg.is_extrusion = true;
            seg.width = 0.4f;
            seg.layer_index = static_cast<uint16_t>(l);
            layer.segments.push_back(seg);
        }
        gcode.layers.push_back(std::move(layer));
    }
    gcode.total_segments = 20;

    SimplificationOptions options;
    options.enable_merging = false;
    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.strips.size() > 0);
    REQUIRE(geometry.strip_color_index.size() == geometry.strips.size());
    REQUIRE(geometry.strip_layer_index.size() == geometry.strips.size());

    // Every recorded index must resolve inside the palette — an out-of-range
    // entry would silently fall back to the default teal at expansion time.
    for (uint8_t ci : geometry.strip_color_index) {
        REQUIRE(ci < geometry.color_palette.size());
    }
}

TEST_CASE("Geometry Builder: Geometry generation - empty G-code",
          "[gcode][geometry][generation][edge]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    // Should handle gracefully
    REQUIRE(geometry.vertices.size() == 0);
    REQUIRE(geometry.strips.size() == 0);
}

TEST_CASE("Geometry Builder: Geometry generation - travel moves skipped",
          "[gcode][geometry][generation]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    // Travel move (should be skipped)
    ToolpathSegment travel;
    travel.start = glm::vec3(0, 0, 0.2f);
    travel.end = glm::vec3(10, 0, 0.2f);
    travel.is_extrusion = false; // Travel move
    layer.segments.push_back(travel);

    // Extrusion move (should be rendered)
    ToolpathSegment extrusion;
    extrusion.start = glm::vec3(10, 0, 0.2f);
    extrusion.end = glm::vec3(20, 0, 0.2f);
    extrusion.is_extrusion = true;
    extrusion.extrusion_amount = 1.0f;
    extrusion.width = 0.4f;
    layer.segments.push_back(extrusion);

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = false;

    RibbonGeometry geometry = builder.build(gcode, options);

    // Should only generate geometry for extrusion move
    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 2);
    REQUIRE(geometry.extrusion_triangle_count > 0);
    REQUIRE(geometry.travel_triangle_count == 0);
}

TEST_CASE("Geometry Builder: Geometry generation - multiple layers",
          "[gcode][geometry][generation]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    // Layer 1
    Layer layer1;
    layer1.z_height = 0.2f;
    ToolpathSegment seg1;
    seg1.start = glm::vec3(0, 0, 0.2f);
    seg1.end = glm::vec3(10, 0, 0.2f);
    seg1.is_extrusion = true;
    seg1.extrusion_amount = 1.0f;
    seg1.width = 0.4f;
    layer1.segments.push_back(seg1);
    gcode.layers.push_back(layer1);

    // Layer 2
    Layer layer2;
    layer2.z_height = 0.4f;
    ToolpathSegment seg2;
    seg2.start = glm::vec3(0, 0, 0.4f);
    seg2.end = glm::vec3(10, 0, 0.4f);
    seg2.is_extrusion = true;
    seg2.extrusion_amount = 1.0f;
    seg2.width = 0.4f;
    layer2.segments.push_back(seg2);
    gcode.layers.push_back(layer2);

    // Layer 3
    Layer layer3;
    layer3.z_height = 0.6f;
    ToolpathSegment seg3;
    seg3.start = glm::vec3(0, 0, 0.6f);
    seg3.end = glm::vec3(10, 0, 0.6f);
    seg3.is_extrusion = true;
    seg3.extrusion_amount = 1.0f;
    seg3.width = 0.4f;
    layer3.segments.push_back(seg3);
    gcode.layers.push_back(layer3);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 3);
    REQUIRE(geometry.vertices.size() > 0);
}

TEST_CASE("Geometry Builder: Geometry generation - very short segment",
          "[gcode][geometry][generation][edge]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    // Extremely short segment (0.01mm)
    ToolpathSegment seg;
    seg.start = glm::vec3(10.0f, 10.0f, 0.2f);
    seg.end = glm::vec3(10.01f, 10.0f, 0.2f);
    seg.is_extrusion = true;
    seg.extrusion_amount = 0.001f;
    seg.width = 0.4f;
    layer.segments.push_back(seg);

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    // Should handle without crashing
    REQUIRE(geometry.vertices.size() > 0);
}

// ============================================================================
// GeometryBuilder - Configuration Tests
// ============================================================================

TEST_CASE("Geometry Builder: Configuration - extrusion width", "[gcode][geometry][config]") {
    GeometryBuilder builder;
    builder.set_extrusion_width(0.5f);

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;
    ToolpathSegment seg;
    seg.start = glm::vec3(0, 0, 0.2f);
    seg.end = glm::vec3(10, 0, 0.2f);
    seg.is_extrusion = true;
    seg.extrusion_amount = 1.0f;
    seg.width = 0.0f; // Should use configured width
    layer.segments.push_back(seg);
    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.vertices.size() > 0);
}

TEST_CASE("Geometry Builder: Configuration - layer height", "[gcode][geometry][config]") {
    GeometryBuilder builder;
    builder.set_layer_height(0.3f); // Non-default

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.3f;
    ToolpathSegment seg;
    seg.start = glm::vec3(0, 0, 0.3f);
    seg.end = glm::vec3(10, 0, 0.3f);
    seg.is_extrusion = true;
    seg.extrusion_amount = 1.0f;
    seg.width = 0.4f;
    layer.segments.push_back(seg);
    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.vertices.size() > 0);
}

// ============================================================================
// GeometryBuilder - Real-world Scenarios
// ============================================================================

TEST_CASE("Geometry Builder: Real-world - calibration cube perimeter",
          "[gcode][geometry][realworld]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(90, 90, 0);
    gcode.global_bounding_box.max = glm::vec3(110, 110, 20);

    Layer layer;
    layer.z_height = 0.2f;

    // Square perimeter (20mm cube)
    std::vector<glm::vec3> points = {
        glm::vec3(95, 95, 0.2f), glm::vec3(105, 95, 0.2f), glm::vec3(105, 105, 0.2f),
        glm::vec3(95, 105, 0.2f), glm::vec3(95, 95, 0.2f) // Close loop
    };

    for (size_t i = 0; i < points.size() - 1; i++) {
        ToolpathSegment seg;
        seg.start = points[i];
        seg.end = points[i + 1];
        seg.is_extrusion = true;
        seg.extrusion_amount = 0.5f;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.vertices.size() > 0);
    REQUIRE(geometry.extrusion_triangle_count > 0);

    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 4);
}

TEST_CASE("Geometry Builder: Real-world - benchy hull curve", "[gcode][geometry][realworld]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 50);

    Layer layer;
    layer.z_height = 10.0f;

    // Curved path (approximating hull)
    for (int i = 0; i < 20; i++) {
        float angle1 = i * M_PI / 20.0f;
        float angle2 = (i + 1) * M_PI / 20.0f;

        ToolpathSegment seg;
        seg.start = glm::vec3(50 + 20 * cos(angle1), 50 + 20 * sin(angle1), 10.0f);
        seg.end = glm::vec3(50 + 20 * cos(angle2), 50 + 20 * sin(angle2), 10.0f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 0.3f;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = true;
    options.tolerance_mm = 0.1f;

    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.vertices.size() > 0);

    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 20);
    // Curve should not simplify much due to direction changes
    REQUIRE(stats.output_segments > 15);
}

TEST_CASE("Geometry Builder: Real-world - sparse infill pattern", "[gcode][geometry][realworld]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 50);

    Layer layer;
    layer.z_height = 5.0f;

    // Diagonal infill lines (rectilinear pattern)
    for (int i = 0; i < 10; i++) {
        float y = 10.0f + i * 8.0f;

        // Line from left to right
        ToolpathSegment seg;
        seg.start = glm::vec3(10, y, 5.0f);
        seg.end = glm::vec3(90, y, 5.0f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 2.0f;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.vertices.size() > 0);
    REQUIRE(geometry.extrusion_triangle_count > 0);
}

// ============================================================================
// BuildStats Tests
// ============================================================================

TEST_CASE("Geometry Builder: BuildStats - statistics tracking", "[gcode][geometry][stats]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(100, 100, 10);

    Layer layer;
    layer.z_height = 0.2f;

    // Add 10 segments
    for (int i = 0; i < 10; i++) {
        ToolpathSegment seg;
        seg.start = glm::vec3(i * 10.0f, 0, 0.2f);
        seg.end = glm::vec3((i + 1) * 10.0f, 0, 0.2f);
        seg.is_extrusion = true;
        seg.extrusion_amount = 1.0f;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }

    gcode.layers.push_back(layer);

    SimplificationOptions options;
    options.enable_merging = true;

    RibbonGeometry geometry = builder.build(gcode, options);

    const auto& stats = builder.last_stats();

    REQUIRE(stats.input_segments == 10);
    REQUIRE(stats.output_segments > 0);
    REQUIRE(stats.output_segments <= stats.input_segments);
    REQUIRE(stats.vertices_generated > 0);
    REQUIRE(stats.triangles_generated > 0);
    REQUIRE(stats.memory_bytes > 0);
    REQUIRE(stats.simplification_ratio >= 0.0f);
    REQUIRE(stats.simplification_ratio <= 1.0f);
}

// ============================================================================
// Budget integration tests
// ============================================================================

TEST_CASE("GeometryBuilder: respects tube_sides from BudgetConfig", "[gcode][budget][builder]") {
    // Create a small test gcode with non-collinear segments to prevent merging
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;
    for (int i = 0; i < 100; ++i) {
        ToolpathSegment seg;
        float x = static_cast<float>(i);
        float y = (i % 2 == 0) ? 0.0f : 1.0f; // Zig-zag to prevent merging
        seg.start = {x, y, 0.2f};
        seg.end = {x + 1.0f, (i % 2 == 0) ? 1.0f : 0.0f, 0.2f};
        seg.is_extrusion = true;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }
    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 100;
    gcode.global_bounding_box.expand({0, 0, 0});
    gcode.global_bounding_box.expand({101, 2, 1});

    SimplificationOptions opts;
    opts.enable_merging = false; // Ensure all segments are processed

    GeometryBuilder builder;
    // Build with default (uses config tube_sides)
    auto geom_default = builder.build(gcode, opts);
    size_t verts_default = geom_default.vertices.size();

    // Build with budget config forcing tube_sides=4
    GeometryBuilder builder4;
    builder4.set_budget_tube_sides(4);
    auto geom_4 = builder4.build(gcode, opts);
    size_t verts_4 = geom_4.vertices.size();

    // N=4 should produce fewer or equal vertices than default
    REQUIRE(verts_4 <= verts_default);
}

TEST_CASE("GeometryBuilder: budget abort returns with flag set", "[gcode][budget][builder]") {
    // Create gcode with enough non-collinear segments to exceed a tiny budget.
    // Zig-zag pattern prevents simplification from merging segments.
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;
    for (int i = 0; i < 10000; ++i) {
        ToolpathSegment seg;
        float x = static_cast<float>(i) * 0.5f;
        float y = (i % 2 == 0) ? 0.0f : 1.0f; // Zig-zag to prevent merging
        seg.start = {x, y, 0.2f};
        seg.end = {x + 0.5f, (i % 2 == 0) ? 1.0f : 0.0f, 0.2f};
        seg.is_extrusion = true;
        seg.width = 0.4f;
        layer.segments.push_back(seg);
    }
    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 10000;
    gcode.global_bounding_box.expand({0, 0, 0});
    gcode.global_bounding_box.expand({5001, 2, 1});

    GeometryBuilder builder;
    builder.set_budget_tube_sides(4);
    builder.set_budget_limit(1024); // 1KB budget — impossibly small

    SimplificationOptions opts;
    opts.enable_merging = false; // Ensure all segments are processed
    auto geom = builder.build(gcode, opts);

    // Build should have aborted
    REQUIRE(builder.was_budget_exceeded());
}

// ============================================================================
// prepare_interleaved_buffers() Tests
// ============================================================================

TEST_CASE("prepare_interleaved_buffers produces correct buffer count and vertex counts",
          "[gcode][geometry][prepared_buffers]") {
    // Build geometry from a 2-layer gcode
    ParsedGCodeFile gcode;

    // Layer 0
    Layer layer0;
    layer0.z_height = 0.2f;
    for (int i = 0; i < 3; ++i) {
        ToolpathSegment seg;
        float x = static_cast<float>(i) * 5.0f;
        seg.start = {x, 0.0f, 0.2f};
        seg.end = {x + 4.0f, 0.0f, 0.2f};
        seg.is_extrusion = true;
        seg.width = 0.4f;
        layer0.segments.push_back(seg);
    }
    gcode.layers.push_back(std::move(layer0));

    // Layer 1
    Layer layer1;
    layer1.z_height = 0.4f;
    for (int i = 0; i < 2; ++i) {
        ToolpathSegment seg;
        float x = static_cast<float>(i) * 5.0f;
        seg.start = {x, 0.0f, 0.4f};
        seg.end = {x + 4.0f, 0.0f, 0.4f};
        seg.is_extrusion = true;
        seg.width = 0.4f;
        layer1.segments.push_back(seg);
    }
    gcode.layers.push_back(std::move(layer1));

    gcode.total_segments = 5;
    gcode.global_bounding_box.expand({0, 0, 0.2f});
    gcode.global_bounding_box.expand({15, 1, 0.4f});

    GeometryBuilder builder;
    SimplificationOptions opts;
    opts.enable_merging = false;
    auto geom = builder.build(gcode, opts);

    REQUIRE(!geom.layer_strip_ranges.empty());

    geom.prepare_interleaved_buffers();

    // Buffer count matches number of layers
    REQUIRE(geom.prepared_buffers.size() == geom.layer_strip_ranges.size());

    // Each buffer's vertex_count == strip_count * 6 (2 triangles = 6 verts per strip)
    for (size_t i = 0; i < geom.layer_strip_ranges.size(); ++i) {
        size_t strip_count = geom.layer_strip_ranges[i].second;
        REQUIRE(geom.prepared_buffers[i].vertex_count == strip_count * 6);
        // Data is raw bytes — one PackedVertex (20B) per vertex.
        REQUIRE(geom.prepared_buffers[i].data.size() ==
                geom.prepared_buffers[i].vertex_count * PackedVertex::stride());
    }
}

TEST_CASE("prepare_interleaved_buffers data matches manual expansion",
          "[gcode][geometry][prepared_buffers]") {
    // Minimal geometry: 1 layer, 1 segment
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;
    ToolpathSegment seg;
    seg.start = {10.0f, 10.0f, 0.2f};
    seg.end = {20.0f, 10.0f, 0.2f};
    seg.is_extrusion = true;
    seg.width = 0.4f;
    layer.segments.push_back(seg);
    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 1;
    gcode.global_bounding_box.expand({10, 10, 0.2f});
    gcode.global_bounding_box.expand({20, 10, 0.2f});

    GeometryBuilder builder;
    SimplificationOptions opts;
    opts.enable_merging = false;
    auto geom = builder.build(gcode, opts);

    REQUIRE(!geom.strips.empty());
    REQUIRE(!geom.vertices.empty());

    geom.prepare_interleaved_buffers();

    REQUIRE(geom.prepared_buffers.size() >= 1);
    auto& buf = geom.prepared_buffers[0];
    REQUIRE(buf.vertex_count > 0);
    REQUIRE(buf.data.size() == buf.vertex_count * PackedVertex::stride());

    // Manually expand the first strip and compare against the packed layout.
    const auto& strip = geom.strips[0];
    static constexpr int TRI_INDICES[6] = {0, 1, 2, 1, 3, 2};
    const auto* packed = reinterpret_cast<const PackedVertex*>(buf.data.data());

    for (int ti = 0; ti < 6; ++ti) {
        const auto& vert = geom.vertices[strip[static_cast<size_t>(TRI_INDICES[ti])]];
        glm::vec3 pos = geom.quantization.dequantize_vec3(vert.position);

        // Color is per-strip, not per-vertex.
        uint32_t rgb = 0x26A69A; // Default teal
        if (!geom.strip_color_index.empty() &&
            geom.strip_color_index[0] < geom.color_palette.size()) {
            rgb = geom.color_palette[geom.strip_color_index[0]];
        }
        uint8_t expected_color[4];
        PackedVertex::encode_color(rgb, expected_color);
        // Normals are stored pre-encoded in the vertex — the packed bytes must
        // be a straight copy, not a re-encode.
        const int8_t expected_normal[2] = {vert.normal[0], vert.normal[1]};

        const auto& pv = packed[ti];
        // Positions are stored quantized and dequantized on the GPU, so the
        // packed bytes must be the raw int16 …
        REQUIRE(pv.position[0] == vert.position.x);
        REQUIRE(pv.position[1] == vert.position.y);
        REQUIRE(pv.position[2] == vert.position.z);
        // … and dequantizing them must still land on the mm coordinate the
        // renderer's matrix fold will produce.
        REQUIRE(geom.quantization.dequantize(pv.position[0], geom.quantization.min_bounds.x) ==
                Approx(pos.x).margin(0.01f));
        REQUIRE(geom.quantization.dequantize(pv.position[1], geom.quantization.min_bounds.y) ==
                Approx(pos.y).margin(0.01f));
        REQUIRE(geom.quantization.dequantize(pv.position[2], geom.quantization.min_bounds.z) ==
                Approx(pos.z).margin(0.01f));
        REQUIRE(pv.color[0] == expected_color[0]);
        REQUIRE(pv.color[1] == expected_color[1]);
        REQUIRE(pv.color[2] == expected_color[2]);
        REQUIRE(pv.color[3] == expected_color[3]);
        REQUIRE(pv.normal[0] == expected_normal[0]);
        REQUIRE(pv.normal[1] == expected_normal[1]);
    }
}

TEST_CASE("prepare_interleaved_buffers on empty geometry is no-op",
          "[gcode][geometry][prepared_buffers]") {
    RibbonGeometry geom;
    geom.prepare_interleaved_buffers();
    REQUIRE(geom.prepared_buffers.empty());
}

TEST_CASE("prepare_interleaved_buffers cleared by clearing prepared_buffers",
          "[gcode][geometry][prepared_buffers]") {
    // Build real geometry
    ParsedGCodeFile gcode;
    Layer layer;
    layer.z_height = 0.2f;
    ToolpathSegment seg;
    seg.start = {5.0f, 5.0f, 0.2f};
    seg.end = {15.0f, 5.0f, 0.2f};
    seg.is_extrusion = true;
    seg.width = 0.4f;
    layer.segments.push_back(seg);
    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 1;
    gcode.global_bounding_box.expand({5, 5, 0.2f});
    gcode.global_bounding_box.expand({15, 5, 0.2f});

    GeometryBuilder builder;
    SimplificationOptions opts;
    opts.enable_merging = false;
    auto geom = builder.build(gcode, opts);

    geom.prepare_interleaved_buffers();
    REQUIRE(!geom.prepared_buffers.empty());

    // Simulate color-override invalidation path
    geom.prepared_buffers.clear();
    REQUIRE(geom.prepared_buffers.empty());
}

// ============================================================================
// PackedVertex GPU layout
// ============================================================================

TEST_CASE("Geometry Builder: PackedVertex is 12 bytes with GL-friendly offsets",
          "[gcode][geometry][packed_vertex]") {
    // Positions ride to the GPU as raw quantized int16 and are dequantized by a
    // transform folded into u_mvp / u_model_view. Sending float positions
    // instead cost 8 bytes a vertex — 1.4 GB on a 28 MB gcode.
    STATIC_REQUIRE(sizeof(PackedVertex) == 12);
    STATIC_REQUIRE(PackedVertex::stride() == 12);

    // Offsets must stay put: the renderer hands these to glVertexAttribPointer.
    CHECK(PackedVertex::position_offset() == 0);
    CHECK(PackedVertex::normal_offset() == 6);
    CHECK(PackedVertex::color_offset() == 8);

    // Stride a multiple of 4 keeps GL drivers off the slow path, and the 4-byte
    // color attribute stays naturally aligned.
    CHECK(PackedVertex::stride() % 4 == 0);
    CHECK(PackedVertex::color_offset() % 4 == 0);
}

TEST_CASE("Geometry Builder: affine dequantization matches the matrix fold",
          "[gcode][geometry][packed_vertex]") {
    // The renderer replaces the CPU-side dequantize_vec3() with
    // translate(min_bounds) * scale(1/scale_factor) baked into its matrices.
    // If those two ever disagree the model silently renders at the wrong scale
    // or position, so pin the equivalence here.
    AABB bbox;
    bbox.min = {12.5f, -30.0f, 0.0f};
    bbox.max = {212.5f, 170.0f, 250.0f};

    QuantizationParams q;
    q.calculate_scale(bbox);

    const float inv = 1.0f / q.scale_factor;
    const glm::mat4 dequant =
        glm::translate(glm::mat4(1.0f), q.min_bounds) * glm::scale(glm::mat4(1.0f), glm::vec3(inv));

    for (const glm::vec3& p : {glm::vec3{12.5f, -30.0f, 0.0f}, glm::vec3{100.0f, 20.0f, 125.0f},
                               glm::vec3{212.5f, 170.0f, 250.0f}}) {
        const QuantizedVertex qv = q.quantize_vec3(p);
        const glm::vec3 cpu = q.dequantize_vec3(qv);
        const glm::vec4 gpu =
            dequant * glm::vec4(static_cast<float>(qv.x), static_cast<float>(qv.y),
                                static_cast<float>(qv.z), 1.0f);

        INFO("point " << p.x << "," << p.y << "," << p.z);
        CHECK(gpu.x == Approx(cpu.x).margin(0.001));
        CHECK(gpu.y == Approx(cpu.y).margin(0.001));
        CHECK(gpu.z == Approx(cpu.z).margin(0.001));
        CHECK(gpu.w == Approx(1.0f));
    }
}

TEST_CASE("Geometry Builder: moving RibbonGeometry preserves every member",
          "[gcode][geometry][move]") {
    // The move operations are hand-written, so a member added to the struct is
    // silently dropped unless it is also added here. layer_height_mm was being
    // reset to its 0.2f default on every move — and ui_gcode_viewer moves the
    // built geometry into its owning pointer, so the real renderer never saw a
    // non-default layer height.
    RibbonGeometry src;
    src.layer_height_mm = 0.32f;
    src.max_layer_index = 7;
    src.quantization.scale_factor = 123.5f;
    src.quantization.min_bounds = {1.0f, 2.0f, 3.0f};
    src.prepared_buffers.resize(3);
    src.prepared_buffers[1].vertex_count = 42;
    src.prepared_buffers[1].data.assign(42 * PackedVertex::stride(), uint8_t{7});
    src.strips.push_back({0, 1, 2, 3});
    src.strips.push_back({4, 5, 6, 7});
    src.strip_color_index = {3, 9};
    src.strip_layer_index = {0, 1};
    src.color_palette = {0x111111, 0x222222, 0x333333, 0x444444, 0x555555,
                         0x666666, 0x777777, 0x888888, 0x999999, 0xAAAAAA};
    src.object_runs = {ObjectRun{0, 12, 3}, ObjectRun{12, 6, 5}};
    src.layer_object_run_ranges = {{0, 1}, {1, 1}};

    SECTION("move construction") {
        RibbonGeometry dst(std::move(src));
        CHECK(dst.layer_height_mm == Approx(0.32f));
        CHECK(dst.max_layer_index == 7);
        CHECK(dst.quantization.scale_factor == Approx(123.5f));
        REQUIRE(dst.prepared_buffers.size() == 3);
        CHECK(dst.prepared_buffers[1].vertex_count == 42);
        CHECK(dst.prepared_buffers[1].data.size() == 42 * PackedVertex::stride());
        // Dropping strip_color_index on a move would recolor the whole model to
        // the default teal, since strip_color() falls back when the index is missing.
        REQUIRE(dst.strip_color_index.size() == dst.strips.size());
        CHECK(dst.strip_color_index[0] == 3);
        CHECK(dst.strip_color_index[1] == 9);
        CHECK(dst.strip_color(1) == 0xAAAAAA);
        CHECK(dst.strip_layer_index.size() == 2);
        // Dropping the run tables on a move would leave the GLES shell pass with
        // nothing to draw, which looks exactly like the feature not existing.
        REQUIRE(dst.object_runs.size() == 2);
        CHECK(dst.object_runs[1].object_index == 5);
        REQUIRE(dst.layer_object_runs(1).count == 1);
        CHECK(dst.layer_object_runs(1).first->vertex_offset == 12);
    }

    SECTION("move assignment") {
        RibbonGeometry dst;
        dst = std::move(src);
        CHECK(dst.layer_height_mm == Approx(0.32f));
        CHECK(dst.max_layer_index == 7);
        CHECK(dst.quantization.scale_factor == Approx(123.5f));
        REQUIRE(dst.prepared_buffers.size() == 3);
        CHECK(dst.prepared_buffers[1].vertex_count == 42);
        REQUIRE(dst.strip_color_index.size() == dst.strips.size());
        CHECK(dst.strip_color_index[0] == 3);
        CHECK(dst.strip_color_index[1] == 9);
        CHECK(dst.strip_color(1) == 0xAAAAAA);
        CHECK(dst.strip_layer_index.size() == 2);
        REQUIRE(dst.object_runs.size() == 2);
        CHECK(dst.object_runs[1].object_index == 5);
        REQUIRE(dst.layer_object_runs(1).count == 1);
        CHECK(dst.layer_object_runs(1).first->vertex_offset == 12);
    }
}

// ============================================================================
// Per-object vertex runs (GLES selection silhouette)
// ============================================================================
//
// The 3D shell pass needs to draw one object's triangles out of a VBO that is
// grouped per LAYER and carries no object identity. RibbonGeometry::object_runs is
// that indirection, and it is only correct if every run indexes its own layer's
// VBO exactly. A run that is off by one strip draws part of the neighbouring
// object's silhouette; a run past the end reads past the buffer.

namespace {

/// One extrusion segment, deliberately placed 5mm from the previous segment's end
/// so the builder never shares vertices between them. Sharing changes how many
/// strips a segment emits (the start cap is skipped), which would make the
/// per-object strip proportions below untestable.
ToolpathSegment run_test_segment(int slot, float z, int16_t object_index) {
    ToolpathSegment seg;
    const float x = static_cast<float>(slot % 40) * 4.0f;
    const float y = static_cast<float>(slot / 40) * 4.0f;
    seg.start = glm::vec3(x, y, z);
    seg.end = glm::vec3(x + 1.0f, y, z);
    seg.is_extrusion = true;
    seg.extrusion_amount = 1.0f;
    seg.width = 0.4f;
    seg.object_name_index = object_index;
    return seg;
}

SimplificationOptions no_merge_options() {
    SimplificationOptions opts;
    // Merging is object-aware, but leaving it on makes the strip count per object
    // depend on collinearity rather than on the run bookkeeping under test.
    opts.enable_merging = false;
    return opts;
}

ParsedGCodeFile make_run_fixture(const std::vector<std::vector<int16_t>>& layers_objects,
                                 const std::vector<std::string>& object_names) {
    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(0, 0, 0);
    gcode.global_bounding_box.max = glm::vec3(200, 200, 20);
    gcode.object_name_table = object_names;

    int slot = 0;
    float z = 0.2f;
    for (const auto& objects : layers_objects) {
        Layer layer;
        layer.z_height = z;
        for (int16_t obj : objects) {
            layer.segments.push_back(run_test_segment(slot++, z, obj));
        }
        gcode.layers.push_back(layer);
        gcode.total_segments += objects.size();
        z += 0.2f;
    }
    return gcode;
}

} // namespace

TEST_CASE("Geometry Builder: object runs cover each object's vertices with no gaps or overlap",
          "[gcode][geometry][objectruns]") {
    // Interleaved EXCLUDE_OBJECT blocks within a layer: A A B B A. That must
    // produce three runs per layer, not two, because the trailing A is not
    // adjacent to the leading A in the VBO.
    const std::vector<int16_t> pattern{0, 0, 1, 1, 0};
    ParsedGCodeFile gcode = make_run_fixture({pattern, pattern}, {"objA", "objB"});

    GeometryBuilder builder;
    SimplificationOptions opts = no_merge_options();
    RibbonGeometry geometry = builder.build(gcode, opts);

    REQUIRE(geometry.layer_object_run_ranges.size() == 2);
    REQUIRE_FALSE(geometry.object_runs.empty());

    for (size_t layer = 0; layer < 2; ++layer) {
        auto runs = geometry.layer_object_runs(layer);
        REQUIRE(runs.count == 3);

        const size_t layer_vertices = geometry.layer_strip_ranges[layer].second * 6;
        REQUIRE(layer_vertices > 0);

        // Object identity, in file order.
        CHECK(runs.first[0].object_index == 0);
        CHECK(runs.first[1].object_index == 1);
        CHECK(runs.first[2].object_index == 0);

        // No gaps, no overlap, and the whole layer is covered: every segment in
        // this fixture belongs to an object, so the runs must partition the VBO.
        size_t expected_offset = 0;
        for (const auto& run : runs) {
            CHECK(run.vertex_offset == expected_offset);
            CHECK(run.vertex_count % 6 == 0); // strip-aligned
            // Offsets stay inside THIS layer's VBO — they are not indices into
            // the global vertex array.
            CHECK(static_cast<size_t>(run.vertex_offset) + run.vertex_count <= layer_vertices);
            expected_offset += run.vertex_count;
        }
        CHECK(expected_offset == layer_vertices);

        // Two segments of A, two of B, one of A — identical segments, so the first
        // two runs must be exactly twice the third.
        CHECK(runs.first[0].vertex_count == runs.first[1].vertex_count);
        CHECK(runs.first[0].vertex_count == 2 * runs.first[2].vertex_count);
    }
}

TEST_CASE("Geometry Builder: an object run longer than uint16 splits at a strip boundary",
          "[gcode][geometry][objectruns]") {
    // One object, one layer, enough contiguous segments that its vertex stretch
    // cannot fit in ObjectRun::vertex_count. Truncating instead of splitting would
    // silently drop most of the silhouette.
    std::vector<int16_t> single_object(400, int16_t{0});
    ParsedGCodeFile gcode = make_run_fixture({single_object}, {"objA"});

    GeometryBuilder builder;
    SimplificationOptions opts = no_merge_options();
    RibbonGeometry geometry = builder.build(gcode, opts);

    const size_t layer_vertices = geometry.layer_strip_ranges[0].second * 6;
    REQUIRE(layer_vertices > MAX_RUN_VERTICES);

    auto runs = geometry.layer_object_runs(0);
    REQUIRE(runs.count >= 2);

    size_t expected_offset = 0;
    for (const auto& run : runs) {
        CHECK(run.object_index == 0);
        CHECK(run.vertex_count <= MAX_RUN_VERTICES);
        CHECK(run.vertex_count % 6 == 0);
        CHECK(run.vertex_offset == expected_offset);
        expected_offset += run.vertex_count;
    }
    // Split, not truncated: the pieces still cover the whole stretch.
    CHECK(expected_offset == layer_vertices);
}

TEST_CASE("Geometry Builder: the run-count guard leaves the run table empty",
          "[gcode][geometry][objectruns][slow]") {
    // Alternating objects every segment defeats run coalescing entirely, so the
    // run count tracks the segment count. Past MAX_OBJECT_RUNS the side table
    // stops being cheap relative to the geometry it indexes, and collection is
    // abandoned so the renderer behaves exactly as it did before runs existed.
    std::vector<int16_t> alternating(MAX_OBJECT_RUNS + 64, int16_t{0});
    for (size_t i = 0; i < alternating.size(); ++i) {
        alternating[i] = static_cast<int16_t>(i % 2);
    }
    ParsedGCodeFile gcode = make_run_fixture({alternating}, {"objA", "objB"});

    GeometryBuilder builder;
    builder.set_budget_tube_sides(4); // smallest tube: keeps this fixture's mesh affordable
    SimplificationOptions opts = no_merge_options();
    RibbonGeometry geometry = builder.build(gcode, opts);

    // The geometry itself is still built and still renders.
    REQUIRE_FALSE(geometry.vertices.empty());
    REQUIRE_FALSE(geometry.strips.empty());

    // But no runs at all — not a truncated table, which would silhouette an
    // arbitrary prefix of the plate.
    CHECK(geometry.object_runs.empty());
    CHECK(geometry.layer_object_run_ranges.empty());
    CHECK(geometry.layer_object_runs(0).empty());
}

TEST_CASE("Geometry Builder: a file with no exclude-object metadata allocates no run table",
          "[gcode][geometry][objectruns]") {
    // Most files have no EXCLUDE_OBJECT_DEFINE at all. They must not pay a byte
    // for the silhouette they can never show.
    std::vector<int16_t> unowned(8, int16_t{-1});
    ParsedGCodeFile gcode = make_run_fixture({unowned, unowned}, {});
    REQUIRE(gcode.object_name_table.empty());

    GeometryBuilder builder;
    SimplificationOptions opts = no_merge_options();
    RibbonGeometry geometry = builder.build(gcode, opts);

    REQUIRE_FALSE(geometry.vertices.empty());
    CHECK(geometry.object_runs.empty());
    CHECK(geometry.object_runs.capacity() == 0);
    CHECK(geometry.layer_object_run_ranges.empty());
    CHECK(geometry.layer_object_run_ranges.capacity() == 0);
    CHECK(geometry.layer_object_runs(0).empty());
}

TEST_CASE("Geometry Builder: segments outside any object do not join a run",
          "[gcode][geometry][objectruns]") {
    // Purge lines, prime blobs and wipe towers carry object_name_index == -1.
    // Folding them into the neighbouring run would paint them white along with
    // the selected object.
    ParsedGCodeFile gcode = make_run_fixture({{0, -1, 0}}, {"objA"});

    GeometryBuilder builder;
    SimplificationOptions opts = no_merge_options();
    RibbonGeometry geometry = builder.build(gcode, opts);

    auto runs = geometry.layer_object_runs(0);
    REQUIRE(runs.count == 2);
    CHECK(runs.first[0].object_index == 0);
    CHECK(runs.first[1].object_index == 0);
    // A gap where the unowned segment sits — the second run does not start where
    // the first one ended.
    CHECK(runs.first[1].vertex_offset >
          static_cast<uint32_t>(runs.first[0].vertex_offset + runs.first[0].vertex_count));
    const size_t layer_vertices = geometry.layer_strip_ranges[0].second * 6;
    CHECK(static_cast<size_t>(runs.first[1].vertex_offset) + runs.first[1].vertex_count <=
          layer_vertices);
}

// ===========================================================================
// AABB helpers + the tube-expansion constants (DRY-6)
// ===========================================================================

TEST_CASE("default_plate_bbox is the 200x200 fallback for an empty box", "[gcode][aabb]") {
    // Callers substitute this when a box is empty, because the +/-inf sentinels
    // of an empty AABB turn into NaN offsets in compute_auto_fit() and every
    // projected point becomes garbage.
    const auto bb = helix::gcode::AABB::default_plate_bbox();
    CHECK_FALSE(bb.is_empty());
    CHECK(bb.min == glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(bb.max == glm::vec3(200.0f, 200.0f, 0.0f));
}

TEST_CASE("expand_by grows both directions on every axis", "[gcode][aabb]") {
    helix::gcode::AABB bb;
    bb.min = glm::vec3(10.0f, 20.0f, 30.0f);
    bb.max = glm::vec3(11.0f, 21.0f, 31.0f);
    bb.expand_by(2.0f);
    CHECK(bb.min == glm::vec3(8.0f, 18.0f, 28.0f));
    CHECK(bb.max == glm::vec3(13.0f, 23.0f, 33.0f));
}

TEST_CASE("tube_half_diagonal is the exact worst-case reach off the centre line",
          "[gcode][geometry_builder]") {
    // A 0.4mm tube reaches 0.2mm off the centre line, and on a diagonal that
    // lands in two axes at once: 0.2 * sqrt(2).
    CHECK(helix::gcode::tube_half_diagonal(0.4f) == Catch::Approx(0.4f * 0.5f * 1.41421356f));
    CHECK(helix::gcode::tube_half_diagonal(0.0f) == Catch::Approx(0.0f));
}

TEST_CASE("quantization slack preserves the historical 1.5x margin", "[gcode][geometry_builder]") {
    // The bounds used to be expanded by max_tube_width * 1.5f under a comment
    // claiming sqrt(2). Naming the geometry must not quietly re-tune the number
    // that has been protecting every build: the product still has to be 1.5x.
    const float width = 0.4f;
    const float margin = helix::gcode::tube_half_diagonal(width) * helix::gcode::kQuantBoundsSlack;
    CHECK(margin == Catch::Approx(width * 1.5f).epsilon(1e-5));
    // And it must stay strictly larger than the exact reach, or the bounds stop
    // being conservative and vertices can quantize out of range.
    CHECK(margin > helix::gcode::tube_half_diagonal(width));
}
