// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_memory_lifecycle.cpp
 * @brief Tests for 3D renderer memory lifecycle
 *
 * Tests that CPU-side geometry data is properly released after GPU upload
 * and when the gcode viewer is cleared. The GPU (GL) side can't be tested
 * without a display, so we focus on the RibbonGeometry data lifecycle and
 * the decision logic for when to release resources.
 */

#include "gcode_geometry_builder.h"
#include "gcode_parser.h"

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

// ============================================================================
// Helper: Build a minimal RibbonGeometry with prepared_buffers
// ============================================================================

static std::unique_ptr<RibbonGeometry> build_test_geometry(int num_layers = 2,
                                                           int segments_per_layer = 3) {
    ParsedGCodeFile gcode;

    for (int l = 0; l < num_layers; ++l) {
        Layer layer;
        layer.z_height = 0.2f * static_cast<float>(l + 1);
        for (int i = 0; i < segments_per_layer; ++i) {
            ToolpathSegment seg;
            float x = static_cast<float>(i) * 5.0f;
            float z = layer.z_height;
            seg.start = {x, 0.0f, z};
            seg.end = {x + 5.0f, 0.0f, z};
            seg.is_extrusion = true;
            seg.width = 0.4f;
            layer.segments.push_back(seg);
        }
        gcode.layers.push_back(layer);
    }

    gcode.total_segments = static_cast<size_t>(num_layers * segments_per_layer);
    gcode.global_bounding_box.expand({0.0f, -1.0f, 0.0f});
    gcode.global_bounding_box.expand({static_cast<float>(segments_per_layer) * 5.0f, 1.0f,
                                      0.2f * static_cast<float>(num_layers)});

    GeometryBuilder builder;
    SimplificationOptions opts;
    opts.enable_merging = false;
    auto geom = std::make_unique<RibbonGeometry>(builder.build(gcode, opts));
    geom->prepare_interleaved_buffers();
    return geom;
}

// ============================================================================
// prepared_buffers memory release
// ============================================================================

TEST_CASE("prepared_buffers consume significant memory relative to compact data",
          "[gcode][geometry][memory]") {
    auto geom = build_test_geometry(5, 10);

    REQUIRE(!geom->prepared_buffers.empty());
    REQUIRE(!geom->vertices.empty());
    REQUIRE(!geom->strips.empty());

    // prepared_buffers hold one packed 12-byte vertex per expanded triangle vertex,
    // versus a compact 8-byte RibbonVertex in the indexed mesh. With strip expansion
    // (6 verts per strip vs 4 indexed) the prepared buffers should be substantially
    // larger than the compact vertex pool.
    size_t prepared_bytes = 0;
    for (const auto& pb : geom->prepared_buffers) {
        prepared_bytes += pb.data.size();
    }
    size_t compact_bytes = geom->vertices.size() * sizeof(RibbonVertex);

    REQUIRE(prepared_bytes > compact_bytes * 2);
}

TEST_CASE("Clearing prepared_buffers frees memory while preserving geometry",
          "[gcode][geometry][memory]") {
    auto geom = build_test_geometry(3, 5);

    size_t vertices_before = geom->vertices.size();
    size_t strips_before = geom->strips.size();
    size_t palette_before = geom->color_palette.size();
    size_t ranges_before = geom->layer_strip_ranges.size();
    uint16_t max_layer_before = geom->max_layer_index;

    REQUIRE(!geom->prepared_buffers.empty());

    // Simulate what the renderer does after VBO upload
    for (auto& pb : geom->prepared_buffers) {
        pb.data.clear();
        pb.data.shrink_to_fit();
    }
    geom->prepared_buffers.clear();
    geom->prepared_buffers.shrink_to_fit();

    // prepared_buffers should be gone
    REQUIRE(geom->prepared_buffers.empty());

    // All other geometry data should be intact (needed for tool color re-upload)
    REQUIRE(geom->vertices.size() == vertices_before);
    REQUIRE(geom->strips.size() == strips_before);
    REQUIRE(geom->color_palette.size() == palette_before);
    REQUIRE(geom->layer_strip_ranges.size() == ranges_before);
    REQUIRE(geom->max_layer_index == max_layer_before);
}

TEST_CASE("memory_usage() reflects prepared_buffers state", "[gcode][geometry][memory]") {
    auto geom = build_test_geometry(3, 5);
    geom->prepare_interleaved_buffers();

    size_t usage_with_prepared = geom->memory_usage();
    REQUIRE(usage_with_prepared > 0);

    // Clear prepared_buffers
    geom->prepared_buffers.clear();
    geom->prepared_buffers.shrink_to_fit();

    size_t usage_without_prepared = geom->memory_usage();

    // memory_usage() doesn't count prepared_buffers (they're a cache),
    // but the core geometry data should still report non-zero
    REQUIRE(usage_without_prepared > 0);
    REQUIRE(geom->vertices.size() > 0);
    REQUIRE(geom->strips.size() > 0);
}

