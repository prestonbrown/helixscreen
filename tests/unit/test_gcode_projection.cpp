// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_camera.h"
#include "gcode_projection.h"

#include <cmath>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <string>
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

// ============================================================================
// THUMBNAIL-PARITY FRAMING — the print-file detail view
//
// The detail view shows the slicer's embedded thumbnail and swaps in the live
// render once it has content. THUMBNAIL_PARITY frames that render the way the
// thumbnail itself is framed — a square of the canvas's short side, lifted
// 12% (the #preview_offset_y token), contain-fit — so the swap is not a jump
// in size or position. The cases below pin that geometry on the real detail
// card (368x390, the same numbers the occlusion cases above use).
// ============================================================================

namespace {

/// The detail card's canvas, from the same measurement as the occlusion cases.
constexpr int PARITY_CW = 368;
constexpr int PARITY_CH = 390;

/// A box whose FRONT projection has the aspect of the measured OrcaSlicer
/// sample: 81.0% of the thumbnail frame tall, 70.3% wide. FRONT extents are
/// range_x = (dx+dy)*COS_H and range_y = dz*COS_E + (dx+dy)*COS_H*SIN_E, so
/// the height is solved from the target aspect.
AABB orca_aspect_box() {
    constexpr float target_aspect = 81.0f / 70.3f;
    const float footprint = 50.0f + 50.0f; // dx + dy
    const float dz = (target_aspect * footprint * projection::COS_H -
                      footprint * projection::COS_H * projection::SIN_E) /
                     projection::COS_E;
    return solid(50.0f, 50.0f, dz);
}

/// Top of the parity frame in pixels: the square of the short side, centred,
/// lifted by the thumbnail's -12% translate. Negative is expected — the
/// thumbnail's own image top sits there.
float parity_frame_top() {
    const float side = std::min<float>(PARITY_CW, PARITY_CH);
    return (PARITY_CH - side) * 0.5f - 0.12f * PARITY_CH;
}

/// Where the model's vertical centre lands: 55% of the frame height.
float parity_model_center_y() {
    const float side = std::min<float>(PARITY_CW, PARITY_CH);
    return parity_frame_top() + 0.55f * side;
}

/// Pixel-space extent of a box rendered with a fit applied — project() over
/// the eight corners, so the cases assert what actually lands on the canvas
/// (scale + offsets + shift together) rather than re-deriving the fit's own
/// arithmetic.
struct PixelBox {
    // mins start at +inf so the first projected corner always replaces them;
    // zero would stick wherever every coordinate is positive.
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = std::numeric_limits<float>::max();
    float max_y = std::numeric_limits<float>::lowest();

    float width() const {
        return max_x - min_x;
    }
    float height() const {
        return max_y - min_y;
    }
    float center_x() const {
        return (min_x + max_x) * 0.5f;
    }
    float center_y() const {
        return (min_y + max_y) * 0.5f;
    }
};

PixelBox projected_box(const AABB& bb, const AutoFitResult& fit, ViewMode view, int cw, int ch) {
    ProjectionParams p;
    p.view_mode = view;
    p.scale = fit.scale;
    p.offset_x = fit.offset_x;
    p.offset_y = fit.offset_y;
    p.offset_z = fit.offset_z;
    p.canvas_width = cw;
    p.canvas_height = ch;
    p.content_offset_y_percent = fit.content_offset_y_percent;

    PixelBox box;
    for (const glm::vec3& corner : bb.corners()) {
        const glm::ivec2 px = project(p, corner.x, corner.y, corner.z);
        box.min_x = std::min(box.min_x, static_cast<float>(px.x));
        box.max_x = std::max(box.max_x, static_cast<float>(px.x));
        box.min_y = std::min(box.min_y, static_cast<float>(px.y));
        box.max_y = std::max(box.max_y, static_cast<float>(px.y));
    }
    return box;
}

/// The measured Orca thumbnail on this card: the model reads 297px tall.
constexpr float MEASURED_THUMBNAIL_MODEL_PX = 297.0f;

} // namespace

