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

/// Halo thickness added to the core line width, total across both sides. Even so
/// the outline is symmetric about the core rather than reading as a drop shadow.
inline constexpr int kHaloDeltaPx = 4;
inline constexpr int kHaloDeltaSmallPanelPx = 2;

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
    bool halo = false; ///< caller must emit the halo pass for this segment
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
    s.halo = highlighted && is_extrusion;
    if (excluded) {
        s.override_color = true;
        s.rgb = kExcludedColor;
        s.opa = kExcludedOpa;
    }
    return s;
}

/// Width of the halo line drawn beneath a core line of `base_width`.
inline int halo_width(int base_width, bool small_panel) {
    return base_width + (small_panel ? kHaloDeltaSmallPanelPx : kHaloDeltaPx);
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
