// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file gcode_selection_style.h
 * @brief The single answer to "how does a selected or excluded object look".
 *
 * Three renderers draw G-code, and each one used to decide this for itself:
 *   - GCodeLayerRenderer  (2D isometric, all platforms)
 *   - GCodeGLESRenderer   (3D, ENABLE_GLES_3D targets only: pi*, x86*)
 *   - GCodeRenderer       (3D CPU wireframe, every non-GLES target, which is
 *                          every embedded printer: ad5m, ad5x, cc1, k1, k2, u1)
 *
 * They disagreed: selection blue in the 2D cache path, theme "success" green in
 * the CPU wireframe, nothing at all in the GLES path, and a dead 1.8x brightness
 * bake in the geometry builder. Bracket color was 0xC0C0C0 in 2D and a
 * hand-written 0.75f in 3D under a comment claiming the two matched.
 *
 * This header owns the decision. Emission stays with each renderer, exactly like
 * AABB::for_each_bracket_arm() owns the bracket geometry while the renderers
 * differ in how they draw the resulting segments.
 *
 * Scope: selection and exclusion only. Base extrusion color, travel styling, and
 * depth/ghost shading remain each renderer's business.
 */

#include "gcode_parser.h" // helix::gcode::AABB
#include "gcode_raster.h" // helix::gcode::kSelectedAlpha

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>

namespace helix::gcode::selection {

/// Orange-red for excluded objects. Was gcode_layer_renderer.cpp's
/// EXCLUDED_OBJECT_COLOR; the CPU wireframe used theme "danger" instead, which
/// is a different hue. This value wins because it is what the isometric view
/// (the path users actually see) has always drawn.
inline constexpr uint32_t kExcludedColor = 0xFF6B35;

/// 60% opacity, spelled as the literal the software rasterizer needs. Equals
/// LV_OPA_60 for the lv_draw_line paths.
inline constexpr uint8_t kExcludedOpa = 153;

/// White silhouette outline for the selected object, matching OrcaSlicer's
/// selection treatment.
inline constexpr uint32_t kOutlineColor = 0xFFFFFF;

/// Light grey for the 24-arm corner bracket wireframe.
inline constexpr uint32_t kBracketColor = 0xC0C0C0;

/// Bracket arm length as a fraction of the shortest bbox edge, and its cap.
inline constexpr float kBracketArmFraction = 0.2f;
inline constexpr float kBracketArmMaxMm = 5.0f;

/// Below this, brackets are sub-pixel noise and are not drawn at all.
inline constexpr float kBracketArmMinMm = 0.01f;

/// Silhouette rim width, in SCREEN PIXELS.
///
/// Both renderers derive the rim from where the object actually lands on screen:
/// the 3D shell pushes its vertices this far in screen space, and the 2D pass
/// scans this far for a neighbouring pixel that is not the selected object. So
/// this is the width you get, at any zoom, on any plate, on any panel.
///
/// It replaces a pair of world-space knobs that could not do that. The 2D halo
/// was a dilate-and-overpaint: draw the object wide in white, draw it again
/// narrower on top, keep what survived. That holds up on a vertical wall, where
/// consecutive layers land on each other, and floods on a sloped one, where each
/// layer's white sticks out past the layer above it (a cone went solid white by
/// layer 120). The 3D shell pushed 0.25mm along the normal, which at plate-wide
/// zoom is roughly half a pixel: it survived on scattered pixels and read as
/// speckle, or as nothing at all.
inline constexpr int kOutlinePx = 2;
inline constexpr int kOutlineSmallPanelPx = 1;

/// Panels at or below this width get the narrower rim: 2px per side swallows a
/// small object whole at 480x272.
inline constexpr int kSmallPanelWidthPx = 320;

/// Rim width for a render target `target_width_px` pixels wide.
inline int outline_width_px(int target_width_px) {
    return (target_width_px <= kSmallPanelWidthPx) ? kOutlineSmallPanelPx : kOutlinePx;
}

/**
 * @brief Halo geometry for the draw-API fallback (TOP_DOWN / ISOMETRIC).
 *
 * Those view modes paint straight into the LVGL layer, so there is no pixel
 * buffer for the rim scan to read and they keep the older dilate-and-overpaint:
 * draw the walls wide in white, draw them again narrower on top, keep what
 * survives. That is sound here for exactly the reason it was not sound in the
 * stacked FRONT view - one layer is drawn, so there are no layers above to punch
 * through the white and no accumulation down a sloped wall.
 */
inline constexpr int kFallbackHaloDeltaPx = 6;
inline constexpr int kFallbackHaloDeltaSmallPanelPx = 4;

/**
 * @brief Resolved draw style for one segment.
 *
 * `override_color == false` means "keep whatever color the renderer computed"
 * (filament color, tool palette, depth shading). A selected object deliberately
 * keeps its own color: the halo carries the selection, as in Orca. Only
 * exclusion recolors.
 */
struct SegmentStyle {
    bool override_color = false;
    uint32_t rgb = 0;  ///< valid only when override_color
    uint8_t opa = 255; ///< selection/exclusion opacity; renderer applies its own for travels

