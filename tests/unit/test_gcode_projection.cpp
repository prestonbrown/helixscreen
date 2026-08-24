// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_projection.h"

#include <utility>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;
using Catch::Approx;

// ============================================================================
// project() TESTS
// ============================================================================

TEST_CASE("project: TOP_DOWN maps center to canvas center", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::TOP_DOWN;
    params.scale = 1.0f;
    params.offset_x = 50.0f;
    params.offset_y = 50.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    auto p = project(params, 50.0f, 50.0f);
    REQUIRE(p.x == 50);
    REQUIRE(p.y == 50);
}

TEST_CASE("project: TOP_DOWN positive Y moves up (lower pixel Y)", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::TOP_DOWN;
    params.scale = 1.0f;
    params.offset_x = 0.0f;
    params.offset_y = 0.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    auto center = project(params, 0.0f, 0.0f);
    auto above = project(params, 0.0f, 10.0f);

    // Positive Y in world = lower pixel Y (toward top of screen)
    REQUIRE(above.y < center.y);
    REQUIRE(above.x == center.x);
}

TEST_CASE("project: TOP_DOWN ignores Z coordinate", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::TOP_DOWN;
    params.scale = 1.0f;
    params.offset_x = 0.0f;
    params.offset_y = 0.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    auto at_z0 = project(params, 10.0f, 20.0f, 0.0f);
    auto at_z100 = project(params, 10.0f, 20.0f, 100.0f);

    REQUIRE(at_z0.x == at_z100.x);
    REQUIRE(at_z0.y == at_z100.y);
}

TEST_CASE("project: FRONT view center maps to canvas center", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::FRONT;
    params.scale = 1.0f;
    params.offset_x = 50.0f;
    params.offset_y = 50.0f;
    params.offset_z = 5.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    auto p = project(params, 50.0f, 50.0f, 5.0f);
    REQUIRE(p.x == 50);
    REQUIRE(p.y == 50);
}

TEST_CASE("project: FRONT view higher Z moves up (lower pixel Y)", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::FRONT;
    params.scale = 1.0f;
    params.offset_x = 0.0f;
    params.offset_y = 0.0f;
    params.offset_z = 0.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    auto low = project(params, 0.0f, 0.0f, 0.0f);
    auto high = project(params, 0.0f, 0.0f, 10.0f);

    // Higher Z = lower pixel Y (toward top of screen)
    REQUIRE(high.y < low.y);
}

TEST_CASE("project: FRONT view Z contributes to Y displacement", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::FRONT;
    params.scale = 1.0f;
    params.offset_x = 0.0f;
    params.offset_y = 0.0f;
    params.offset_z = 0.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    // Two points same XY but different Z should have different screen positions
    auto z0 = project(params, 10.0f, 10.0f, 0.0f);
    auto z10 = project(params, 10.0f, 10.0f, 10.0f);

    REQUIRE(z0.y != z10.y);
}

TEST_CASE("project: content_offset_y_percent shifts Y", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::TOP_DOWN;
    params.scale = 1.0f;
    params.offset_x = 0.0f;
    params.offset_y = 0.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    params.content_offset_y_percent = 0.0f;
    auto no_offset = project(params, 0.0f, 0.0f);

    params.content_offset_y_percent = 0.1f;
    auto with_offset = project(params, 0.0f, 0.0f);

    // 10% of 100px = 10px shift down
    REQUIRE(with_offset.y == no_offset.y + 10);
}

TEST_CASE("project: ISOMETRIC view center maps to canvas center", "[projection]") {
    ProjectionParams params;
    params.view_mode = ViewMode::ISOMETRIC;
    params.scale = 1.0f;
    params.offset_x = 50.0f;
    params.offset_y = 50.0f;
    params.canvas_width = 100;
    params.canvas_height = 100;

    auto p = project(params, 50.0f, 50.0f);
    REQUIRE(p.x == 50);
    REQUIRE(p.y == 50);
}

// ============================================================================
// compute_auto_fit() TESTS
// ============================================================================

TEST_CASE("compute_auto_fit: basic square bounding box", "[projection]") {
    AABB bb;
    bb.expand(glm::vec3(0.0f, 0.0f, 0.0f));
    bb.expand(glm::vec3(100.0f, 100.0f, 0.0f));

    auto fit = compute_auto_fit(bb, ViewMode::TOP_DOWN, 100, 100);

    REQUIRE(fit.scale > 0.0f);
    REQUIRE(fit.offset_x == Catch::Approx(50.0f));
    REQUIRE(fit.offset_y == Catch::Approx(50.0f));
}