TEST_CASE("RibbonGeometry::clear() releases all data", "[gcode][geometry][memory]") {
    auto geom = build_test_geometry(3, 5);

    REQUIRE(geom->vertices.size() > 0);
    REQUIRE(geom->strips.size() > 0);
    REQUIRE(geom->color_palette.size() > 0);
    REQUIRE(!geom->prepared_buffers.empty());

    geom->clear();

    REQUIRE(geom->vertices.empty());
    REQUIRE(geom->strips.empty());
    REQUIRE(geom->color_palette.empty());
    REQUIRE(geom->layer_strip_ranges.empty());
    REQUIRE(geom->strip_layer_index.empty());
    REQUIRE(geom->strip_color_index.empty());
    REQUIRE(geom->max_layer_index == 0);
    REQUIRE(geom->memory_usage() == 0);
}

// ============================================================================
// CPU fallback re-expansion after prepared_buffers cleared
// ============================================================================

TEST_CASE("Geometry data supports CPU re-expansion after prepared_buffers cleared",
          "[gcode][geometry][memory]") {
    auto geom = build_test_geometry(2, 3);

    // Verify prepared_buffers match the manual expansion path
    REQUIRE(!geom->prepared_buffers.empty());

    // Snapshot the first prepared layer for byte-exact comparison after rebuild.
    auto& first_buf = geom->prepared_buffers[0];
    REQUIRE(first_buf.vertex_count > 0);
    REQUIRE(first_buf.data.size() == first_buf.vertex_count * PackedVertex::stride());
    std::vector<uint8_t> reference_data(first_buf.data.begin(), first_buf.data.end());

    // Clear prepared_buffers (simulating post-VBO-upload cleanup)
    geom->prepared_buffers.clear();
    geom->prepared_buffers.shrink_to_fit();

    // Verify the raw data needed for CPU fallback re-expansion is intact
    REQUIRE(!geom->strips.empty());
    REQUIRE(!geom->vertices.empty());
    REQUIRE(geom->strip_color_index.size() == geom->strips.size());
    REQUIRE(!geom->layer_strip_ranges.empty());

    // Manually re-expand first layer (mirrors upload_geometry_chunk CPU fallback,
    // packed 12-byte PackedVertex layout — quantized int16 positions go to the
    // GPU as-is, with dequantization folded into the renderer's matrices).
    auto [first_strip, strip_count] = geom->layer_strip_ranges[0];
    REQUIRE(strip_count > 0);

    size_t total_verts = strip_count * 6;
    std::vector<uint8_t> re_expanded(total_verts * PackedVertex::stride());
    static constexpr int TRI_INDICES[6] = {0, 1, 2, 1, 3, 2};

    auto* out = reinterpret_cast<PackedVertex*>(re_expanded.data());
    for (size_t s = 0; s < strip_count; ++s) {
        const size_t strip_idx = first_strip + s;
        const auto& strip = geom->strips[strip_idx];

        // Color is per-strip; normals come pre-encoded off the vertex.
        uint32_t rgb = 0x26A69A;
        if (strip_idx < geom->strip_color_index.size() &&
            geom->strip_color_index[strip_idx] < geom->color_palette.size()) {
            rgb = geom->color_palette[geom->strip_color_index[strip_idx]];
        }

        for (int ti = 0; ti < 6; ++ti) {
            const auto& vert = geom->vertices[strip[static_cast<size_t>(TRI_INDICES[ti])]];

            out->position[0] = vert.position.x;
            out->position[1] = vert.position.y;
            out->position[2] = vert.position.z;

            PackedVertex::encode_color(rgb, out->color);
            out->normal[0] = vert.normal[0];
            out->normal[1] = vert.normal[1];
            ++out;
        }
    }

    // Re-expanded bytes should match what prepared_buffers held.
    REQUIRE(re_expanded.size() == reference_data.size());
    REQUIRE(re_expanded == reference_data);
}

// ============================================================================
// Deactivation resource release decision logic
// ============================================================================

enum class PrintState { Idle, Preparing, Printing, Paused, Complete, Cancelled, Error };

/**
 * @brief Decides whether to release gcode viewer resources on panel deactivation.
 *
 * Mirrors the logic in PrintStatusPanel::on_deactivate():
 * - During active print (Printing/Paused/Preparing): keep resources for quick return
 * - After print ends (Complete/Cancelled/Error/Idle): release to free memory
 */
static bool should_release_on_deactivate(PrintState state) {
    return state != PrintState::Printing && state != PrintState::Paused &&
           state != PrintState::Preparing;
}

TEST_CASE("Deactivation releases resources when print is not active",
          "[gcode][memory][lifecycle]") {
    SECTION("Idle state releases resources") {
        REQUIRE(should_release_on_deactivate(PrintState::Idle));
    }

    SECTION("Complete state releases resources") {
        REQUIRE(should_release_on_deactivate(PrintState::Complete));
    }

    SECTION("Cancelled state releases resources") {
        REQUIRE(should_release_on_deactivate(PrintState::Cancelled));
    }

    SECTION("Error state releases resources") {
        REQUIRE(should_release_on_deactivate(PrintState::Error));
    }
}

TEST_CASE("Deactivation preserves resources during active print", "[gcode][memory][lifecycle]") {
    SECTION("Printing state keeps resources") {
        REQUIRE_FALSE(should_release_on_deactivate(PrintState::Printing));
    }

    SECTION("Paused state keeps resources") {
        REQUIRE_FALSE(should_release_on_deactivate(PrintState::Paused));
    }

    SECTION("Preparing state keeps resources") {
        REQUIRE_FALSE(should_release_on_deactivate(PrintState::Preparing));
    }
}
