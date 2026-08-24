// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_object_thumbnail_renderer.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using Catch::Approx;

// ============================================================================
// TEST HELPERS
// ============================================================================

namespace {

/// Create a minimal ParsedGCodeFile with no objects
ParsedGCodeFile make_empty_gcode() {
    ParsedGCodeFile gcode;
    return gcode;
}

/// Create a gcode with one object that has a single horizontal line
ParsedGCodeFile make_single_object_gcode(const std::string& name = "cube1") {
    ParsedGCodeFile gcode;

    // Define the object
    GCodeObject obj;
    obj.name = name;
    obj.bounding_box.expand(glm::vec3(10.0f, 20.0f, 0.2f));
    obj.bounding_box.expand(glm::vec3(90.0f, 80.0f, 0.2f));
    gcode.objects[name] = obj;

    // Create a layer with segments belonging to the object
    Layer layer;
    layer.z_height = 0.2f;

    ToolpathSegment seg;
    seg.start = glm::vec3(10.0f, 50.0f, 0.2f);
    seg.end = glm::vec3(90.0f, 50.0f, 0.2f);
    seg.is_extrusion = true;
    seg.object_name_index = gcode.intern_object_name(name);
    layer.segments.push_back(seg);

    // Another segment: vertical line
    ToolpathSegment seg2;
    seg2.start = glm::vec3(50.0f, 20.0f, 0.2f);
    seg2.end = glm::vec3(50.0f, 80.0f, 0.2f);
    seg2.is_extrusion = true;
    seg2.object_name_index = gcode.intern_object_name(name);
    layer.segments.push_back(seg2);

    layer.bounding_box.expand(glm::vec3(10.0f, 20.0f, 0.2f));
    layer.bounding_box.expand(glm::vec3(90.0f, 80.0f, 0.2f));
    layer.segment_count_extrusion = 2;

    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 2;
    gcode.global_bounding_box.expand(glm::vec3(10.0f, 20.0f, 0.2f));
    gcode.global_bounding_box.expand(glm::vec3(90.0f, 80.0f, 0.2f));

    return gcode;
}

/// Create a gcode with multiple distinct objects
ParsedGCodeFile make_multi_object_gcode() {
    ParsedGCodeFile gcode;

    // Object A: left side of bed
    GCodeObject obj_a;
    obj_a.name = "part_A";
    obj_a.bounding_box.expand(glm::vec3(10.0f, 10.0f, 0.2f));
    obj_a.bounding_box.expand(glm::vec3(40.0f, 40.0f, 0.2f));
    gcode.objects["part_A"] = obj_a;

    // Object B: right side of bed
    GCodeObject obj_b;
    obj_b.name = "part_B";
    obj_b.bounding_box.expand(glm::vec3(60.0f, 60.0f, 0.2f));
    obj_b.bounding_box.expand(glm::vec3(90.0f, 90.0f, 0.2f));
    gcode.objects["part_B"] = obj_b;

    // Layer with segments for both objects
    Layer layer;
    layer.z_height = 0.2f;

    // Part A segments (square-ish)
    auto add_seg = [&](const std::string& obj_name, float x0, float y0, float x1, float y1) {
        ToolpathSegment seg;
        seg.start = glm::vec3(x0, y0, 0.2f);
        seg.end = glm::vec3(x1, y1, 0.2f);
        seg.is_extrusion = true;
        seg.object_name_index = gcode.intern_object_name(obj_name);
        layer.segments.push_back(seg);
    };

    add_seg("part_A", 10.0f, 10.0f, 40.0f, 10.0f);
    add_seg("part_A", 40.0f, 10.0f, 40.0f, 40.0f);
    add_seg("part_A", 40.0f, 40.0f, 10.0f, 40.0f);
    add_seg("part_A", 10.0f, 40.0f, 10.0f, 10.0f);

    add_seg("part_B", 60.0f, 60.0f, 90.0f, 60.0f);
    add_seg("part_B", 90.0f, 60.0f, 90.0f, 90.0f);
    add_seg("part_B", 90.0f, 90.0f, 60.0f, 90.0f);
    add_seg("part_B", 60.0f, 90.0f, 60.0f, 60.0f);

    layer.bounding_box.expand(glm::vec3(10.0f, 10.0f, 0.2f));
    layer.bounding_box.expand(glm::vec3(90.0f, 90.0f, 0.2f));
    layer.segment_count_extrusion = 8;

    gcode.layers.push_back(std::move(layer));
    gcode.total_segments = 8;
    gcode.global_bounding_box.expand(glm::vec3(10.0f, 10.0f, 0.2f));
    gcode.global_bounding_box.expand(glm::vec3(90.0f, 90.0f, 0.2f));

    return gcode;
}

/// Count non-zero (non-transparent) pixels in a thumbnail
size_t count_drawn_pixels(const ObjectThumbnail& thumb) {
    if (!thumb.is_valid())
        return 0;

    size_t count = 0;
    for (int y = 0; y < thumb.height; ++y) {
        for (int x = 0; x < thumb.width; ++x) {
            const uint8_t* pixel = thumb.pixels.get() + y * thumb.stride + x * 4;
            // Check alpha channel (byte 3 in BGRA order)
            if (pixel[3] > 0) {
                ++count;
            }
        }
    }
    return count;
}

/// Check if a specific pixel has been drawn (non-transparent)
bool pixel_drawn_at(const ObjectThumbnail& thumb, int x, int y) {
    if (!thumb.is_valid() || x < 0 || x >= thumb.width || y < 0 || y >= thumb.height) {
        return false;
    }
    const uint8_t* pixel = thumb.pixels.get() + y * thumb.stride + x * 4;
    return pixel[3] > 0;
}

/// Test color: opaque teal (ARGB)
constexpr uint32_t TEST_COLOR = 0xFF26A69A;

} // namespace

