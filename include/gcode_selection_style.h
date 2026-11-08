// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file gcode_selection_style.h
 * @brief The single answer to "how does a selected or excluded object look".
 *
 * Two renderers draw G-code:
 *   - GCodeLayerRenderer  (2D isometric, all platforms, and the only renderer on
 *                          every non-GLES target: ad5m, ad5x, cc1, k1, k2, u1)
 *   - GCodeGLESRenderer   (3D, ENABLE_GLES_3D targets only: pi*, x86*)
 *
 * This header owns the decision, so both draw a selected or excluded object the
 * same way. Emission stays with each renderer, exactly like
 * AABB::for_each_bracket_arm() owns the bracket geometry while the renderers
 * differ in how they draw the resulting segments.
 *
 * Scope: selection and exclusion only. Base extrusion color, travel styling, and
 * depth/ghost shading remain each renderer's business.
 */

#include "gcode_parser.h" // helix::gcode::AABB
#include "gcode_raster.h" // helix::gcode::kSelectedAlpha

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace helix::gcode::selection {

/// An excluded object keeps its shading but loses its hue: excluded_grey() maps
/// the renderer's shaded color to a grey this bright at full white, plus this
/// floor so the darkest wall still reads against the dark viewer background.
inline constexpr int kExcludedGreyScalePct = 50;
inline constexpr int kExcludedGreyFloor = 70;

/// Hatch stripes over an excluded object, in SCREEN PIXELS at the widget size.
/// The GLES renderer scales these into its readback the same way it scales the
/// rim (scale_px).
inline constexpr int kHatchPeriodPx = 6;
inline constexpr int kHatchStripePx = 2;

/**
 * @brief The three selection colors, resolved once from ui_xml/gcode_tokens.xml.
 *
 * They are XML tokens (`gcode_selection_outline`, `_excluded`, `_bracket`), which
 * makes the XML the single source of truth and this struct the way the value
 * reaches a renderer.
 *
 * The member defaults are NOT a production fallback - every shipped tree
 * carries ui_xml, and no packaging rule omits it. They exist for the headless
 * unit tests: LVGLTestFixture brings up LVGL without theme_manager, so no token
 * is registered and palette_from_theme() has nothing to read. Keeping them equal
 * to the token file is what test_gcode_selection_style's drift case enforces.
 *
 * WHY A STRUCT AND NOT A LOOKUP AT THE POINT OF USE. Reading a token means
 * walking LVGL's const registry, which is main-thread-only, and the values are
 * consumed in software-rasterizer inner loops and on the ghost worker thread.
 * So a renderer resolves the palette ONCE on the main thread and carries it:
 * as a member for the cache path, and copied into the ghost worker's snapshot
 * alongside its other `local_*` state. Calling palette_from_theme() from the
 * ghost thread would be a background-thread LVGL access.
 */
struct Palette {
    uint32_t excluded = 0xFF3B30; ///< Red hatch stripes over excluded (cancelled) objects
    uint32_t outline = 0xFFFFFF;  ///< White silhouette rim on the selected object
    uint32_t bracket = 0xC0C0C0;  ///< Light grey 24-arm corner wireframe
};

/**
 * @brief Read the selection palette from the registered XML tokens.
 *
 * MAIN THREAD ONLY - see the note on Palette. A missing token keeps the struct's
 * default rather than reading back black; in practice that path is the headless
 * test fixture, not a shipped tree.
 */
Palette palette_from_theme();

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
/// A world-space width cannot do that. A 2D dilate-and-overpaint halo (draw the
/// object wide in white, then narrower on top) holds up on a vertical wall but
/// floods on a sloped one, where each layer's white sticks out past the layer
/// above it (a cone goes solid white by layer 120). A 3D shell pushed 0.25mm
/// along the normal is roughly half a pixel at plate-wide zoom and reads as
/// speckle, or as nothing at all.
inline constexpr int kOutlinePx = 2;
inline constexpr int kOutlineSmallPanelPx = 1;

/// How near a tap has to land, in screen pixels, to pick a toolpath. Shared
/// because both renderers hit-test the same way; two copies of the number would
/// let one view select where the other does not.
inline constexpr float kPickThresholdPx = 15.0f;

/// Panels at or below this width get the narrower rim: 2px per side swallows a
/// small object whole at 480x272.
inline constexpr int kSmallPanelWidthPx = 320;

/// Rim width for a render target `target_width_px` pixels wide.
inline int outline_width_px(int target_width_px) {
    return (target_width_px <= kSmallPanelWidthPx) ? kOutlineSmallPanelPx : kOutlinePx;
}

/// A widget-pixel length expressed in a readback `fbo_width_px` wide displayed at
/// `widget_width_px`, never below 1. Shared by the rim and the hatch.
inline int scale_px(int px, int widget_width_px, int fbo_width_px) {
    const float scale =
        static_cast<float>(fbo_width_px) / static_cast<float>(std::max(1, widget_width_px));
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(px) * scale)));
}

