// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_camera.h"
#include "gcode_parser.h"

#include <lvgl/lvgl.h>

#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <unordered_set>

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
 */

namespace helix {
namespace gcode {

/**
 * @brief Level-of-detail setting for rendering
 */
enum class LODLevel {
    FULL = 0,   ///< Render all segments (high quality)
    HALF = 1,   ///< Render every 2nd segment (medium quality)
    QUARTER = 2 ///< Render every 4th segment (low quality/zoomed out)
};

/**
 * @brief Ghost layer rendering mode (for print progress visualization)
 *
 * Note: Ghost rendering is primarily a 3D renderer feature. The 2D renderer
 * provides these stubs for API compatibility but doesn't render ghost layers.
 */
enum class GhostRenderMode : uint8_t {
    Dimmed = 0, ///< Reduce opacity of unprinted layers
    Stipple = 1 ///< Use stipple pattern for unprinted layers
};

/**
 * @brief Rendering options and filters
 */
struct RenderOptions {
    bool show_extrusions{true};     ///< Render extrusion moves
    bool show_travels{false};       ///< Render travel moves (hidden by default)
    bool show_object_bounds{false}; ///< Render object boundary polygons
    std::string highlighted_object; ///< Object to highlight (legacy single-object)
    std::unordered_set<std::string> highlighted_objects; ///< Objects to highlight (multi-select)
    LODLevel lod{LODLevel::FULL};                        ///< Level of detail
    int layer_start{0};                                  ///< First layer to render (inclusive)
    int layer_end{-1};                                   ///< Last layer to render (-1 = all)
    std::unordered_set<std::string> excluded_objects;    ///< Objects excluded from print
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
     * @param widget_coords Optional widget coordinates (ignored in 2D renderer)
     *
     * Main rendering function. Call from LVGL draw event callback.
     * Renders according to current RenderOptions.
     * The widget_coords parameter is for API compatibility with the 3D renderer.
     */
    void render(lv_layer_t* layer, const ParsedGCodeFile& gcode, const GCodeCamera& camera,
                const lv_area_t* widget_coords = nullptr);

    /**
     * @brief Set interaction mode (stub for API compatibility)
     * @param interacting true if user is dragging/interacting
     *
     * In the 3D renderer, this enables reduced quality during interaction.
     * In the 2D renderer, this is a no-op.
     */
    void set_interaction_mode(bool interacting) {
        (void)interacting; // No-op in 2D renderer
    }

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
    void set_options(const RenderOptions& options);

    /**
     * @brief Get current rendering options
     * @return Current options
     */
    const RenderOptions& get_options() const {
        return options_;
    }

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
     * @brief Set highlighted object (legacy single-object API)
     * @param name Object name to highlight (empty string to clear)
     */
    void set_highlighted_object(const std::string& name);

    /**
     * @brief Set highlighted objects (multi-select API)
     * @param names Set of object names to highlight (empty set to clear)
     */
    void set_highlighted_objects(const std::unordered_set<std::string>& names);

    /**
     * @brief Set excluded objects
     * @param names Set of object names that are excluded from print
     *
     * Excluded objects are rendered with a red/orange strikethrough style
     * at reduced opacity to indicate they won't be printed.
     */
    void set_excluded_objects(const std::unordered_set<std::string>& names);

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
    // Print Progress / Ghost Layer (Stubs for API Compatibility)
    // ==============================================

    /**
     * @brief Set current print progress layer (stub)
     * @param layer Current layer being printed
     *
     * In the 3D renderer, this enables ghost rendering of unprinted layers.
     * In the 2D renderer, this is a no-op (ghost rendering not supported).
     */
    void set_print_progress_layer(int layer) {
        (void)layer; // No-op in 2D renderer
    }

    /**
     * @brief Set ghost layer opacity (stub)
     * @param opacity Opacity for ghost layers (0-255)
     *
     * In the 3D renderer, this controls ghost layer visibility.
     * In the 2D renderer, this is a no-op.
     */
    void set_ghost_opacity(lv_opa_t opacity) {
        (void)opacity; // No-op in 2D renderer
    }

    /**
     * @brief Set vertical content offset (stub)
     * @param offset_percent Offset as percentage of canvas height (-1.0 to 1.0)
     *
     * In the 3D renderer, this shifts the projection vertically.
     * In the 2D renderer, this is handled by GCodeLayerRenderer directly.
     */
    void set_content_offset_y(float offset_percent) {
        (void)offset_percent; // No-op in base renderer
    }

    /**
     * @brief Set ghost layer render mode (stub)
     * @param mode Ghost rendering style
     *
     * In the 3D renderer, this selects dimmed vs stipple rendering.
     * In the 2D renderer, this is a no-op.
     */
    void set_ghost_render_mode(GhostRenderMode mode) {
        (void)mode; // No-op in 2D renderer
    }

    /**
     * @brief Get maximum layer index
     * @return Number of layers - 1, or -1 if no data
     *
     * Note: This returns -1 for the 2D renderer since it doesn't track
     * geometry internally. Use ui_gcode_viewer_get_layer_count() instead.
     */
    int get_max_layer_index() const {
        return -1; // 2D renderer doesn't track geometry
    }

    // ==============================================
    // Color & Rendering Control
    // ==============================================

