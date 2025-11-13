// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 HelixScreen Contributors
 *
 * This file is part of HelixScreen, which is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * See <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "gcode_camera.h"
#include "gcode_parser.h"
#include <glm/glm.hpp>
#include <lvgl/lvgl.h>
#include <optional>
#include <string>

/**
 * @file gcode_renderer.h
 * @brief 3D-to-2D renderer for G-code toolpath visualization
 *
 * Transforms 3D toolpath data to 2D screen coordinates and renders
 * using LVGL canvas drawing primitives. Supports layer filtering,
 * object highlighting, and level-of-detail optimization.
 *
 * Rendering pipeline:
 * 1. Frustum culling: Skip segments outside view
 * 2. Transform: Apply camera view+projection matrix
 * 3. Project: 3D world coordinates → 2D screen coordinates
 * 4. Clip: Clip lines to viewport bounds
 * 5. Draw: Use lv_draw_line() with style
 *
 * @see docs/GCODE_VISUALIZATION.md for complete design
 */

namespace gcode {

/**
 * @brief Level-of-detail setting for rendering
 */
enum class LODLevel {
    FULL = 0,     ///< Render all segments (high quality)
    HALF = 1,     ///< Render every 2nd segment (medium quality)
    QUARTER = 2   ///< Render every 4th segment (low quality/zoomed out)
};

/**
 * @brief Rendering options and filters
 */
struct RenderOptions {
    bool show_extrusions{true};        ///< Render extrusion moves
    bool show_travels{true};           ///< Render travel moves
    bool show_object_bounds{false};    ///< Render object boundary polygons
    std::string highlighted_object;    ///< Object to highlight (empty = none)
    LODLevel lod{LODLevel::FULL};      ///< Level of detail
    int layer_start{0};                ///< First layer to render (inclusive)
    int layer_end{-1};                 ///< Last layer to render (-1 = all)
};

/**
 * @brief 3D G-code renderer using LVGL canvas
 *
 * Usage pattern:
 * @code
 *   GCodeRenderer renderer;
 *   renderer.set_viewport_size(800, 480);
 *
 *   RenderOptions opts;
 *   opts.show_travels = false;  // Hide travel moves
 *   renderer.set_options(opts);
 *
 *   // In draw callback:
 *   renderer.render(layer, gcode_file, camera);
 * @endcode
 */
class GCodeRenderer {
  public:
    GCodeRenderer();
    ~GCodeRenderer() = default;

    // ==============================================
    // Rendering
    // ==============================================

    /**
     * @brief Render G-code to LVGL layer
     * @param layer LVGL draw layer (from draw event callback)
     * @param gcode Parsed G-code file
     * @param camera Camera with view/projection matrices
     *
     * Main rendering function. Call from LVGL draw event callback.
     * Renders according to current RenderOptions.
     */
    void render(lv_layer_t *layer,
                const ParsedGCodeFile &gcode,
                const GCodeCamera &camera);

    // ==============================================
    // Configuration
    // ==============================================

    /**
     * @brief Set viewport size
     * @param width Viewport width in pixels
     * @param height Viewport height in pixels
     */
    void set_viewport_size(int width, int height);

    /**
     * @brief Set rendering options
     * @param options Rendering configuration
     */
    void set_options(const RenderOptions &options);

    /**
     * @brief Get current rendering options
     * @return Current options
     */
    const RenderOptions &get_options() const { return options_; }

    // ==============================================
    // Convenience Setters
    // ==============================================

    /**
     * @brief Show/hide travel moves
     * @param show true to show, false to hide
     */
    void set_show_travels(bool show);

    /**
     * @brief Show/hide extrusion moves
     * @param show true to show, false to hide
     */
    void set_show_extrusions(bool show);

    /**
     * @brief Set highlighted object
     * @param name Object name to highlight (empty string to clear)
     */
    void set_highlighted_object(const std::string &name);

    /**
     * @brief Set level of detail
     * @param level LOD setting
     */
    void set_lod_level(LODLevel level);