/// Rim width for a readback `fbo_width_px` wide that is displayed at a widget
/// `widget_width_px` wide: the GLES renderer strokes its rim on the readback,
/// which supersampled stills render at 2x and moving frames at half resolution.
/// Scaled from the widget's rim so the displayed width comes out the same at
/// any ratio, and never below 1: stroke_selection_rim ignores a rim smaller
/// than one pixel, and a highlighted object with no visible rim reads as
/// un-highlighted.
inline int outline_width_px_scaled(int widget_width_px, int fbo_width_px) {
    return scale_px(outline_width_px(widget_width_px), widget_width_px, fbo_width_px);
}

/**
 * @brief Resolved draw style for one segment.
 *
 * A selected object keeps its own color: the rim carries the selection, as in
 * Orca. An excluded object keeps its shading but is drained to grey
 * (excluded_grey) and tagged, so stroke_exclusion_hatch() can stripe it red.
 */
struct SegmentStyle {
    bool grey = false; ///< pass the renderer's shaded color through excluded_grey()
    uint8_t opa = 255; ///< alpha byte: opaque, or the tag a post-pass reads

    /// True when `opa` is a tag (kSelectedAlpha or kExcludedAlpha). The renderer
    /// must draw the segment WITHOUT antialiasing: the AA rasterizer writes
    /// coverage into alpha and would erase the tag along every edge, which is
    /// where the rim needs it most.
    bool tagged = false;
};

/**
 * @brief Decide how a segment draws given its selection and exclusion state.
 *
 * Excluded-and-selected draws grey inside the white rim, without stripes: a
 * pixel carries one tag, and you have to see which object you picked in order
 * to un-exclude it.
 *
 * Travels never take the selection tag. A travel move belonging to the selected
 * object cuts across the interior and would spray white through the middle of
 * the silhouette.
 */
inline SegmentStyle resolve(bool excluded, bool highlighted, bool is_extrusion) {
    SegmentStyle s;
    if (excluded) {
        s.grey = true;
        s.opa = kExcludedAlpha;
        s.tagged = true;
    }
    if (highlighted && is_extrusion) {
        s.opa = kSelectedAlpha;
        s.tagged = true;
    }
    return s;
}

/// The grey an excluded object draws in: the luminance of its shaded `rgb`,
/// compressed toward the middle so shading survives but no hue does.
inline uint32_t excluded_grey(uint32_t rgb) {
    const uint32_t r = (rgb >> 16) & 0xFF;
    const uint32_t g = (rgb >> 8) & 0xFF;
    const uint32_t b = rgb & 0xFF;
    const uint32_t luma = (r * 77 + g * 150 + b * 29) >> 8; // Rec.601, 0..255
    const uint32_t v = kExcludedGreyFloor + luma * kExcludedGreyScalePct / 100;
    return (v << 16) | (v << 8) | v;
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