    /// True when `opa` is kSelectedAlpha, i.e. this segment carries the tag the
    /// rim scan reads. The renderer must draw it WITHOUT antialiasing: the AA
    /// rasterizer writes coverage into alpha and would erase the tag along every
    /// edge, which is where the rim needs it most.
    bool tagged = false;

    /// Emit the draw-API fallback's halo for this segment. See kFallbackHaloDeltaPx.
    /// The cached path ignores it and uses `tagged` instead.
    bool fallback_halo = false;
};

/**
 * @brief Decide how a segment draws given its selection and exclusion state.
 *
 * Exclusion wins on color: that mirrors the existing cache-path precedence,
 * where the excluded branch `continue`s before the highlight check runs. The
 * halo is still emitted for an excluded-and-selected object, because you have to
 * see which object you picked in order to un-exclude it.
 *
 * Travels never halo. A travel move belonging to the selected object cuts across
 * the interior and would spray white through the middle of the silhouette.
 */
inline SegmentStyle resolve(bool excluded, bool highlighted, bool is_extrusion) {
    SegmentStyle s;
    if (excluded) {
        s.override_color = true;
        s.rgb = kExcludedColor;
        s.opa = kExcludedOpa;
    }
    if (highlighted && is_extrusion) {
        // The tag replaces the opacity, including the excluded object's 60%: an
        // object cannot be tagged and faded at once, and being able to see what
        // you just picked matters more than the fade. Excluded-and-selected
        // draws opaque orange inside a white rim, which reads correctly.
        s.opa = kSelectedAlpha;
        s.tagged = true;
        s.fallback_halo = true;
    }
    return s;
}

/// Whether a feature contributes to the draw-API fallback's halo.
///
/// Only the walls trace the object's contour. Haloing infill puts a white band
/// along every infill line, which reads as stripes across the middle of the
/// object instead of an outline. Unknown counts as eligible so a file with no
/// ;TYPE annotations still gets a halo from all of its extrusions rather than
/// none.
///
/// The tag path does NOT use this and deliberately tags every extrusion: the rim
/// is derived from the boundary of the tagged region, so tagging the interior
/// too is what stops infill gaps from reading as boundaries.
inline bool halo_feature(FeatureType t) {
    return t == FeatureType::OuterWall || t == FeatureType::OverhangWall ||
           t == FeatureType::Unknown;
}

/// Width of the fallback halo line drawn beneath a core line of `base_width`.
inline int halo_width(int base_width, bool small_panel) {
    return base_width + (small_panel ? kFallbackHaloDeltaSmallPanelPx : kFallbackHaloDeltaPx);
}

/**
 * @brief Corner-bracket arm length for a bounding box, in mm.
 * @return 0 when the box is empty or too small to bracket legibly.
 *
 * Was duplicated at gcode_layer_renderer.cpp:1485 and
 * gcode_gles_renderer.cpp:1942. The 2D copy had no is_empty() check and relied
 * on an infinite edge incidentally tripping the degeneracy guard.
 */
inline float bracket_arm_length(const AABB& bbox) {
    if (bbox.is_empty()) {
        return 0.0f;
    }
    const glm::vec3 d = bbox.size();
    const float min_edge = std::min({d.x, d.y, d.z});
    const float arm = std::min(min_edge * kBracketArmFraction, kBracketArmMaxMm);
    return (arm < kBracketArmMinMm) ? 0.0f : arm;
}

/// 0xRRGGBB plus 8-bit alpha to normalized floats, for the GLES uniforms. Exact
/// division by 255, not a hand-rounded literal.
inline glm::vec4 to_vec4(uint32_t rgb, uint8_t opa = 255) {
    return glm::vec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                     static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                     static_cast<float>(rgb & 0xFF) / 255.0f, static_cast<float>(opa) / 255.0f);
}

} // namespace helix::gcode::selection
