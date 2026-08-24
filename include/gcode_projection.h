// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_parser.h"

#include <glm/glm.hpp>

namespace helix::gcode {

// ============================================================================
// VIEW MODES
// ============================================================================

/// View mode for 2D projection of 3D toolpath data.
/// Shared by all renderers (layer renderer, thumbnail renderer, etc.)
enum class ViewMode {
    TOP_DOWN, ///< X/Y plane from above
    FRONT,    ///< Isometric-style: -45° horizontal + 45° elevation (default)
    ISOMETRIC ///< X/Y plane with isometric projection (45° rotation, Y compressed)
};

// ============================================================================
// PROJECTION CONSTANTS
// ============================================================================

/// Projection constants for FRONT view (-45° azimuth, 45° elevation).
/// Matching OrcaSlicer's default thumbnail camera (Camera.cpp zenit=45°, phi=45°).
namespace projection {

// 90° CCW pre-rotation (applied before horizontal rotation)
// new_x = -old_y, new_y = old_x

// Horizontal rotation: -45° (view from front-right corner)
constexpr float COS_H = 0.7071f;  // cos(45°)
constexpr float SIN_H = -0.7071f; // sin(-45°)

// Elevation angle: 45° looking down (matches OrcaSlicer thumbnail camera)
constexpr float COS_E = 0.7071f; // cos(45°)
constexpr float SIN_E = 0.7071f; // sin(45°)

// Isometric constants
constexpr float ISO_ANGLE = 0.7071f; // cos(45°)
constexpr float ISO_Y_SCALE = 0.5f;  // Y compression factor

/// How much taller than wide a model must project before it is allowed to run
/// under a bottom-anchored UI strip instead of being shrunk to clear it.
///
/// Fitting into the unoccluded area costs exactly (1 - occlusion) whenever
/// height is the binding axis — the same cost for a cube as for a tower — so
/// the cost cannot tell the two apart and the decision has to be about shape.
/// At 2.0 a cube, a cylinder and a flat plate (h/w 1.21, 1.21, 0.76 projected)
/// all stay fully visible, while a 10x10x50 post (3.21) keeps its size and
/// accepts the overlap.
constexpr float ELONGATION_LIMIT = 2.0f;

} // namespace projection

// ============================================================================
// PROJECTION PARAMETERS
// ============================================================================

/// Parameters for world-to-screen coordinate transformation.
/// Captured as a snapshot for thread-safe rendering.
struct ProjectionParams {
    ViewMode view_mode = ViewMode::FRONT;
    float scale = 1.0f;
    float offset_x = 0.0f; ///< World-space center X
    float offset_y = 0.0f; ///< World-space center Y
    float offset_z = 0.0f; ///< World-space center Z (FRONT view only)
    int canvas_width = 0;
    int canvas_height = 0;
    float content_offset_y_percent =
        0.0f; ///< Vertical shift for UI overlap (layer renderer only, 0.0 for thumbnails)
};

// ============================================================================
// PROJECTION FUNCTIONS
// ============================================================================

/// Convert world coordinates to screen pixel coordinates.
///
/// This is the single source of truth for 2D projection across all renderers.
/// Supports TOP_DOWN, FRONT, and ISOMETRIC view modes.
///
/// @param params  Projection parameters (view mode, scale, offsets, canvas size)
/// @param x       World X coordinate (mm)
/// @param y       World Y coordinate (mm)
/// @param z       World Z coordinate (mm) - used by FRONT view
/// @return Screen coordinates in pixels (origin at top-left of canvas)
glm::ivec2 project(const ProjectionParams& params, float x, float y, float z = 0.0f);

/// Result of auto-fit computation.
struct AutoFitResult {
    float scale = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    /// Projected extent of the box at `scale`, in canvas pixels, padding
    /// included.
    float content_width = 0.0f;
    float content_height = 0.0f;
    /// Vertical shift for project(), as a fraction of canvas height. Computed
    /// from the same occlusion the scale above already accounts for, so the two
    /// can never disagree.
    float content_offset_y_percent = 0.0f;
    /// True when the model projected taller than ELONGATION_LIMIT x its width
    /// and was therefore fitted to the full canvas rather than the clear area.
    bool elongated = false;
};

/// Vertical shift, as a fraction of canvas height, that keeps the top of the
/// projected content on screen while letting a tall model run underneath a
/// bottom-anchored UI strip.
///
/// The preview cards stack a translucent metadata strip over the bottom of the
/// canvas. Auto-fit scales against the FULL canvas height, so a short, wide
/// model ends up well clear of the strip while a tall, narrow one fills the
/// height and would sit behind it. One rule covers both without inspecting the
/// model's shape, because the shape is already expressed in `content_height`:
///
///   clear = canvas_height * (1 - bottom_occlusion)
///   top   = max(0, (clear - content_height) / 2)
///
/// While the content fits the clear area it is centred there — nothing clipped,
/// nothing hidden. Once it is taller, `top` pins to zero: the top edge stays on
/// screen and the surplus runs under the strip, which is what the translucency
/// is for. The two branches agree at content_height == clear, so the shift moves
/// continuously as a model grows. Everything is a ratio of canvas height, so the
/// result is independent of resolution, orientation, and aspect.
///
/// @param content_height_px  Projected content height (AutoFitResult::content_height)
/// @param canvas_height_px   Full canvas height in pixels
/// @param bottom_occlusion   Fraction of canvas height covered at the bottom, 0..1
/// @return Offset as a fraction of canvas height; negative shifts content up.
float compute_content_offset_y(float content_height_px, int canvas_height_px,
                               float bottom_occlusion);

/// Compute projection scale and offsets to fit a bounding box within a canvas.
///
/// @param raw_bb        Bounding box to fit (world coordinates, mm). Normalized
///                      internally, so an axis that arrived inverted does not
///                      discard the axes that were valid.
/// @param view_mode     Projection mode
/// @param canvas_width  Canvas width in pixels
/// @param canvas_height Canvas height in pixels
/// @param padding       Fractional padding around content (e.g. 0.05 = 5% each side)
/// @param bottom_occlusion Fraction of canvas height covered by UI at the bottom
///                      (0..1). A squat model is scaled to sit entirely above it;
///                      one taller than ELONGATION_LIMIT x its width keeps the
///                      full canvas and runs underneath instead. 0 disables both.
/// @return Scale and offset parameters for use with project()
AutoFitResult compute_auto_fit(const AABB& raw_bb, ViewMode view_mode, int canvas_width,
                               int canvas_height, float padding = 0.05f,
                               float bottom_occlusion = 0.0f);

// ============================================================================
// DEPTH SHADING
// ============================================================================

/// Depth shading constants shared by all 2D renderers.
/// Bottom of model = darker, top = brighter. Back = slightly darker than front.
namespace depth_shading {

constexpr float MIN_BRIGHTNESS = 0.4f;        ///< Brightness at bottom (Z min)
constexpr float BRIGHTNESS_RANGE = 0.6f;      ///< Added at top (total = 0.4 + 0.6 = 1.0)
constexpr float BACK_FADE_MIN = 0.85f;        ///< Brightness at back (Y max)
constexpr float BACK_FADE_RANGE = 0.15f;      ///< Added at front (total = 0.85 + 0.15 = 1.0)
constexpr float FULL_GRADIENT_HEIGHT = 50.0f; ///< Objects shorter than this get compressed gradient

} // namespace depth_shading

/// Compute depth-based brightness factor for fake-3D shading in FRONT view.
///
/// Combines Z-height gradient (bottom=40%, top=100%) with subtle Y-depth fade
/// (front=100%, back=85%). Used by both the full-scene layer renderer and
/// per-object thumbnail renderer.
///
/// @param avg_z  Average Z of the segment
/// @param z_min  Minimum Z of the model/object bounding box
/// @param z_max  Maximum Z of the bounding box
/// @param avg_y  Average Y of the segment
/// @param y_min  Minimum Y of the model/object bounding box
/// @param y_max  Maximum Y of the bounding box
/// @return Brightness multiplier in [~0.34, 1.0]
inline float compute_depth_brightness(float avg_z, float z_min, float z_max, float avg_y,
                                      float y_min, float y_max) {
    constexpr float EPSILON = 0.001f;

    // Z-height: bottom=darker, top=brighter.
    // Short objects (<50mm) get a compressed gradient so they look more uniform.
    float brightness = depth_shading::MIN_BRIGHTNESS;
    float z_range = z_max - z_min;
    if (z_range > EPSILON) {
        float norm_z = (avg_z - z_min) / z_range;
        if (norm_z < 0.0f)
            norm_z = 0.0f;
        if (norm_z > 1.0f)
            norm_z = 1.0f;
        float height_scale = (z_range < depth_shading::FULL_GRADIENT_HEIGHT)
                                 ? z_range / depth_shading::FULL_GRADIENT_HEIGHT
                                 : 1.0f;
        float effective_range = depth_shading::BRIGHTNESS_RANGE * height_scale;
        float floor =
            depth_shading::MIN_BRIGHTNESS + depth_shading::BRIGHTNESS_RANGE - effective_range;
        brightness = floor + effective_range * norm_z;
    }

    // Y-depth: front (low Y) = 100%, back (high Y) = 85%
    float y_range = y_max - y_min;
    if (y_range > EPSILON) {
        float norm_y = (avg_y - y_min) / y_range;
        if (norm_y < 0.0f)
            norm_y = 0.0f;
        if (norm_y > 1.0f)
            norm_y = 1.0f;
        brightness *=
            depth_shading::BACK_FADE_MIN + depth_shading::BACK_FADE_RANGE * (1.0f - norm_y);
    }

    return brightness;
}

} // namespace helix::gcode