// ============================================================================
// BASIC FUNCTIONALITY
// ============================================================================

TEST_CASE("GCodeObjectThumbnailRenderer: empty gcode produces empty set", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_empty_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result != nullptr);
    REQUIRE(result->thumbnails.empty());
}

TEST_CASE("GCodeObjectThumbnailRenderer: null gcode produces empty set", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;

    auto result = renderer.render_sync(nullptr, 40, 40, TEST_COLOR);

    REQUIRE(result != nullptr);
    REQUIRE(result->thumbnails.empty());
}

TEST_CASE("GCodeObjectThumbnailRenderer: single object produces one thumbnail",
          "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_single_object_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result != nullptr);
    REQUIRE(result->thumbnails.size() == 1);

    const auto& thumb = result->thumbnails[0];
    REQUIRE(thumb.object_name == "cube1");
    REQUIRE(thumb.width == 40);
    REQUIRE(thumb.height == 40);
    REQUIRE(thumb.stride == 160); // 40 * 4
    REQUIRE(thumb.is_valid());
}

TEST_CASE("GCodeObjectThumbnailRenderer: thumbnail has drawn pixels", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_single_object_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result != nullptr);
    REQUIRE(result->thumbnails.size() == 1);

    size_t drawn = count_drawn_pixels(result->thumbnails[0]);
    REQUIRE(drawn > 0);

    // A cross pattern in a 40x40 thumbnail should have a reasonable number of pixels
    // At minimum we expect both lines to have some pixels drawn
    REQUIRE(drawn >= 10);
}

TEST_CASE("GCodeObjectThumbnailRenderer: multiple objects produce multiple thumbnails",
          "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_multi_object_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result != nullptr);
    REQUIRE(result->thumbnails.size() == 2);

    // Both thumbnails should have pixels drawn
    const auto* thumb_a = result->find("part_A");
    const auto* thumb_b = result->find("part_B");

    REQUIRE(thumb_a != nullptr);
    REQUIRE(thumb_b != nullptr);
    REQUIRE(count_drawn_pixels(*thumb_a) > 0);
    REQUIRE(count_drawn_pixels(*thumb_b) > 0);
}

TEST_CASE("GCodeObjectThumbnailRenderer: find by name works", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_multi_object_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result->find("part_A") != nullptr);
    REQUIRE(result->find("part_B") != nullptr);
    REQUIRE(result->find("nonexistent") == nullptr);
}

// ============================================================================
// THUMBNAIL SIZING
// ============================================================================