TEST_CASE("compute_auto_fit - parity reproduces the measured thumbnail box",
          "[gcode][projection][parity]") {
    auto fit = compute_auto_fit(orca_aspect_box(), ViewMode::FRONT, PARITY_CW, PARITY_CH, 0.05f,
                                119.0f / PARITY_CH, FitFraming::THUMBNAIL_PARITY);

    // The padded box fills the limiting axis of the square exactly, so the
    // model itself covers 1 / (1 + 2*0.14) = 78.1% of the 368px side.
    REQUIRE(fit.content_height == Approx(368.0f).margin(0.5f));
    REQUIRE(fit.content_width < 368.0f);

    const PixelBox box =
        projected_box(orca_aspect_box(), fit, ViewMode::FRONT, PARITY_CW, PARITY_CH);

    // The whole point: within 5% of what the slicer thumbnail showed.
    REQUIRE(box.height() == Approx(MEASURED_THUMBNAIL_MODEL_PX).epsilon(0.05f));
    // ...which is the square's side at the parity fill, exactly.
    REQUIRE(box.height() == Approx(368.0f / 1.28f).margin(2.5f));

    // Horizontal: centred on the canvas.
    REQUIRE(box.center_x() == Approx(PARITY_CW * 0.5f).margin(1.0f));
    // Vertical: model centre at 55% of the lifted frame — above canvas centre,
    // tracking the thumbnail's -12% lift.
    REQUIRE(box.center_y() == Approx(parity_model_center_y()).margin(2.0f));
}

TEST_CASE("compute_auto_fit - parity ignores the metadata strip", "[gcode][projection][parity]") {
    const AABB bb = orca_aspect_box();
    auto with_strip = compute_auto_fit(bb, ViewMode::FRONT, PARITY_CW, PARITY_CH, 0.05f,
                                       119.0f / PARITY_CH, FitFraming::THUMBNAIL_PARITY);
    auto bare = compute_auto_fit(bb, ViewMode::FRONT, PARITY_CW, PARITY_CH, 0.05f, 0.0f,
                                 FitFraming::THUMBNAIL_PARITY);

    // The strip still exists in parity mode (the thumbnail is drawn over it
    // too), but it must not shrink or shift the model: matching the thumbnail
    // is the rule, and the thumbnail ignores it.
    REQUIRE(with_strip.scale == Approx(bare.scale));
    REQUIRE(with_strip.content_offset_y_percent == Approx(bare.content_offset_y_percent));
    REQUIRE(with_strip.content_height == Approx(bare.content_height));
}

TEST_CASE("compute_auto_fit - parity has no elongation escape", "[gcode][projection][parity]") {
    // A tower is exactly the model STANDARD framing lets run under the strip.
    // Parity frames it like the thumbnail would: inside the square, same
    // placement as everything else.
    auto fit = compute_auto_fit(solid(10, 10, 50), ViewMode::FRONT, PARITY_CW, PARITY_CH, 0.05f,
                                119.0f / PARITY_CH, FitFraming::THUMBNAIL_PARITY);
    REQUIRE_FALSE(fit.elongated);
    REQUIRE(fit.content_height == Approx(368.0f).margin(0.5f));

    const PixelBox box =
        projected_box(solid(10, 10, 50), fit, ViewMode::FRONT, PARITY_CW, PARITY_CH);
    REQUIRE(box.height() == Approx(368.0f / 1.28f).margin(2.5f));
    REQUIRE(box.center_y() == Approx(parity_model_center_y()).margin(2.0f));
}

TEST_CASE("compute_auto_fit - a wide plate fills the square's width under parity",
          "[gcode][projection][parity]") {
    // Width-limited (the third measured sample): the limiting axis is still
    // the square's side, and the placement rules do not change.
    const AABB plate = solid(100, 100, 5);
    auto fit = compute_auto_fit(plate, ViewMode::FRONT, PARITY_CW, PARITY_CH, 0.05f,
                                119.0f / PARITY_CH, FitFraming::THUMBNAIL_PARITY);
    REQUIRE(fit.content_width == Approx(368.0f).margin(0.5f));
    REQUIRE(fit.content_height < 368.0f);

    const PixelBox box = projected_box(plate, fit, ViewMode::FRONT, PARITY_CW, PARITY_CH);
    REQUIRE(box.width() == Approx(368.0f / 1.28f).margin(2.5f));
    REQUIRE(box.center_x() == Approx(PARITY_CW * 0.5f).margin(1.0f));
    REQUIRE(box.center_y() == Approx(parity_model_center_y()).margin(2.0f));
}

