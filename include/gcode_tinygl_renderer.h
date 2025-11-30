// Copyright (c) 2025 Preston Brown <pbrown@brown-house.net>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TinyGL-based G-code 3D Renderer
// High-quality 3D visualization with lighting, smooth shading, and optimized geometry.

#pragma once

#include "gcode_camera.h"
#include "gcode_geometry_builder.h"
#include "gcode_parser.h"

#include <lvgl/lvgl.h>

#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <unordered_set>

namespace gcode {

/**
 * @brief Ghost layer rendering mode for print progress visualization
 *
 * Controls how "unprinted" layers above the current progress layer are rendered.
 */
enum class GhostRenderMode : uint8_t {
    Dimmed = 0,  ///< Darker color but fully opaque
    Stipple = 1  ///< Polygon stipple pattern (screen-door transparency) - DEFAULT
};

/// Default ghost render mode (configurable via this constant)
constexpr GhostRenderMode kDefaultGhostRenderMode = GhostRenderMode::Stipple;

/**
 * @brief TinyGL-based 3D renderer for G-code visualization
 *
 * Provides high-quality 3D rendering with:
 * - Smooth Gouraud shading
 * - Two-point studio lighting
 * - Optimized triangle strip geometry
 * - Sub-5MB memory footprint
 *
 * Designed as a drop-in replacement for GCodeRenderer with dramatically
 * improved visual quality.
 */
class GCodeTinyGLRenderer {
  public:
    GCodeTinyGLRenderer();
    ~GCodeTinyGLRenderer();

    // ==============================================
    // Main Rendering Interface (compatible with GCodeRenderer)
    // ==============================================

    /**
     * @brief Render G-code to LVGL layer
     * @param layer LVGL draw layer (from draw event callback)
     * @param gcode Parsed G-code file
     * @param camera Camera with view/projection matrices
     *
     * Main rendering function. Call from LVGL draw event callback.
     */
    void render(lv_layer_t* layer, const ParsedGCodeFile& gcode, const GCodeCamera& camera);

    /**
     * @brief Set viewport size
     * @param width Viewport width in pixels
     * @param height Viewport height in pixels
     */
    void set_viewport_size(int width, int height);

    // ==============================================
    // Configuration
    // ==============================================

    /**
     * @brief Set filament color from hex string
     * @param hex_color Color in hex format (e.g., "#26A69A")
     */
    void set_filament_color(const std::string& hex_color);

    /**
     * @brief Enable/disable smooth shading (Gouraud)
     * @param enable true for smooth shading, false for flat shading
     */
    void set_smooth_shading(bool enable);

    /**
     * @brief Set extrusion width (ribbon tube width)
     * @param width_mm Width in millimeters (default: 0.26mm)
     */
    void set_extrusion_width(float width_mm);

    /**
     * @brief Set geometry simplification tolerance
     * @param tolerance_mm Merge tolerance in mm (default: 0.15mm)
     */
    void set_simplification_tolerance(float tolerance_mm);

    /**
     * @brief Set material specular lighting parameters
     * @param intensity Specular intensity (0.0-0.2, where 0.0 = matte, 0.075 = OrcaSlicer default)
     * @param shininess Specular shininess/focus (5.0-50.0, where 20.0 = OrcaSlicer default)
     *
     * Controls reflective highlights on G-code surfaces. Higher intensity = brighter highlights.
     * Higher shininess = tighter/sharper highlights. Updates take effect on next render.
     */
    void set_specular(float intensity, float shininess);

    /**
     * @brief Enable/disable per-face debug coloring
     * @param enable true to assign distinct colors to each face for debugging
     *
     * When enabled, renders each face of the tube in a different bright color:
     * - Top face: Red (#FF0000)
     * - Bottom face: Blue (#0000FF)
     * - Left face: Green (#00FF00)
     * - Right face: Yellow (#FFFF00)
     * - Start end cap: Magenta (#FF00FF)
     * - End end cap: Cyan (#00FFFF)
     */
    void set_debug_face_colors(bool enable);

    // ==============================================
    // Compatibility Methods (for LVGL renderer interface)
    // ==============================================

    /**
     * @brief Set extrusion color (TinyGL uses filament color instead)
     * @param color Ignored - use set_filament_color() instead
     */
    void set_extrusion_color(lv_color_t color) {
        (void)color;
    }