TEST_CASE("GCodeObjectThumbnailRenderer: custom thumbnail size", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_single_object_gcode();

    SECTION("64x64 thumbnails") {
        auto result = renderer.render_sync(&gcode, 64, 64, TEST_COLOR);
        REQUIRE(result->thumbnails[0].width == 64);
        REQUIRE(result->thumbnails[0].height == 64);
        REQUIRE(result->thumbnails[0].stride == 256);
    }

    SECTION("20x20 thumbnails") {
        auto result = renderer.render_sync(&gcode, 20, 20, TEST_COLOR);
        REQUIRE(result->thumbnails[0].width == 20);
        REQUIRE(result->thumbnails[0].height == 20);
    }

    SECTION("non-square thumbnails") {
        auto result = renderer.render_sync(&gcode, 60, 40, TEST_COLOR);
        REQUIRE(result->thumbnails[0].width == 60);
        REQUIRE(result->thumbnails[0].height == 40);
    }
}

// ============================================================================
// EDGE CASES
// ============================================================================

TEST_CASE("GCodeObjectThumbnailRenderer: object with no segments gets empty thumbnail",
          "[object-thumbnail]") {
    ParsedGCodeFile gcode;

    // Define object but don't add any segments for it
    GCodeObject obj;
    obj.name = "empty_obj";
    obj.bounding_box.expand(glm::vec3(10.0f, 10.0f, 0.2f));
    obj.bounding_box.expand(glm::vec3(50.0f, 50.0f, 0.2f));
    gcode.objects["empty_obj"] = obj;

    // Add an empty layer
    Layer layer;
    layer.z_height = 0.2f;
    gcode.layers.push_back(std::move(layer));

    GCodeObjectThumbnailRenderer renderer;
    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result->thumbnails.size() == 1);
    REQUIRE(result->thumbnails[0].object_name == "empty_obj");
    // Thumbnail exists but has no drawn pixels
    REQUIRE(count_drawn_pixels(result->thumbnails[0]) == 0);
}

TEST_CASE("GCodeObjectThumbnailRenderer: segments without object_name are skipped",
          "[object-thumbnail]") {
    ParsedGCodeFile gcode;

    GCodeObject obj;
    obj.name = "my_obj";
    obj.bounding_box.expand(glm::vec3(0.0f, 0.0f, 0.2f));
    obj.bounding_box.expand(glm::vec3(100.0f, 100.0f, 0.2f));
    gcode.objects["my_obj"] = obj;

    Layer layer;
    layer.z_height = 0.2f;

    // Unnamed segment (e.g., skirt/brim)
    ToolpathSegment unnamed;
    unnamed.start = glm::vec3(0.0f, 0.0f, 0.2f);
    unnamed.end = glm::vec3(100.0f, 100.0f, 0.2f);
    unnamed.is_extrusion = true;
    // object_name is empty
    layer.segments.push_back(unnamed);

    // Named segment
    ToolpathSegment named;
    named.start = glm::vec3(20.0f, 20.0f, 0.2f);
    named.end = glm::vec3(80.0f, 80.0f, 0.2f);
    named.is_extrusion = true;
    named.object_name_index = gcode.intern_object_name("my_obj");
    layer.segments.push_back(named);

    gcode.layers.push_back(std::move(layer));

    GCodeObjectThumbnailRenderer renderer;
    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result->thumbnails.size() == 1);
    // Only the named segment should have been drawn
    REQUIRE(count_drawn_pixels(result->thumbnails[0]) > 0);
}

TEST_CASE("GCodeObjectThumbnailRenderer: travel moves are skipped", "[object-thumbnail]") {
    ParsedGCodeFile gcode;

    GCodeObject obj;
    obj.name = "obj";
    obj.bounding_box.expand(glm::vec3(0.0f, 0.0f, 0.2f));
    obj.bounding_box.expand(glm::vec3(100.0f, 100.0f, 0.2f));
    gcode.objects["obj"] = obj;

    Layer layer;
    layer.z_height = 0.2f;

    // Only travel moves for this object
    ToolpathSegment travel;
    travel.start = glm::vec3(10.0f, 10.0f, 0.2f);
    travel.end = glm::vec3(90.0f, 90.0f, 0.2f);
    travel.is_extrusion = false; // travel!
    travel.object_name_index = gcode.intern_object_name("obj");
    layer.segments.push_back(travel);

    gcode.layers.push_back(std::move(layer));

    GCodeObjectThumbnailRenderer renderer;
    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result->thumbnails.size() == 1);
    // Travel moves should be skipped
    REQUIRE(count_drawn_pixels(result->thumbnails[0]) == 0);
}