TEST_CASE("parity_content_offset_y - pins the model centre, not its edges",
          "[gcode][projection][parity]") {
    // The parity shift is a function of the canvas alone: the measured
    // thumbnails put the model centre at ~55% of the frame whatever the model
    // fills, so there is no content height to feed it.
    const float shift = parity_content_offset_y(PARITY_CW, PARITY_CH);
    REQUIRE(std::isfinite(shift));
    REQUIRE(shift < 0.0f); // the lift moves the centre above canvas centre

    // And it puts the centre where the thumbnail puts it.
    const float center_y = PARITY_CH * 0.5f + shift * PARITY_CH;
    REQUIRE(center_y == Approx(parity_model_center_y()).margin(0.5f));

    // Degenerate canvases are inert, not NaN.
    REQUIRE(parity_content_offset_y(0, PARITY_CH) == Approx(0.0f));
    REQUIRE(parity_content_offset_y(PARITY_CW, 0) == Approx(0.0f));
}

// ===========================================================================
// Clip-space projection helpers (DRY-4)
//
// These replaced three hand-written copies of the same eight-corner /
// point-to-segment math in gcode_gles_renderer.cpp, gcode_renderer.cpp and
// gcode_layer_renderer.cpp. The copies had drifted on the one predicate that
// matters, which is what these cases pin.
// ===========================================================================

namespace {

/// A perspective camera at +Z looking back toward the origin. With this MVP,
/// clip.w is the view-space depth: positive in front, negative behind.
glm::mat4 test_mvp() {
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0.0f, 0.0f, 100.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    return proj * view;
}

} // namespace

TEST_CASE("project_clip_to_screen places a point in front of the camera", "[projection][clip]") {
    const auto s = helix::gcode::project_clip_to_screen(test_mvp(), glm::vec3(0.0f), 200, 100);
    REQUIRE(s.has_value());
    // Dead centre of the world maps to dead centre of the viewport.
    CHECK(s->x == Catch::Approx(100.0f));
    CHECK(s->y == Catch::Approx(50.0f));
}

// THE REGRESSION. The candidate-bbox loop and the per-segment loop both used
// `std::abs(clip.w) < EPSILON`, which only rejects points near the w == 0 plane.
// A point behind the camera has a large NEGATIVE w, sails through that test, and
// divides to a mirrored NDC — reported at a confident, wrong screen position.
TEST_CASE("project_clip_to_screen rejects a point behind the camera", "[projection][clip]") {
    // The camera sits at z = +100 looking toward the origin, so z = +500 is
    // well behind it.
    const glm::vec3 behind(0.0f, 0.0f, 500.0f);

    const glm::vec4 clip = test_mvp() * glm::vec4(behind, 1.0f);
    REQUIRE(clip.w < 0.0f);              // precondition: it IS behind
    REQUIRE(std::abs(clip.w) > 0.0001f); // and the OLD abs() test would pass it

    CHECK_FALSE(helix::gcode::project_clip_to_screen(test_mvp(), behind, 200, 100).has_value());
}

TEST_CASE("project_aabb_to_screen reports invalid when the whole box is behind",
          "[projection][clip]") {
    helix::gcode::AABB box;
    box.min = glm::vec3(-10.0f, -10.0f, 400.0f);
    box.max = glm::vec3(10.0f, 10.0f, 600.0f);
    const auto sb = helix::gcode::project_aabb_to_screen(test_mvp(), box, 200, 100);
    CHECK_FALSE(sb.valid);
}

TEST_CASE("project_aabb_to_screen bounds a box in front of the camera", "[projection][clip]") {
    helix::gcode::AABB box;
    box.min = glm::vec3(-10.0f, -10.0f, -10.0f);
    box.max = glm::vec3(10.0f, 10.0f, 10.0f);
    const auto sb = helix::gcode::project_aabb_to_screen(test_mvp(), box, 200, 100);
    REQUIRE(sb.valid);
    CHECK(sb.min_x < sb.max_x);
    CHECK(sb.min_y < sb.max_y);
    // The box straddles the origin, so its screen box straddles the centre.
    CHECK(sb.contains(100.0f, 50.0f));
    // ...and a point far outside is only inside once given enough slack.
    CHECK_FALSE(sb.contains(sb.max_x + 20.0f, 50.0f));
    CHECK(sb.contains(sb.max_x + 20.0f, 50.0f, /*slack=*/25.0f));
}

