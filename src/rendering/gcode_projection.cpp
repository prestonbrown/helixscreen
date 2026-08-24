// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_GCODE_VIEWER

#include "gcode_projection.h"

#include <algorithm>

namespace helix::gcode {

using namespace projection;

// ============================================================================
// PROJECTION
// ============================================================================

glm::ivec2 project(const ProjectionParams& params, float x, float y, float z) {
    float sx, sy;
    const float half_w = static_cast<float>(params.canvas_width) / 2.0f;
    const float half_h = static_cast<float>(params.canvas_height) / 2.0f;

    switch (params.view_mode) {
    case ViewMode::FRONT: {
        // Isometric-style view: 45° horizontal rotation + 30° elevation
        // Creates a "corner view looking down" perspective

        // 90° CCW rotation around Z to match slicer thumbnail orientation
        float raw_dx = x - params.offset_x;
        float raw_dy = y - params.offset_y;
        float dx = -raw_dy; // 90° CCW: new_x = -old_y
        float dy = raw_dx;  // 90° CCW: new_y = old_x
        float dz = z - params.offset_z;

        // Horizontal rotation (around Z axis)
        float rx = dx * COS_H - dy * SIN_H;
        float ry = dx * SIN_H + dy * COS_H;

        // Elevation (tilt camera down)
        sx = rx * params.scale + half_w;
        sy = half_h - (dz * COS_E + ry * SIN_E) * params.scale;
        break;
    }

    case ViewMode::ISOMETRIC: {
        // Isometric projection (45° rotation with Y compression)
        float dx = x - params.offset_x;
        float dy = y - params.offset_y;

        float iso_x = (dx - dy) * ISO_ANGLE;
        float iso_y = (dx + dy) * ISO_ANGLE * ISO_Y_SCALE;

        sx = iso_x * params.scale + half_w;
        sy = half_h - iso_y * params.scale;
        break;
    }

    case ViewMode::TOP_DOWN:
    default: {
        // Top-down: X → screen X, Y → screen Y (flipped)
        float dx = x - params.offset_x;
        float dy = y - params.offset_y;
        sx = dx * params.scale + half_w;
        sy = half_h - dy * params.scale;
        break;
    }
    }

    // Apply content offset (shifts render for UI overlap - used by layer renderer)
    sy += params.content_offset_y_percent * static_cast<float>(params.canvas_height);

    return {static_cast<int>(sx), static_cast<int>(sy)};
}

// ============================================================================
// CONTENT OFFSET
// ============================================================================

float compute_content_offset_y(float content_height_px, int canvas_height_px,
                               float bottom_occlusion) {
    if (canvas_height_px <= 0) {
        return 0.0f;
    }

    const float canvas_h = static_cast<float>(canvas_height_px);
    const float occlusion = std::clamp(bottom_occlusion, 0.0f, 1.0f);
    const float clear_h = canvas_h * (1.0f - occlusion);

    // A caller that has not run auto-fit yet passes 0; clamping to the canvas
    // also keeps a stale content height from a larger canvas from driving the
    // top off screen.
    const float content_h = std::clamp(content_height_px, 0.0f, canvas_h);

    // Centre within the clear area while it fits, then pin the top edge.
    const float top = std::max(0.0f, (clear_h - content_h) * 0.5f);

    // project() adds this to the canvas-centred Y, so express it as the shift
    // from centre rather than as an absolute position.
    return (top + content_h * 0.5f - canvas_h * 0.5f) / canvas_h;
}

// ============================================================================
// AUTO-FIT
// ============================================================================

AutoFitResult compute_auto_fit(const AABB& raw_bb, ViewMode view_mode, int canvas_width,
                               int canvas_height, float padding, float bottom_occlusion) {
    AutoFitResult result;

    // Order any inverted axis before the emptiness test — one bad axis must not
    // discard the others. Shared with GCodeCamera::fit_to_bounds so the 2D and
    // 3D paths agree on which boxes are framable.
    const AABB bb = raw_bb.normalized();

    // A default-constructed AABB is {+inf, -inf}: every range below goes to -inf,
    // trips the degeneracy clamp, and — worse — the offsets become
    // (inf + -inf) / 2 = NaN, which poisons every projected point and makes the
    // int cast in project() undefined. Callers are expected to substitute a real
    // box, but guard here too so no caller can produce NaN.
    if (bb.is_empty()) {
        result.scale = 1.0f;
        result.offset_x = 0.0f;
        result.offset_y = 0.0f;
        result.offset_z = 0.0f;
        return result;
    }

    float range_x, range_y;

    switch (view_mode) {
    case ViewMode::FRONT: {
        float xy_range_x = bb.max.x - bb.min.x;
        float xy_range_y = bb.max.y - bb.min.y;
        float z_range = bb.max.z - bb.min.z;

        // Horizontal extent after 45° rotation (cos(-45°) = cos(45°) = 0.7071)
        range_x = (xy_range_x + xy_range_y) * COS_H;

        // Vertical extent: Z * cos(30°) + Y_depth * sin(30°)
        float y_depth = (xy_range_x + xy_range_y) * COS_H;
        range_y = z_range * COS_E + y_depth * SIN_E;

        result.offset_z = (bb.min.z + bb.max.z) / 2.0f;
        break;
    }

    case ViewMode::ISOMETRIC: {
        float xy_range_x = bb.max.x - bb.min.x;
        float xy_range_y = bb.max.y - bb.min.y;
        range_x = (xy_range_x + xy_range_y) * ISO_ANGLE;
        range_y = (xy_range_x + xy_range_y) * ISO_ANGLE * ISO_Y_SCALE;
        break;
    }

    case ViewMode::TOP_DOWN:
    default:
        range_x = bb.max.x - bb.min.x;
        range_y = bb.max.y - bb.min.y;
        break;
    }

    // Handle degenerate cases
    if (range_x < 0.001f)
        range_x = 1.0f;
    if (range_y < 0.001f)
        range_y = 1.0f;

    // Add padding
    range_x *= (1.0f + 2.0f * padding);
    range_y *= (1.0f + 2.0f * padding);

    // Shape decides which vertical budget the model gets. A model that is not
    // dramatically taller than it is wide is scaled to sit entirely above the
    // bottom UI strip; a genuinely elongated one keeps the whole canvas and is
    // allowed to run under it rather than shrinking by (1 - occlusion).
    // Ratios of the padded ranges, so this is independent of canvas size.
    result.elongated = range_y > ELONGATION_LIMIT * range_x;

    const float occlusion = std::clamp(bottom_occlusion, 0.0f, 1.0f);
    const float usable_height = result.elongated
                                    ? static_cast<float>(canvas_height)
                                    : static_cast<float>(canvas_height) * (1.0f - occlusion);

    // Scale to fit canvas (maintain aspect ratio)
    float scale_x = static_cast<float>(canvas_width) / range_x;
    float scale_y = usable_height / range_y;
    result.scale = std::min(scale_x, scale_y);

    result.offset_x = (bb.min.x + bb.max.x) / 2.0f;
    result.offset_y = (bb.min.y + bb.max.y) / 2.0f;

    // Projected extent at the chosen scale. The limiting axis lands exactly on
    // its canvas dimension; the other is smaller. compute_content_offset_y()
    // reads the shape of the model out of this rather than guessing from the
    // bounding box.
    result.content_width = range_x * result.scale;
    result.content_height = range_y * result.scale;
    result.content_offset_y_percent =
        compute_content_offset_y(result.content_height, canvas_height, occlusion);

    return result;
}

} // namespace helix::gcode

#endif // HELIX_HAS_GCODE_VIEWER