TEST_CASE("compute_auto_fit: wider canvas gives same scale as taller", "[projection]") {
    AABB bb;
    bb.expand(glm::vec3(0.0f, 0.0f, 0.0f));
    bb.expand(glm::vec3(100.0f, 100.0f, 0.0f));

    // Square object in wide canvas vs tall canvas
    auto fit_wide = compute_auto_fit(bb, ViewMode::TOP_DOWN, 200, 100);
    auto fit_tall = compute_auto_fit(bb, ViewMode::TOP_DOWN, 100, 200);

    // Both constrained by the smaller dimension, so scale should be the same
    REQUIRE(fit_wide.scale == Catch::Approx(fit_tall.scale));
}

TEST_CASE("compute_auto_fit: degenerate bbox gets valid result", "[projection]") {
    AABB bb;
    bb.expand(glm::vec3(50.0f, 50.0f, 0.0f));
    bb.expand(glm::vec3(50.0f, 50.0f, 0.0f)); // Zero-size

    auto fit = compute_auto_fit(bb, ViewMode::TOP_DOWN, 100, 100);

    // Should not produce inf/nan
    REQUIRE(std::isfinite(fit.scale));
    REQUIRE(fit.scale > 0.0f);
}

TEST_CASE("compute_auto_fit: FRONT view includes Z in fitting", "[projection]") {
    // Flat object (no Z extent)
    AABB flat;
    flat.expand(glm::vec3(0.0f, 0.0f, 0.0f));
    flat.expand(glm::vec3(100.0f, 100.0f, 0.2f));

    // Tall object (large Z extent)
    AABB tall;
    tall.expand(glm::vec3(0.0f, 0.0f, 0.0f));
    tall.expand(glm::vec3(100.0f, 100.0f, 200.0f));

    auto fit_flat = compute_auto_fit(flat, ViewMode::FRONT, 100, 100);
    auto fit_tall = compute_auto_fit(tall, ViewMode::FRONT, 100, 100);

    // Tall object needs smaller scale to fit
    REQUIRE(fit_tall.scale < fit_flat.scale);

    // FRONT view should set offset_z
    REQUIRE(fit_tall.offset_z == Catch::Approx(100.0f));
}

TEST_CASE("compute_auto_fit: padding reduces effective scale", "[projection]") {
    AABB bb;
    bb.expand(glm::vec3(0.0f, 0.0f, 0.0f));
    bb.expand(glm::vec3(100.0f, 100.0f, 0.0f));

    auto no_pad = compute_auto_fit(bb, ViewMode::TOP_DOWN, 100, 100, 0.0f);
    auto with_pad = compute_auto_fit(bb, ViewMode::TOP_DOWN, 100, 100, 0.1f);

    REQUIRE(with_pad.scale < no_pad.scale);
}

TEST_CASE("compute_auto_fit: TOP_DOWN offset_z stays zero", "[projection]") {
    AABB bb;
    bb.expand(glm::vec3(0.0f, 0.0f, 0.0f));
    bb.expand(glm::vec3(100.0f, 100.0f, 50.0f));

    auto fit = compute_auto_fit(bb, ViewMode::TOP_DOWN, 100, 100);

    // TOP_DOWN should not set offset_z
    REQUIRE(fit.offset_z == Catch::Approx(0.0f));
}

// ============================================================================
// CONTENT OFFSET — framing against a translucent bottom strip
// ============================================================================

namespace {

/// Where the content lands, in canvas pixels, once the offset is applied.
struct Placed {
    float top;
    float bottom;
};

Placed place(float content_h, int canvas_h, float occlusion) {
    const float off = compute_content_offset_y(content_h, canvas_h, occlusion);
    const float centre = static_cast<float>(canvas_h) * 0.5f + off * static_cast<float>(canvas_h);
    return Placed{centre - content_h * 0.5f, centre + content_h * 0.5f};
}

} // namespace

TEST_CASE("compute_content_offset_y - no occlusion leaves content centred", "[gcode][projection]") {
    // A layout whose metadata strip is a flex sibling BELOW the preview occludes
    // nothing. Any shift there is pure loss: it clips the top and buys no room.
    for (float h : {40.0f, 150.0f, 286.0f, 287.0f}) {
        REQUIRE(compute_content_offset_y(h, 287, 0.0f) == Approx(0.0f).margin(1e-6));
    }
}

TEST_CASE("compute_content_offset_y - top is never clipped", "[gcode][projection]") {
    // The invariant the whole rule exists to hold. Sweep shape against occlusion
    // and assert the content's top edge stays on canvas every time.
    for (int canvas_h : {120, 287, 390, 800}) {
        for (float frac = 0.05f; frac <= 1.0f; frac += 0.05f) {
            for (float occ : {0.0f, 0.1f, 0.33f, 0.5f, 0.75f}) {
                const float content_h = static_cast<float>(canvas_h) * frac;
                const Placed p = place(content_h, canvas_h, occ);
                INFO("canvas=" << canvas_h << " frac=" << frac << " occ=" << occ);
                REQUIRE(p.top >= -0.5f);
            }
        }
    }
}