    /**
     * @brief Set travel color (TinyGL doesn't render travel moves)
     * @param color Ignored
     */
    void set_travel_color(lv_color_t color) {
        (void)color;
    }

    /**
     * @brief Set brightness factor (not implemented for TinyGL)
     * @param factor Ignored
     */
    void set_brightness_factor(float factor) {
        (void)factor;
    }

    /**
     * @brief Get rendering options (layer range, etc.)
     */
    struct RenderingOptions {
        bool show_extrusions;
        bool show_travels;
        int layer_start;
        int layer_end;
        std::string highlighted_object;
    };
    RenderingOptions get_options() const;

    /**
     * @brief Pick object at screen position
     * @param screen_pos Screen coordinates (pixels)
     * @param gcode Parsed G-code file
     * @param camera Current camera
     * @return Object name if picked, nullopt otherwise
     */
    std::optional<std::string> pick_object(const glm::vec2& screen_pos,
                                           const ParsedGCodeFile& gcode,
                                           const GCodeCamera& camera) const;

    /**
     * @brief Show/hide travel moves
     * @param show true to show travel moves, false to hide
     */
    void set_show_travels(bool show);

    /**
     * @brief Show/hide extrusion moves
     * @param show true to show extrusion moves, false to hide
     */
    void set_show_extrusions(bool show);

    /**
     * @brief Set visible layer range
     * @param start First layer to render (0-based)
     * @param end Last layer to render (-1 for all)
     */
    void set_layer_range(int start, int end);

    /**
     * @brief Set highlighted object name (legacy single-object API)
     * @param name Object to highlight (empty to clear)
     */
    void set_highlighted_object(const std::string& name);

    /**
     * @brief Set highlighted object names (multi-select support)
     * @param names Set of objects to highlight (empty to clear all)
     */
    void set_highlighted_objects(const std::unordered_set<std::string>& names);

    /**
     * @brief Set excluded objects
     * @param names Set of object names that are excluded from print
     *
     * Excluded objects are rendered with a red/orange color at reduced opacity
     * to indicate they won't be printed.
     */
    void set_excluded_objects(const std::unordered_set<std::string>& names);

    /**
     * @brief Reset to default rendering settings
     */
    void reset_colors();

    /**
     * @brief Set global rendering opacity
     * @param opacity Opacity value (0-255)
     */
    void set_global_opacity(lv_opa_t opacity);

    // ==============================================
    // Print Progress / Ghost Layer Visualization
    // ==============================================

    /**
     * @brief Set print progress layer for ghost visualization
     * @param current_layer Layer index representing current print progress.
     *                      Layers 0..current_layer render solid.
     *                      Layers current_layer+1..max render as transparent ghost.
     *                      Set to -1 to disable ghost mode (render all solid).
     *
     * This enables a two-pass rendering mode where "printed" layers appear
     * solid and "unprinted" layers appear ghosted/transparent. Useful for
     * visualizing print progress during a print job.
     *
     * Performance: Layer changes are instant (<1ms) - no geometry rebuild needed.
     */
    void set_print_progress_layer(int current_layer);

    /**
     * @brief Set ghost layer opacity
     * @param opacity Opacity value (0=invisible, 255=fully opaque, default: 77 = ~30%)
     *
     * Controls how visible the ghost (unprinted) layers appear.
     */
    void set_ghost_opacity(lv_opa_t opacity);

    /**
     * @brief Get maximum layer index in current geometry
     * @return Max layer index, or -1 if no geometry loaded
     */
    int get_max_layer_index() const;

    /**
     * @brief Check if ghost mode is currently enabled
     * @return true if print progress visualization is active
     */
    bool is_ghost_mode_enabled() const {
        return ghost_mode_enabled_;
    }

    /**
     * @brief Set ghost layer rendering mode
     * @param mode Rendering mode for ghost layers (Dimmed, Stipple, Wireframe, DepthOnly)
     */
    void set_ghost_render_mode(GhostRenderMode mode);

    /**
     * @brief Get current ghost layer rendering mode
     * @return Current rendering mode
     */
    GhostRenderMode get_ghost_render_mode() const {
        return ghost_render_mode_;
    }

    // ==============================================
    // Statistics
    // ==============================================

    /**
     * @brief Get number of segments rendered (returns triangle count / 2)
     */
    size_t get_segments_rendered() const {
        return get_triangle_count() / 2;
    }