TEST_CASE("GCodeObjectThumbnailRenderer: object with empty bounding box is skipped",
          "[object-thumbnail]") {
    ParsedGCodeFile gcode;

    // Object with default (empty/degenerate) bounding box
    GCodeObject obj;
    obj.name = "degenerate";
    // Don't expand bounding box - it stays at infinity/-infinity
    gcode.objects["degenerate"] = obj;

    GCodeObjectThumbnailRenderer renderer;
    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    // Object with empty bbox should be skipped entirely
    REQUIRE(result->thumbnails.empty());
}

TEST_CASE("GCodeObjectThumbnailRenderer: correct color in pixels", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_single_object_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);
    REQUIRE(result->thumbnails.size() == 1);

    const auto& thumb = result->thumbnails[0];

    // Find a drawn pixel and verify its color
    // The renderer applies depth shading, so RGB channels will be darkened
    // from the input color. Alpha is preserved. Verify proportions are correct.
    bool found_pixel = false;
    for (int y = 0; y < thumb.height && !found_pixel; ++y) {
        for (int x = 0; x < thumb.width && !found_pixel; ++x) {
            const uint8_t* pixel = thumb.pixels.get() + y * thumb.stride + x * 4;
            if (pixel[3] > 0) {
                // TEST_COLOR = 0xFF26A69A → A=0xFF, R=0x26, G=0xA6, B=0x9A
                // BGRA byte order; depth shading darkens RGB but preserves alpha
                REQUIRE(pixel[0] > 0);     // B: shaded but non-zero
                REQUIRE(pixel[0] <= 0x9A); // B: no brighter than input
                REQUIRE(pixel[1] > 0);     // G: shaded but non-zero
                REQUIRE(pixel[1] <= 0xA6); // G: no brighter than input
                REQUIRE(pixel[2] > 0);     // R: shaded but non-zero
                REQUIRE(pixel[2] <= 0x26); // R: no brighter than input
                REQUIRE(pixel[3] == 0xFF); // A: preserved exactly
                found_pixel = true;
            }
        }
    }
    REQUIRE(found_pixel);
}

// ============================================================================
// CANCELLATION
// ============================================================================

TEST_CASE("GCodeObjectThumbnailRenderer: cancellation doesn't crash", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_multi_object_gcode();

    // Render sync then immediately cancel (tests cancel with no active render)
    REQUIRE_NOTHROW(renderer.cancel());

    // Double cancel is safe
    REQUIRE_NOTHROW(renderer.cancel());

    // Render completes normally after cancelled state cleared
    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);
    REQUIRE(result != nullptr);
    REQUIRE(result->thumbnails.size() == 2);
}

// ============================================================================
// MULTI-LAYER
// ============================================================================

TEST_CASE("GCodeObjectThumbnailRenderer: multiple layers are rendered", "[object-thumbnail]") {
    ParsedGCodeFile gcode;

    GCodeObject obj;
    obj.name = "tall_part";
    obj.bounding_box.expand(glm::vec3(10.0f, 10.0f, 0.2f));
    obj.bounding_box.expand(glm::vec3(90.0f, 90.0f, 2.0f));
    gcode.objects["tall_part"] = obj;

    // Add multiple layers with different segments
    for (int i = 0; i < 5; ++i) {
        Layer layer;
        layer.z_height = 0.2f * (i + 1);

        ToolpathSegment seg;
        seg.start = glm::vec3(10.0f + i * 5.0f, 10.0f, layer.z_height);
        seg.end = glm::vec3(90.0f - i * 5.0f, 90.0f, layer.z_height);
        seg.is_extrusion = true;
        seg.object_name_index = gcode.intern_object_name("tall_part");
        layer.segments.push_back(seg);
        layer.segment_count_extrusion = 1;

        gcode.layers.push_back(std::move(layer));
    }

    GCodeObjectThumbnailRenderer renderer;
    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result->thumbnails.size() == 1);
    // All layers contribute pixels, so we should have more pixels than a single line
    size_t drawn = count_drawn_pixels(result->thumbnails[0]);
    REQUIRE(drawn > 20); // Multiple overlapping diagonal lines
}

// ============================================================================
// BYTE SIZE AND MEMORY
// ============================================================================

TEST_CASE("GCodeObjectThumbnailRenderer: byte_size calculation is correct", "[object-thumbnail]") {
    GCodeObjectThumbnailRenderer renderer;
    auto gcode = make_single_object_gcode();

    auto result = renderer.render_sync(&gcode, 40, 40, TEST_COLOR);

    REQUIRE(result->thumbnails[0].byte_size() == 40 * 40 * 4);
}