TEST_CASE("compute_content_offset_y - squat content clears the strip entirely",
          "[gcode][projection]") {
    // "Square or round": width-limited, so it is short enough to sit wholly in
    // the clear area. It must be fully visible AND centred in that area, not
    // jammed against the top.
    const int canvas_h = 390;
    const float occlusion = 1.0f / 3.0f;
    const float clear_h = static_cast<float>(canvas_h) * (1.0f - occlusion);
    const float content_h = 150.0f; // comfortably shorter than clear_h (260)

    const Placed p = place(content_h, canvas_h, occlusion);

    REQUIRE(p.top > 0.0f);
    REQUIRE(p.bottom <= Approx(clear_h).margin(0.5f));
    // Centred within the clear area: equal gaps above and below.
    REQUIRE(p.top == Approx(clear_h - p.bottom).margin(0.5f));
}

TEST_CASE("compute_content_offset_y - tall content runs under the strip", "[gcode][projection]") {
    // "Long and skinny": height-limited, so it fills the canvas. Top pinned to
    // the top edge, surplus deliberately behind the translucent strip rather
    // than shrinking the model to dodge it.
    const int canvas_h = 390;
    const float occlusion = 1.0f / 3.0f;
    const float clear_h = static_cast<float>(canvas_h) * (1.0f - occlusion);

    const Placed p = place(static_cast<float>(canvas_h), canvas_h, occlusion);

    REQUIRE(p.top == Approx(0.0f).margin(0.5f));
    REQUIRE(p.bottom > clear_h); // genuinely overlapping the strip
    REQUIRE(p.bottom <= Approx(static_cast<float>(canvas_h)).margin(0.5f));
}

TEST_CASE("compute_content_offset_y - continuous at the crossover", "[gcode][projection]") {
    // The two branches meet where content height equals the clear height. A step
    // there would show up as the preview jumping as a model grows a layer.
    const int canvas_h = 390;
    const float occlusion = 0.25f;
    const float clear_h = static_cast<float>(canvas_h) * (1.0f - occlusion);

    const float just_under = compute_content_offset_y(clear_h - 0.5f, canvas_h, occlusion);
    const float just_over = compute_content_offset_y(clear_h + 0.5f, canvas_h, occlusion);

    REQUIRE(just_under == Approx(just_over).margin(0.005f));
}

TEST_CASE("compute_content_offset_y - depends only on ratios", "[gcode][projection]") {
    // Screen-size and aspect agnostic: the same shape on a 272px strip and an
    // 800px one must land at the same fractional offset.
    const float occlusion = 0.3f;
    for (float frac : {0.2f, 0.6f, 0.95f}) {
        const float small = compute_content_offset_y(272.0f * frac, 272, occlusion);
        const float large = compute_content_offset_y(800.0f * frac, 800, occlusion);
        INFO("frac=" << frac);
        REQUIRE(small == Approx(large).margin(1e-5));
    }
}

TEST_CASE("compute_content_offset_y - degenerate inputs are inert", "[gcode][projection]") {
    REQUIRE(compute_content_offset_y(100.0f, 0, 0.5f) == Approx(0.0f));
    REQUIRE(compute_content_offset_y(100.0f, -10, 0.5f) == Approx(0.0f));
    // Content taller than the canvas cannot have an unclipped top — nothing can
    // place it. What the clamp buys is that it degrades to the tall-content
    // branch instead of running away proportionally to how oversized it is.
    // (auto_fit never produces this; a stale height from a larger canvas can.)
    REQUIRE(compute_content_offset_y(10000.0f, 287, 0.3f) ==
            Approx(compute_content_offset_y(287.0f, 287, 0.3f)));
    // Occlusion outside 0..1 is clamped, not trusted.
    REQUIRE(place(100.0f, 287, 5.0f).top >= -0.5f);
    REQUIRE(compute_content_offset_y(100.0f, 287, -2.0f) ==
            Approx(compute_content_offset_y(100.0f, 287, 0.0f)));
}

TEST_CASE("compute_auto_fit - reports the projected content extent", "[gcode][projection]") {
    // content_height is what feeds the offset rule, so it has to match what the
    // limiting axis actually fills.
    AABB bb;
    bb.min = {0.0f, 0.0f, 0.0f};
    bb.max = {100.0f, 100.0f, 10.0f};

    SECTION("height-limited box fills the canvas height") {
        auto fit = compute_auto_fit(bb, ViewMode::TOP_DOWN, 1000, 200);
        REQUIRE(fit.content_height == Approx(200.0f).margin(0.5f));
        REQUIRE(fit.content_width <= Approx(1000.0f).margin(0.5f));
    }

    SECTION("width-limited box fills the canvas width") {
        auto fit = compute_auto_fit(bb, ViewMode::TOP_DOWN, 200, 1000);
        REQUIRE(fit.content_width == Approx(200.0f).margin(0.5f));
        REQUIRE(fit.content_height <= Approx(1000.0f).margin(0.5f));
    }

    SECTION("empty box reports no extent") {
        auto fit = compute_auto_fit(AABB{}, ViewMode::FRONT, 377, 287);
        REQUIRE(fit.content_height == Approx(0.0f));
    }
}