TEST_CASE("point_segment_distance measures to the segment, not the infinite line",
          "[projection][pick]") {
    const glm::vec2 a(0.0f, 0.0f);
    const glm::vec2 b(10.0f, 0.0f);

    // Perpendicular, over the middle of the span.
    CHECK(helix::gcode::point_segment_distance({5.0f, 3.0f}, a, b) == Catch::Approx(3.0f));

    // Past the far end: clamped to b, so 5 out and 3 up is a 3-4-5 triangle.
    // The infinite-line answer would be 3.0, which is the bug this guards.
    CHECK(helix::gcode::point_segment_distance({14.0f, 3.0f}, a, b) == Catch::Approx(5.0f));

    // Past the near end, symmetrically.
    CHECK(helix::gcode::point_segment_distance({-4.0f, 3.0f}, a, b) == Catch::Approx(5.0f));
}

TEST_CASE("point_segment_distance handles a zero-length segment", "[projection][pick]") {
    // Two identical endpoints: the old copies divided by segment_length_sq and
    // leaned on a 0.0001f guard to avoid a NaN. Distance to the point is the
    // only sensible answer.
    const glm::vec2 p(3.0f, 4.0f);
    CHECK(helix::gcode::point_segment_distance(p, glm::vec2(0.0f), glm::vec2(0.0f)) ==
          Catch::Approx(5.0f));
}

TEST_CASE("GCodeCamera - parity zoom frames the model into the same square as the 2D path",
          "[gcode][projection][parity]") {
    // The 3D path must honour parity too: same square, same fill, so switching
    // render modes does not move the model. get_content_height_fraction() is
    // the model's height as a fraction of the viewport — the same quantity the
    // 2D path reports in AutoFitResult::content_height.
    GCodeCamera parity;
    parity.set_viewport_size(PARITY_CW, PARITY_CH);
    parity.set_framing(FitFraming::THUMBNAIL_PARITY);
    parity.fit_to_bounds(orca_aspect_box());
    REQUIRE(parity.get_content_height_fraction() * PARITY_CH ==
            Approx(368.0f / 1.28f).margin(2.0f));

    // And it is genuinely a different framing from STANDARD-on-this-card,
    // where the metadata strip shrinks a squat model by (1 - occlusion).
    GCodeCamera standard;
    standard.set_viewport_size(PARITY_CW, PARITY_CH);
    standard.set_bottom_occlusion(119.0f / PARITY_CH);
    standard.fit_to_bounds(orca_aspect_box());
    REQUIRE(parity.get_content_height_fraction() > standard.get_content_height_fraction());
}

TEST_CASE("2D and GLES parity framings agree on the model's on-screen height",
          "[gcode][projection][parity]") {
    // The parity rule is hand-derived twice - compute_auto_fit() in scale form
    // and GCodeCamera::fit_to_bounds() in zoom form - so the two can drift
    // apart when someone tunes one. This pins them to each other on the real
    // detail card: whatever the square, fill and lift resolve to, both paths
    // must put the same box on screen.
    const AABB bb = orca_aspect_box();
    auto fit = compute_auto_fit(bb, ViewMode::FRONT, PARITY_CW, PARITY_CH, 0.05f, 0.0f,
                                FitFraming::THUMBNAIL_PARITY);
    const PixelBox box = projected_box(bb, fit, ViewMode::FRONT, PARITY_CW, PARITY_CH);

    GCodeCamera camera;
    camera.set_viewport_size(PARITY_CW, PARITY_CH);
    camera.set_framing(FitFraming::THUMBNAIL_PARITY);
    camera.fit_to_bounds(bb);

    REQUIRE(camera.get_content_height_fraction() * PARITY_CH == Approx(box.height()).margin(2.0f));
}

TEST_CASE("thumbnail_parity::LIFT matches the #preview_offset_y design token",
          "[gcode][projection][parity]") {
    // The lift must equal what the XML applies to the thumbnail itself
    // (ui_xml/globals.xml), or the render lands displaced from where the
    // thumbnail sat at the swap. The token is hot-reloadable XML with no
    // rebuild, so only a test reading the file catches someone tuning it
    // without the C++ constant.
    std::ifstream in("ui_xml/globals.xml");
    REQUIRE(in.is_open());

    std::string line;
    bool found = false;
    float token = 0.0f;
    while (std::getline(in, line)) {
        const auto name_at = line.find("name=\"preview_offset_y\"");
        if (name_at == std::string::npos) {
            continue;
        }
        const auto value_at = line.find("value=\"", name_at);
        if (value_at == std::string::npos) {
            continue;
        }
        token = std::stof(line.substr(value_at + 7));
        found = true;
        break;
    }
    REQUIRE(found);
    REQUIRE(token == Approx(-100.0f * projection::thumbnail_parity::LIFT).margin(0.05f));
}
