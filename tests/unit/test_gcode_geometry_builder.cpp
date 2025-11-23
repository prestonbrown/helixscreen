/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "../catch_amalgamated.hpp"
#include "gcode_geometry_builder.h"
#include "gcode_parser.h"

using namespace gcode;
using Catch::Approx;

// ============================================================================
// QuantizationParams Tests
// ============================================================================

TEST_CASE("Geometry Builder: QuantizationParams - calculate scale from bounding box", "[gcode][geometry][quantization]") {
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

TEST_CASE("Geometry Builder: QuantizationParams - quantize and dequantize round trip", "[gcode][geometry][quantization]") {
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

TEST_CASE("Geometry Builder: QuantizationParams - degenerate bounding box", "[gcode][geometry][quantization][edge]") {
    QuantizationParams params;
    AABB bbox;
    bbox.min = glm::vec3(0.0f, 0.0f, 0.0f);
    bbox.max = glm::vec3(0.0f, 0.0f, 0.0f);  // Zero-size box

    params.calculate_scale(bbox);

    // Should fall back to default scale factor
    REQUIRE(params.scale_factor == Approx(1000.0f));
}

TEST_CASE("Geometry Builder: QuantizationParams - large build volume", "[gcode][geometry][quantization]") {
    QuantizationParams params;
    AABB bbox;
    bbox.min = glm::vec3(-150.0f, -150.0f, 0.0f);
    bbox.max = glm::vec3(150.0f, 150.0f, 300.0f);  // 300x300x300mm
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

TEST_CASE("Geometry Builder: SimplificationOptions - validate clamps values", "[gcode][geometry][simplification]") {
    SimplificationOptions options;

    SECTION("Tolerance too small") {
        options.tolerance_mm = 0.001f;
        options.validate();
        REQUIRE(options.tolerance_mm == Approx(0.01f));  // Clamped to min
    }

    SECTION("Tolerance too large") {
        options.tolerance_mm = 1.0f;
        options.validate();
        REQUIRE(options.tolerance_mm == Approx(0.2f));  // Clamped to max
    }

    SECTION("Valid tolerance") {
        options.tolerance_mm = 0.15f;
        options.validate();
        REQUIRE(options.tolerance_mm == Approx(0.15f));  // Unchanged
    }

    SECTION("Min segment length too small") {
        options.min_segment_length_mm = 0.00001f;
        options.validate();
        REQUIRE(options.min_segment_length_mm == Approx(0.0001f));  // Clamped to min
    }
}

// ============================================================================
// RibbonGeometry Tests
// ============================================================================

TEST_CASE("Geometry Builder: RibbonGeometry - construction and destruction", "[gcode][geometry][ribbon]") {
    RibbonGeometry geometry;

    REQUIRE(geometry.vertices.empty());
    REQUIRE(geometry.indices.empty());
    REQUIRE(geometry.strips.empty());
    REQUIRE(geometry.normal_palette.empty());
    REQUIRE(geometry.color_palette.empty());
    REQUIRE(geometry.normal_cache_ptr != nullptr);
    REQUIRE(geometry.color_cache_ptr != nullptr);
}

TEST_CASE("Geometry Builder: RibbonGeometry - move semantics", "[gcode][geometry][ribbon]") {
    RibbonGeometry geom1;
    geom1.vertices.push_back({{100, 200, 300}, 0, 0});
    geom1.extrusion_triangle_count = 42;

    RibbonGeometry geom2(std::move(geom1));

    REQUIRE(geom2.vertices.size() == 1);
    REQUIRE(geom2.extrusion_triangle_count == 42);
    REQUIRE(geom2.normal_cache_ptr != nullptr);
}

TEST_CASE("Geometry Builder: RibbonGeometry - clear", "[gcode][geometry][ribbon]") {
    RibbonGeometry geometry;
    geometry.vertices.push_back({{100, 200, 300}, 0, 0});
    geometry.normal_palette.push_back(glm::vec3(0, 0, 1));
    geometry.color_palette.push_back(0xFF0000);
    geometry.extrusion_triangle_count = 10;

    geometry.clear();

    REQUIRE(geometry.vertices.empty());
    REQUIRE(geometry.normal_palette.empty());
    REQUIRE(geometry.color_palette.empty());
    REQUIRE(geometry.extrusion_triangle_count == 0);
}

TEST_CASE("Geometry Builder: RibbonGeometry - memory usage", "[gcode][geometry][ribbon]") {
    RibbonGeometry geometry;

    size_t empty_memory = geometry.memory_usage();
    REQUIRE(empty_memory == 0);

    // Add some data
    geometry.vertices.push_back({{100, 200, 300}, 0, 0});
    geometry.strips.push_back({0, 1, 2, 3});
    geometry.normal_palette.push_back(glm::vec3(0, 0, 1));
    geometry.color_palette.push_back(0xFF0000);

    size_t used_memory = geometry.memory_usage();
    REQUIRE(used_memory > empty_memory);
}

// ============================================================================
// GeometryBuilder - Color Tests
// ============================================================================

TEST_CASE("Geometry Builder: Color computation - hex parsing", "[gcode][geometry][color]") {
    GeometryBuilder builder;

    SECTION("Parse with # prefix") {
        builder.set_filament_color("#26A69A");
        // Internal state should be updated (can't directly test private members)
    }

    SECTION("Parse without # prefix") {
        builder.set_filament_color("FF0000");
        // Internal state should be updated
    }

    SECTION("Invalid color string") {
        builder.set_filament_color("XYZ");  // Should not crash
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

TEST_CASE("Geometry Builder: Color computation - solid filament color", "[gcode][geometry][color]") {
    GeometryBuilder builder;
    builder.set_filament_color("#ED1C24");  // Red
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

TEST_CASE("Geometry Builder: Segment simplification - collinear merging", "[gcode][geometry][simplification]") {
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
    REQUIRE(stats.output_segments < stats.input_segments);  // Should have merged
}

TEST_CASE("Geometry Builder: Segment simplification - non-collinear preservation", "[gcode][geometry][simplification]") {
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
    seg2.end = glm::vec3(10, 10, 0.2f);  // 90 degree turn
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
    REQUIRE(stats.output_segments == 2);  // Should NOT merge
}

TEST_CASE("Geometry Builder: Segment simplification - disabled", "[gcode][geometry][simplification]") {
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
    options.enable_merging = false;  // DISABLED

    RibbonGeometry geometry = builder.build(gcode, options);

    const auto& stats = builder.last_stats();
    REQUIRE(stats.input_segments == 5);
    REQUIRE(stats.output_segments == 5);  // No simplification
    REQUIRE(stats.simplification_ratio == Approx(0.0f));
}

// ============================================================================
// GeometryBuilder - Geometry Generation Tests
// ============================================================================

TEST_CASE("Geometry Builder: Geometry generation - single segment", "[gcode][geometry][generation]") {
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
    REQUIRE(geometry.normal_palette.size() > 0);
    REQUIRE(geometry.color_palette.size() > 0);
}

TEST_CASE("Geometry Builder: Geometry generation - empty G-code", "[gcode][geometry][generation][edge]") {
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

TEST_CASE("Geometry Builder: Geometry generation - travel moves skipped", "[gcode][geometry][generation]") {
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
    travel.is_extrusion = false;  // Travel move
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

TEST_CASE("Geometry Builder: Geometry generation - multiple layers", "[gcode][geometry][generation]") {
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

TEST_CASE("Geometry Builder: Geometry generation - very short segment", "[gcode][geometry][generation][edge]") {
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
    seg.width = 0.0f;  // Should use configured width
    layer.segments.push_back(seg);
    gcode.layers.push_back(layer);

    SimplificationOptions options;
    RibbonGeometry geometry = builder.build(gcode, options);

    REQUIRE(geometry.vertices.size() > 0);
}

TEST_CASE("Geometry Builder: Configuration - layer height", "[gcode][geometry][config]") {
    GeometryBuilder builder;
    builder.set_layer_height(0.3f);  // Non-default

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

TEST_CASE("Geometry Builder: Real-world - calibration cube perimeter", "[gcode][geometry][realworld]") {
    GeometryBuilder builder;

    ParsedGCodeFile gcode;
    gcode.global_bounding_box.min = glm::vec3(90, 90, 0);
    gcode.global_bounding_box.max = glm::vec3(110, 110, 20);

    Layer layer;
    layer.z_height = 0.2f;

    // Square perimeter (20mm cube)
    std::vector<glm::vec3> points = {
        glm::vec3(95, 95, 0.2f),
        glm::vec3(105, 95, 0.2f),
        glm::vec3(105, 105, 0.2f),
        glm::vec3(95, 105, 0.2f),
        glm::vec3(95, 95, 0.2f)  // Close loop
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