// ============================================================================
// ELONGATION — who is allowed under the strip
// ============================================================================

namespace {

/// Bounding box of a rectangular solid, centred on the bed.
AABB solid(float dx, float dy, float dz) {
    AABB bb;
    bb.min = {100.0f - dx * 0.5f, 100.0f - dy * 0.5f, 0.0f};
    bb.max = {100.0f + dx * 0.5f, 100.0f + dy * 0.5f, dz};
    return bb;
}

/// Bottom of the content relative to the top of the occluding strip.
/// <= 0 means the model sits entirely clear of it.
float overlap_into_strip(const AABB& bb, int cw, int ch, float occlusion) {
    auto fit = compute_auto_fit(bb, ViewMode::FRONT, cw, ch, 0.05f, occlusion);
    const float centre =
        static_cast<float>(ch) * 0.5f + fit.content_offset_y_percent * static_cast<float>(ch);
    const float bottom = centre + fit.content_height * 0.5f;
    return bottom - static_cast<float>(ch) * (1.0f - occlusion);
}

} // namespace

TEST_CASE("compute_auto_fit - squat models are framed clear of the strip", "[gcode][projection]") {
    // The cases from the bug report: a cube, a cylinder-ish solid and a flat
    // plate must all end up fully visible above the metadata, because none of
    // them is "really tall and skinny".
    const int cw = 368, ch = 390;
    const float occ = 119.0f / 390.0f; // measured on the real detail card

    for (auto [name, bb] : {std::pair{"10mm cube", solid(10, 10, 10)},
                            std::pair{"50 dia x 50 cylinder", solid(50, 50, 50)},
                            std::pair{"100x100x10 plate", solid(100, 100, 10)}}) {
        INFO(name);
        auto fit = compute_auto_fit(bb, ViewMode::FRONT, cw, ch, 0.05f, occ);
        REQUIRE_FALSE(fit.elongated);
        REQUIRE(overlap_into_strip(bb, cw, ch, occ) <= Approx(0.0f).margin(0.5f));
    }
}

TEST_CASE("compute_auto_fit - a tower keeps its size and runs under the strip",
          "[gcode][projection]") {
    const int cw = 368, ch = 390;
    const float occ = 119.0f / 390.0f;
    const AABB tower = solid(10, 10, 50);

    auto fit = compute_auto_fit(tower, ViewMode::FRONT, cw, ch, 0.05f, occ);
    REQUIRE(fit.elongated);
    // It genuinely reaches past the strip's top edge...
    REQUIRE(overlap_into_strip(tower, cw, ch, occ) > 0.0f);
    // ...and that buys real size: bigger than if it had been made to fit clear.
    auto squashed = compute_auto_fit(tower, ViewMode::FRONT, cw,
                                     static_cast<int>(ch * (1.0f - occ)), 0.05f, 0.0f);
    REQUIRE(fit.scale > squashed.scale);
}

TEST_CASE("compute_auto_fit - top is never clipped either way", "[gcode][projection]") {
    // The invariant that holds across the elongation branch, not just within it.
    const int cw = 368, ch = 390;
    for (float occ : {0.0f, 0.15f, 0.305f, 0.5f}) {
        for (float dz : {1.0f, 10.0f, 25.0f, 50.0f, 200.0f}) {
            const AABB bb = solid(10, 10, dz);
            auto fit = compute_auto_fit(bb, ViewMode::FRONT, cw, ch, 0.05f, occ);
            const float centre = static_cast<float>(ch) * 0.5f +
                                 fit.content_offset_y_percent * static_cast<float>(ch);
            INFO("occ=" << occ << " dz=" << dz);
            REQUIRE(centre - fit.content_height * 0.5f >= -0.5f);
        }
    }
}

TEST_CASE("compute_auto_fit - zero occlusion ignores shape entirely", "[gcode][projection]") {
    // With nothing covering the canvas (print status' layout) elongated and
    // squat models must both simply centre — no shift, no shrink.
    const int cw = 368, ch = 390;
    for (float dz : {10.0f, 200.0f}) {
        auto fit = compute_auto_fit(solid(10, 10, dz), ViewMode::FRONT, cw, ch, 0.05f, 0.0f);
        INFO("dz=" << dz);
        REQUIRE(fit.content_offset_y_percent == Approx(0.0f).margin(1e-6));
    }
}