    /**
     * @brief Get number of unique colors in geometry (for multicolor detection)
     * @return Color palette size, or 0 if no geometry loaded
     */
    size_t get_geometry_color_count() const {
        return geometry_.has_value() ? geometry_->color_palette.size() : 0;
    }

    /**
     * @brief Get memory usage of last rendered geometry
     * @return Memory in bytes
     */
    size_t get_memory_usage() const;

    /**
     * @brief Get triangle count of last rendered geometry
     * @return Triangle count
     */
    size_t get_triangle_count() const;

  private:
    // ==============================================
    // Internal Rendering
    // ==============================================

    /**
     * @brief Initialize TinyGL context for current viewport
     */
    void init_tinygl();

    /**
     * @brief Shutdown TinyGL context
     */
    void shutdown_tinygl();

    /**
     * @brief Build or rebuild geometry from G-code
     * @param gcode Parsed G-code file
     */
    void build_geometry(const ParsedGCodeFile& gcode);

  public:
    /**
     * @brief Set pre-built geometry (for async loading)
     * @param geometry Pre-built ribbon geometry from background thread
     * @param filename Filename to associate with this geometry
     *
     * Allows setting geometry that was built in a background thread,
     * avoiding UI blocking during geometry construction.
     */
    void set_prebuilt_geometry(std::unique_ptr<RibbonGeometry> geometry,
                               const std::string& filename);

  private:
    /**
     * @brief Render geometry with TinyGL
     * @param camera Camera with view/projection
     */
    void render_geometry(const GCodeCamera& camera);

    /**
     * @brief Convert TinyGL framebuffer to LVGL image and draw to layer
     * @param layer LVGL draw layer
     */
    void draw_to_lvgl(lv_layer_t* layer);

    /**
     * @brief Setup lighting (two-point studio setup)
     */
    void setup_lighting();

    /**
     * @brief Render bounding box wireframe for highlighted object
     * @param gcode Parsed G-code file (contains object bounds)
     */
    void render_bounding_box(const ParsedGCodeFile& gcode);

    /**
     * @brief Render strips within a layer range (for two-pass ghost mode)
     * @param start_layer First layer to render (inclusive)
     * @param end_layer Last layer to render (inclusive)
     * @param dim_factor Color dimming factor (1.0 = full, 0.3 = 30% brightness for ghost)
     */
    void render_layer_range(int start_layer, int end_layer, float dim_factor);

    // Configuration
    int viewport_width_{800};
    int viewport_height_{600};
    bool smooth_shading_{false};  // Use flat shading to avoid triangle seam artifacts
    float extrusion_width_{0.5f}; // Wider for solid appearance
    SimplificationOptions simplification_;

    // Rendering options
    bool show_extrusions_{true};
    bool show_travels_{false};
    int layer_start_{0};
    int layer_end_{-1}; // -1 = all layers
    std::unordered_set<std::string> highlighted_objects_;
    std::unordered_set<std::string> excluded_objects_;  ///< Objects excluded from print
    lv_opa_t global_opacity_{LV_OPA_100};
    float brightness_factor_{1.0f};

    // Material lighting properties
    float specular_intensity_{0.10f}; // Default: subtle highlights (OrcaSlicer: 0.075)
    float specular_shininess_{20.0f}; // Default: soft, broad highlights (OrcaSlicer: 20.0)

    // Ghost layer (print progress) visualization
    int current_progress_layer_{-1};  ///< Layer up to which solid rendering applies (-1 = all solid)
    lv_opa_t ghost_opacity_{77};      ///< ~30% opacity for ghost layers
    bool ghost_mode_enabled_{false};  ///< True when showing print progress visualization
    GhostRenderMode ghost_render_mode_{kDefaultGhostRenderMode};  ///< How ghost layers are rendered
    uint8_t ghost_stipple_pattern_[128];  ///< 32x32 bit stipple pattern for screen-door effect

    // TinyGL context (opaque pointer to avoid header dependency)
    void* zbuffer_{nullptr};
    unsigned int* framebuffer_{nullptr};

    // Geometry
    std::unique_ptr<GeometryBuilder> geometry_builder_;
    std::optional<RibbonGeometry> geometry_;
    std::string current_gcode_filename_; // Track if we need to rebuild

    // LVGL image buffer for display
    lv_draw_buf_t* draw_buf_{nullptr};
};

} // namespace gcode