    /**
     * @brief Set custom extrusion color
     * @param color Color for extrusion moves
     *
     * Overrides theme default. Call with invalid color to reset to theme default.
     */
    void set_extrusion_color(lv_color_t color);

    /**
     * @brief Set custom travel move color
     * @param color Color for travel moves
     *
     * Overrides theme default. Call with invalid color to reset to theme default.
     */
    void set_travel_color(lv_color_t color);

    /**
     * @brief Set global rendering opacity
     * @param opacity Opacity value (0-255)
     */
    void set_global_opacity(lv_opa_t opacity);

    /**
     * @brief Set brightness multiplier
     * @param factor Brightness factor (0.5-2.0, clamped)
     *
     * Applied to all colors. Values >1.0 brighten, <1.0 darken.
     */
    void set_brightness_factor(float factor);

    /**
     * @brief Reset colors to theme defaults
     */
    void reset_colors();

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
    std::optional<std::string> pick_object(const glm::vec2& screen_pos,
                                           const ParsedGCodeFile& gcode,
                                           const GCodeCamera& camera) const;

    // ==============================================
    // Statistics
    // ==============================================

    /**
     * @brief Get number of segments rendered in last frame
     * @return Segment count
     */
    size_t get_segments_rendered() const {
        return segments_rendered_;
    }

    /**
     * @brief Get number of segments culled in last frame
     * @return Culled segment count
     */
    size_t get_segments_culled() const {
        return segments_culled_;
    }

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
    void render_layer(lv_layer_t* layer, const Layer& gcode_layer, const glm::mat4& transform);

    /**
     * @brief Render single segment
     * @param layer LVGL draw layer
     * @param segment Toolpath segment
     * @param transform View-projection matrix
     */
    void render_segment(lv_layer_t* layer, const ToolpathSegment& segment,
                        const glm::mat4& transform);

    /**
     * @brief Render object boundary polygon
     * @param layer LVGL draw layer
     * @param object Object metadata
     * @param transform View-projection matrix
     */
    void render_object_boundary(lv_layer_t* layer, const GCodeObject& object,
                                const glm::mat4& transform);

    // ==============================================
    // Projection & Culling
    // ==============================================

    /**
     * @brief Project 3D world position to 2D screen coordinates
     * @param world_pos World-space position
     * @param transform View-projection matrix
     * @return Screen coordinates (pixels), or nullopt if outside view
     */
    std::optional<glm::vec2> project_to_screen(const glm::vec3& world_pos,
                                               const glm::mat4& transform) const;

    /**
     * @brief Check if segment should be rendered (filtering + culling)
     * @param segment Segment to test
     * @return true if should render
     */
    bool should_render_segment(const ToolpathSegment& segment) const;

    /**
     * @brief Clip line segment to viewport bounds
     * @param p1 First point (modified in-place)
     * @param p2 Second point (modified in-place)
     * @return true if line is visible after clipping
     */
    bool clip_line_to_viewport(glm::vec2& p1, glm::vec2& p2) const;

    // ==============================================
    // Drawing Helpers
    // ==============================================

    /**
     * @brief Get LVGL draw descriptor for segment
     * @param segment Toolpath segment
     * @param normalized_depth Normalized depth value (0 = closest, 1 = farthest)
     * @return Line draw descriptor with style
     */
    lv_draw_line_dsc_t get_line_style(const ToolpathSegment& segment, float normalized_depth) const;

    /**
     * @brief Draw line on LVGL layer
     * @param layer LVGL draw layer
     * @param p1 Start point (screen coords)
     * @param p2 End point (screen coords)
     * @param dsc Line draw descriptor
     */
    void draw_line(lv_layer_t* layer, const glm::vec2& p1, const glm::vec2& p2,
                   const lv_draw_line_dsc_t& dsc);

    // Configuration
    int viewport_width_{800};
    int viewport_height_{480};
    RenderOptions options_;

    // Colors (lazily loaded from theme on first render)
    lv_color_t color_extrusion_{};
    lv_color_t color_travel_{};
    lv_color_t color_object_boundary_{};
    lv_color_t color_highlighted_{};
    lv_color_t color_excluded_{}; ///< Red/orange for excluded objects

    // Theme default colors (for reset)
    lv_color_t theme_color_extrusion_{};
    lv_color_t theme_color_travel_{};

    // Lazy initialization flag (colors loaded on first render when theme is ready)
    bool colors_initialized_{false};

    // Initialize colors from theme (called on first render)
    void ensure_colors_initialized();

    // Rendering control
    bool use_custom_extrusion_color_{false};
    bool use_custom_travel_color_{false};
    lv_opa_t global_opacity_{LV_OPA_90}; // Default opacity for all segments
    float brightness_factor_{1.0f};      // Brightness multiplier (0.5-2.0)

    // Depth-based rendering (computed per frame)
    glm::mat4 view_matrix_;   // Cached view matrix for depth calculations
    float depth_range_{1.0f}; // Depth range for normalization
    float min_depth_{0.0f};   // Minimum depth value
    float z_min_{0.0f};       // Minimum Z-height for color gradient
    float z_max_{1.0f};       // Maximum Z-height for color gradient

    // Statistics (updated each frame)
    size_t segments_rendered_{0};
    size_t segments_culled_{0};

    // Current gcode file (set during render/pick_object, valid only during call)
    const ParsedGCodeFile* current_gcode_{nullptr};
};

} // namespace gcode
} // namespace helix