    /**
     * @brief Set visible layer range
     * @param start First layer (inclusive, 0-based)
     * @param end Last layer (inclusive, -1 for all)
     */
    void set_layer_range(int start, int end);

    // ==============================================
    // Object Picking
    // ==============================================

    /**
     * @brief Pick object at screen coordinates
     * @param screen_pos Screen coordinates (pixels)
     * @param gcode Parsed G-code file
     * @param camera Current camera
     * @return Object name if picked, nullopt otherwise
     *
     * Used for touch/click interaction. Casts ray from screen position
     * through camera and tests intersection with object polygons.
     */
    std::optional<std::string> pick_object(
        const glm::vec2 &screen_pos,
        const ParsedGCodeFile &gcode,
        const GCodeCamera &camera) const;

    // ==============================================
    // Statistics
    // ==============================================

    /**
     * @brief Get number of segments rendered in last frame
     * @return Segment count
     */
    size_t get_segments_rendered() const { return segments_rendered_; }

    /**
     * @brief Get number of segments culled in last frame
     * @return Culled segment count
     */
    size_t get_segments_culled() const { return segments_culled_; }

  private:
    // ==============================================
    // Internal Rendering
    // ==============================================

    /**
     * @brief Render single layer
     * @param layer LVGL draw layer
     * @param gcode_layer Layer data
     * @param transform View-projection matrix
     */
    void render_layer(lv_layer_t *layer,
                     const Layer &gcode_layer,
                     const glm::mat4 &transform);

    /**
     * @brief Render single segment
     * @param layer LVGL draw layer
     * @param segment Toolpath segment
     * @param transform View-projection matrix
     */
    void render_segment(lv_layer_t *layer,
                       const ToolpathSegment &segment,
                       const glm::mat4 &transform);

    /**
     * @brief Render object boundary polygon
     * @param layer LVGL draw layer
     * @param object Object metadata
     * @param transform View-projection matrix
     */
    void render_object_boundary(lv_layer_t *layer,
                               const GCodeObject &object,
                               const glm::mat4 &transform);

    // ==============================================
    // Projection & Culling
    // ==============================================

    /**
     * @brief Project 3D world position to 2D screen coordinates
     * @param world_pos World-space position
     * @param transform View-projection matrix
     * @return Screen coordinates (pixels), or nullopt if outside view
     */
    std::optional<glm::vec2> project_to_screen(
        const glm::vec3 &world_pos,
        const glm::mat4 &transform) const;

    /**
     * @brief Check if segment should be rendered (filtering + culling)
     * @param segment Segment to test
     * @return true if should render
     */
    bool should_render_segment(const ToolpathSegment &segment) const;

    /**
     * @brief Clip line segment to viewport bounds
     * @param p1 First point (modified in-place)
     * @param p2 Second point (modified in-place)
     * @return true if line is visible after clipping
     */
    bool clip_line_to_viewport(glm::vec2 &p1, glm::vec2 &p2) const;

    // ==============================================
    // Drawing Helpers
    // ==============================================

    /**
     * @brief Get LVGL draw descriptor for segment
     * @param segment Toolpath segment
     * @return Line draw descriptor with style
     */
    lv_draw_line_dsc_t get_line_style(const ToolpathSegment &segment) const;

    /**
     * @brief Draw line on LVGL layer
     * @param layer LVGL draw layer
     * @param p1 Start point (screen coords)
     * @param p2 End point (screen coords)
     * @param dsc Line draw descriptor
     */
    void draw_line(lv_layer_t *layer,
                  const glm::vec2 &p1,
                  const glm::vec2 &p2,
                  const lv_draw_line_dsc_t &dsc);

    // Configuration
    int viewport_width_{800};
    int viewport_height_{480};
    RenderOptions options_;

    // Colors (will be set from theme in constructor)
    lv_color_t color_extrusion_;
    lv_color_t color_travel_;
    lv_color_t color_object_boundary_;
    lv_color_t color_highlighted_;
    lv_color_t color_excluded_;

    // Statistics (updated each frame)
    size_t segments_rendered_{0};
    size_t segments_culled_{0};
};

} // namespace gcode
