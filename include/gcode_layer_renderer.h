// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gcode_color_palette.h"
#include "gcode_parser.h"
#include "gcode_projection.h"
#include "gcode_streaming_controller.h"

#include <lvgl/lvgl.h>

#include <atomic>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

namespace helix {
namespace gcode {

/**
 * @brief 2D orthographic layer renderer for G-code visualization
 *
 * Renders a single layer from a top-down view using direct X/Y → pixel
 * mapping. Optimized for low-power hardware (AD5M) without 3D matrix transforms.
 *
 * Features:
 * - Single layer rendering (fast, no depth sorting)
 * - Auto-fit to canvas bounds
 * - Toggle visibility of travels/supports
 * - Print progress integration (auto-follow current layer)
 *
 * Usage:
 * @code
 *   GCodeLayerRenderer renderer;
 *   renderer.set_gcode(&parsed_file);
 *   renderer.set_canvas_size(400, 400);
 *   renderer.auto_fit();
 *   renderer.set_current_layer(42);
 *   renderer.render(layer, clip_area);
 * @endcode
 */
class GCodeLayerRenderer {
  public:
    GCodeLayerRenderer();
    ~GCodeLayerRenderer();

    // =========================================================================
    // Data Source
    // =========================================================================

    /**
     * @brief Set G-code data source (full file mode)
     * @param gcode Pointer to parsed G-code file (not owned)
     *
     * Use this for files small enough to fit in memory. Clears any
     * streaming controller set via set_streaming_controller().
     */
    void set_gcode(const ParsedGCodeFile* gcode);

    /**
     * @brief Set streaming controller as data source (streaming mode)
     * @param controller Pointer to streaming controller (not owned)
     *
     * Use this for large files that should be streamed layer-by-layer.
     * Clears any parsed file set via set_gcode().
     *
     * In streaming mode:
     * - Layers are loaded on-demand via the controller
     * - Prefetching happens automatically for nearby layers
     * - Memory usage is bounded by the controller's cache budget
     */
    void set_streaming_controller(GCodeStreamingController* controller);

    /**
     * @brief Check if using streaming mode
     * @return true if streaming controller is set, false if using parsed file
     */
    bool is_streaming() const {
        return streaming_controller_ != nullptr;
    }

    /**
     * @brief Get current G-code data source (full file mode only)
     * @return Pointer to parsed G-code file, or nullptr if not set or in streaming mode
     */
    const ParsedGCodeFile* get_gcode() const {
        return gcode_;
    }

    /**
     * @brief Get streaming controller (streaming mode only)
     * @return Pointer to streaming controller, or nullptr if not in streaming mode
     */
    GCodeStreamingController* get_streaming_controller() const {
        return streaming_controller_;
    }

    // =========================================================================
    // Layer Selection
    // =========================================================================

    /**
     * @brief Set current layer to render
     * @param layer Layer index (0-based)
     */
    void set_current_layer(int layer);

    /**
     * @brief Get current layer index
     * @return Current layer (0-based)
     */
    int get_current_layer() const {
        return current_layer_;
    }

    /**
     * @brief Get total number of layers
     * @return Layer count, or 0 if no G-code loaded
     */
    int get_layer_count() const;

    // =========================================================================
    // Rendering
    // =========================================================================

    /**
     * @brief Render current layer to LVGL draw layer
     * @param layer LVGL draw layer (from draw event callback)
     * @param clip_area Clipping area for the render
     */
    void render(lv_layer_t* layer, const lv_area_t* clip_area);

    /**
     * @brief Check if renderer needs more frames to complete caching
     *
     * Progressive rendering renders N layers per frame to avoid UI blocking.
     * After calling render(), check this method - if true, the caller should
     * invalidate the widget to trigger another frame.
     *
     * @return true if more frames are needed to complete solid or ghost cache
     */
    bool needs_more_frames() const;

    /**
     * @brief Highest solid-cache layer rendered so far (-1 if cache empty)
     *
     * Diagnostic accessor for the renderer-stall watchdog in ui_gcode_viewer.
     * Lets the watchdog detect when print_progress_layer_ is advancing but
     * the cache is not catching up — which means a continuation invalidate
     * was dropped (UpdateQueue back-pressure, etc.) and the renderer is
     * stranded mid-print.
     */
    int get_cached_up_to_layer() const {
        return cached_up_to_layer_;
    }

    /**
     * @brief Set canvas dimensions
     * @param width Canvas width in pixels
     * @param height Canvas height in pixels
     */
    void set_canvas_size(int width, int height);

    /**
     * @brief Set vertical content offset (shifts render center up/down)
     * @param offset_percent Offset as percentage of canvas height (-1.0 to 1.0)
     *                       Negative = shift content up, Positive = shift down
     *
     * Use this to account for overlapping UI elements (e.g., metadata overlay at bottom).
     * A value of -0.1 shifts the render center up by 10% of canvas height.
     */
    void set_content_offset_y(float offset_percent);

    /// Tell the renderer how much of the canvas bottom the UI covers (0..1).
    /// Both the fit and the vertical shift depend on it, so changing it
    /// re-runs auto-fit rather than only nudging the existing framing.
    void set_bottom_occlusion(float occlusion);

    // =========================================================================
    // Display Options
    // =========================================================================

    /**
     * @brief Show/hide travel moves (default: OFF)
     * @param show true to show travel moves
     */
    void set_show_travels(bool show) {
        show_travels_.store(show, std::memory_order_relaxed);
    }

    /**
     * @brief Show/hide extrusion moves (default: ON)
     * @param show true to show extrusion moves
     */
    void set_show_extrusions(bool show) {
        show_extrusions_.store(show, std::memory_order_relaxed);
    }

    /**
     * @brief Show/hide support structures (default: ON, if detectable)
     * @param show true to show support structures
     */
    void set_show_supports(bool show) {
        show_supports_.store(show, std::memory_order_relaxed);
    }

    /** @brief Check if travel moves are shown */
    bool get_show_travels() const {
        return show_travels_.load(std::memory_order_relaxed);
    }

    /** @brief Check if support structures are shown */
    bool get_show_supports() const {
        return show_supports_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Enable/disable depth shading for 3D-like appearance (default: ON)
     *
     * When enabled in FRONT view:
     * - Lines are brighter at top, darker at bottom (simulates top-down lighting)
     * - Older layers slightly fade (focus on current print progress)
     *
     * @param enable true to enable depth shading
     */
    void set_depth_shading(bool enable) {
        depth_shading_.store(enable, std::memory_order_relaxed);
    }

    /** @brief Check if depth shading is enabled */
    bool get_depth_shading() const {
        return depth_shading_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Enable/disable screen-space ambient occlusion post-processing
     * @param enable true to enable SSAO (default: OFF)
     *
     * When enabled in FRONT view, applies a post-processing pass to the solid
     * cache buffer that darkens pixels in concavities (corners, crevices) for
     * improved depth perception. Only applied when the cache is fully rendered.
     *
     * Toggle via HELIX_SSAO=1 environment variable for testing.
     */
    void set_ssao_enabled(bool enable) {
        ssao_enabled_.store(enable, std::memory_order_relaxed);
        ssao_cache_valid_ = false;
        if (!enable) {
            // Release the full-canvas ARGB8888 scratch buffer; ensure_ssao_cache()
            // recreates it if SSAO is switched back on. Main-thread only (waits for
            // in-flight LVGL draw tasks) — all callers are widget/UI-thread code.
            destroy_ssao_cache();
        }
    }

    /** @brief Check if SSAO is enabled */
    bool get_ssao_enabled() const {
        return ssao_enabled_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Enable/disable ghost mode (shows faded preview of remaining layers)
     * @param enable true to enable ghost mode (default: ON)
     */
    void set_ghost_mode(bool enable) {
        ghost_mode_enabled_.store(enable, std::memory_order_relaxed);
    }

    /** @brief Check if ghost mode is enabled */
    bool get_ghost_mode() const {
        return ghost_mode_enabled_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get progress of streaming ghost build
     *
     * In streaming mode, ghost is built progressively in background.
     * Returns 0.0 to 1.0 indicating build progress.
     *
     * @return Progress fraction, or 1.0 if complete/not applicable
     */
    float get_ghost_build_progress() const;

    /**
     * @brief Check if streaming ghost build is complete
     * @return true if ghost is fully built (all layers rendered)
     */
    bool is_ghost_build_complete() const;

    /**
     * @brief Check if streaming ghost build is running
     * @return true if background ghost build is in progress
     */
    bool is_ghost_build_running() const;

    /// View mode alias — uses shared enum from gcode_projection.h
    using ViewMode = helix::gcode::ViewMode;

    /**
     * @brief Set view mode
     * @param mode View mode (TOP_DOWN, FRONT, or ISOMETRIC)
     */
    void set_view_mode(ViewMode mode) {
        view_mode_.store(static_cast<int>(mode), std::memory_order_relaxed);
        bounds_valid_ = false; // Recompute scale for new projection
    }

    /** @brief Get current view mode */
    ViewMode get_view_mode() const {
        return static_cast<ViewMode>(view_mode_.load(std::memory_order_relaxed));
    }

    // =========================================================================
    // Colors
    // =========================================================================

    /**
     * @brief Set extrusion color (overrides theme)
     * @param color Color for extrusion moves
     */
    void set_extrusion_color(lv_color_t color);

    /**
     * @brief Set travel color (overrides theme)
     * @param color Color for travel moves
     */
    void set_travel_color(lv_color_t color);

    /**
     * @brief Set support color (overrides theme)
     * @param color Color for support structures
     */
    void set_support_color(lv_color_t color);

    /**
     * @brief Set tool color palette for multi-color prints
     * @param hex_colors Vector of hex color strings (e.g., "#ED1C24")
     */
    void set_tool_color_palette(const std::vector<std::string>& hex_colors);

    /**
     * @brief Override per-tool colors with AMS slot colors
     * @param ams_colors Vector of RGB colors indexed by tool (0xRRGGBB)
     *
     * Replaces tool_palette_ entries with real AMS filament colors.
     * Colors resolve at render time per-segment — no rebuild needed.
     */
    void set_tool_color_overrides(const std::vector<uint32_t>& ams_colors);

    /**
     * @brief Reset all colors to theme defaults
     */
    void reset_colors();

    // =========================================================================
    // Object Selection & Exclusion
    // =========================================================================

    /**
     * @brief Set excluded objects (rendered with strikethrough style)
     * @param names Set of object names that are excluded from print
     */
    void set_excluded_objects(const std::unordered_set<std::string>& names);

    /**
     * @brief Set highlighted objects (rendered with selection highlight)
     * @param names Set of object names to highlight
     */
    void set_highlighted_objects(const std::unordered_set<std::string>& names);

    /**
     * @brief Pick the object under a screen coordinate
     * @param screen_x X in widget-local coordinates
     * @param screen_y Y in widget-local coordinates
     * @return Object name if found within threshold, nullopt otherwise
     */
    std::optional<std::string> pick_object_at(int screen_x, int screen_y) const;

    // =========================================================================
    // Viewport Control
    // =========================================================================

    /**
     * @brief Auto-fit all layers to canvas
     *
     * Computes scale and offset to fit the entire model's bounding box
     * within the canvas with 5% padding.
     */
    void auto_fit();

    /**
     * @brief Fit current layer to canvas
     *
     * Computes scale and offset to fit only the current layer's bounding box.
     */
    void fit_layer();

    /**
     * @brief Set zoom scale manually
     * @param scale Pixels per mm
     */
    void set_scale(float scale);

    /**
     * @brief Set viewport offset manually
     * @param x Center X in world coordinates
     * @param y Center Y in world coordinates
     */
    void set_offset(float x, float y);

    // =========================================================================
    // Layer Information
    // =========================================================================

    /**
     * @brief Information about the current layer
     */
    struct LayerInfo {
        int layer_number;       ///< Layer index (0-based)
        float z_height;         ///< Z-height in mm
        size_t segment_count;   ///< Total segments in layer
        size_t extrusion_count; ///< Number of extrusion segments
        size_t travel_count;    ///< Number of travel segments
        bool has_supports;      ///< True if layer contains support structures
    };

    /**
     * @brief Get information about current layer
     * @return LayerInfo struct with layer details
     */
    LayerInfo get_layer_info() const;

    /**
     * @brief Check if G-code has detectable support structures
     * @return true if supports can be identified
     */
    bool has_support_detection() const;

  private:
    // =========================================================================
    // Internal Rendering
    // =========================================================================

    /**
     * @brief Render a single segment
     * @param layer LVGL draw layer
     * @param seg Toolpath segment to render
     * @param ghost If true, render in ghost style (grey, for preview)
     */
    void render_segment(lv_layer_t* layer, const ToolpathSegment& seg, bool ghost = false);

    /**
     * @brief Render L-shaped corner brackets around highlighted objects' bounding boxes
     * @param layer LVGL draw layer
     *
     * Draws wireframe corner brackets at all 8 projected corners of each
     * highlighted object's AABB.
     */
    void render_selection_brackets(lv_layer_t* layer);

    /**
     * @brief Convert world coordinates to screen coordinates
     * @param x World X coordinate (mm)
     * @param y World Y coordinate (mm)
     * @param z World Z coordinate (mm) - used for FRONT view
     * @return Screen coordinates (pixels)
     */
    glm::ivec2 world_to_screen(float x, float y, float z = 0.0f) const;

    // =========================================================================
    // Transformation Parameters (for thread-safe coordinate conversion)
    // =========================================================================

    /// Transform params alias — uses shared struct from gcode_projection.h
    using TransformParams = helix::gcode::ProjectionParams;

    /**
     * @brief Capture current transformation parameters as a thread-safe snapshot
     *
     * The background ghost render thread calls this ONCE at thread start to
     * snapshot all transform state (scale, offset, canvas size, etc.). Each
     * field is a plain float/int read, and the main thread only modifies these
     * between render() calls, so no additional synchronization is needed.
     *
     * @return TransformParams struct with current values
     */
    TransformParams capture_transform_params() const;

    /**
     * @brief Convert world coordinates to screen using captured parameters
     *
     * Delegates to shared helix::gcode::project() — the single source of truth
     * for coordinate conversion across all renderers.
     *
     * @param params Captured transformation parameters
     * @param x World X coordinate (mm)
     * @param y World Y coordinate (mm)
     * @param z World Z coordinate (mm) - used for FRONT view
     * @return Screen coordinates (pixels, no widget offset applied)
     */
    static glm::ivec2 world_to_screen_raw(const TransformParams& params, float x, float y,
                                          float z = 0.0f) {
        return helix::gcode::project(params, x, y, z);
    }

    /**
     * @brief Check if a segment is a support structure
     * @param seg Segment to check
     * @return true if segment is part of support
     */
    bool is_support_segment(const ToolpathSegment& seg) const;

    /**
     * @brief Check if a segment should be rendered based on visibility settings
     * @param seg Segment to check
     * @return true if segment should be rendered
     */
    bool should_render_segment(const ToolpathSegment& seg) const;

    /**
     * @brief Get line color for a segment
     * @param seg Segment to get color for
     * @return LVGL color
     */
    lv_color_t get_segment_color(const ToolpathSegment& seg) const;

    /**
     * @brief Resolve object name index to string via current data source
     * @param index Object name index from ToolpathSegment
     * @return Resolved object name, or empty string if invalid
     */
    std::string resolve_object_name(int16_t index) const;

    // Data source (exactly one should be non-null)
    const ParsedGCodeFile* gcode_ = nullptr;
    GCodeStreamingController* streaming_controller_ = nullptr;
    int current_layer_ = 0;

    // Canvas dimensions
    int canvas_width_ = 400;
    int canvas_height_ = 400;
    float content_offset_y_percent_ = 0.0f; // Vertical content offset (-1.0 to 1.0)
    float bottom_occlusion_ = 0.0f;         // Fraction of canvas height covered by UI

    // Viewport transform (world → screen)
    float scale_ = 1.0f;
    float offset_x_ = 0.0f; // World-space center X
    float offset_y_ = 0.0f; // World-space center Y
    float offset_z_ = 0.0f; // World-space center Z (for FRONT view)

    // Display options (atomic: read by background ghost thread, written by main thread)
    std::atomic<bool> show_travels_{false};
    std::atomic<bool> show_extrusions_{true};
    std::atomic<bool> show_supports_{true};
    std::atomic<bool> depth_shading_{true}; // Enabled by default for 3D-like appearance
    std::atomic<bool> ssao_enabled_{true};  // Enhanced shading (normals, AA, outline)
    std::atomic<int> view_mode_{static_cast<int>(ViewMode::FRONT)}; // Default to front view

    // Colors
    lv_color_t color_extrusion_;
    lv_color_t color_travel_;
    lv_color_t color_support_;
    bool use_custom_extrusion_color_ = false;
    bool use_custom_travel_color_ = false;
    bool use_custom_support_color_ = false;
    GCodeColorPalette tool_palette_; ///< Per-tool colors for multi-color prints

    // Object exclusion/highlight state
    std::unordered_set<std::string> excluded_objects_;
    std::unordered_set<std::string> highlighted_objects_;

    // Cached bounds
    float bounds_min_x_ = 0.0f;
    float bounds_max_x_ = 0.0f;
    float bounds_min_y_ = 0.0f;
    float bounds_max_y_ = 0.0f;
    float bounds_min_z_ = 0.0f;
    float bounds_max_z_ = 0.0f;
    bool bounds_valid_ = false;

    // Widget screen offset (set during render())
    int widget_offset_x_ = 0;
    int widget_offset_y_ = 0;

    // Render statistics (for debugging)
    int last_rendered_layer_ = -1;
    uint32_t last_render_time_ms_ = 0;
    size_t last_segment_count_ = 0;

    // Incremental render cache - paint new layers on top of previous (SOLID)
    // Note: We only use draw buffers (no canvas widgets) to avoid clip area
    // contamination from overlays/toasts on lv_layer_top().
    lv_draw_buf_t* cache_buf_ = nullptr;
    int cached_up_to_layer_ = -1; // Highest layer rendered in cache
    int cached_width_ = 0;        // Dimensions cache was built for
    int cached_height_ = 0;

    // SSAO post-processing buffer - copy of cache with ambient occlusion applied
    lv_draw_buf_t* ssao_buf_ = nullptr;
    int ssao_cached_width_ = 0;
    int ssao_cached_height_ = 0;
    bool ssao_cache_valid_ = false;

    // Ghost cache - all layers rendered once at reduced opacity
    // Note: We only use draw buffers (no canvas widgets) to avoid clip area
    // contamination from overlays/toasts on lv_layer_top().
    lv_draw_buf_t* ghost_buf_ = nullptr;
    int ghost_cached_width_ = 0;
    int ghost_cached_height_ = 0;
    bool ghost_cache_valid_ = false;
    std::atomic<bool> ghost_mode_enabled_{true}; // Enable ghost mode by default
    int ghost_rendered_up_to_ = -1;              // Progress tracker for progressive ghost rendering

    // Progressive rendering - render N layers per frame to avoid blocking UI
    // These are defaults; actual values come from config or adaptive adjustment
    static constexpr int DEFAULT_LAYERS_PER_FRAME = 15;
    static constexpr int MIN_LAYERS_PER_FRAME = 1;
    static constexpr int MAX_LAYERS_PER_FRAME = 100;
    static constexpr int DEFAULT_ADAPTIVE_TARGET_MS = 16; // ~60 FPS

    // Constrained device limits (AD5M, low-RAM embedded < 256MB)
    static constexpr int CONSTRAINED_START_LPF = 5;
    static constexpr int CONSTRAINED_MAX_LPF = 15;
    static constexpr float CONSTRAINED_GROWTH_CAP = 1.3f; // vs 2.0f normal

    int layers_per_frame_{DEFAULT_LAYERS_PER_FRAME}; ///< Current layers per frame (may be adaptive)
    int config_layers_per_frame_{0};                 ///< Config value (0 = adaptive)
    int adaptive_target_ms_{DEFAULT_ADAPTIVE_TARGET_MS}; ///< Target render time for adaptive mode
    uint32_t last_frame_render_ms_{0}; ///< Render time of last frame (for adaptive)

    // Device-aware limits
    bool is_constrained_device_{false};              ///< True if device has < 256MB RAM
    int max_layers_per_frame_{MAX_LAYERS_PER_FRAME}; ///< Device-adjusted max (15 on constrained)

    // Warm-up frames: skip heavy rendering for first N frames to let panel layout complete
    static constexpr int WARMUP_FRAMES = 2;
    int warmup_frames_remaining_{WARMUP_FRAMES}; ///< Countdown before heavy render starts

    /// Load config values from settings.json
    void load_config();

    /// Adjust layers_per_frame based on last render time (when config_layers_per_frame_ == 0)
    void adapt_layers_per_frame();

    void invalidate_cache();
    void ensure_cache(int width, int height);
    /// Render [from_layer, to_layer] into cache_buf_. Returns the highest layer
    /// successfully rendered. In streaming mode, a layer that returns null
    /// segments (load failed or cache miss with failed I/O) stops the run so
    /// the next frame retries from that layer — silently advancing past a
    /// missing layer would leave a permanent gap in the printed render.
    int render_layers_to_cache(int from_layer, int to_layer);
    void blit_cache(lv_layer_t* target);
    void destroy_cache();

    // SSAO post-processing
    void ensure_ssao_cache(int width, int height);
    void apply_ssao();
    void blit_ssao_cache(lv_layer_t* target);
    void destroy_ssao_cache();

    // Ghost cache methods (LVGL-based, for main thread progressive rendering)
    void ensure_ghost_cache(int width, int height);
    void render_ghost_layers(int from_layer, int to_layer);
    void blit_ghost_cache(lv_layer_t* target);
    void destroy_ghost_cache();

    // =========================================================================
    // Background Thread Ghost Rendering
    // =========================================================================
    // LVGL is not thread-safe, so background thread renders to a raw pixel
    // buffer using software Bresenham line drawing, then copies to LVGL buffer
    // on main thread when complete.

    /// Raw pixel buffer for background thread rendering (ARGB8888)
    std::unique_ptr<uint8_t[]> ghost_raw_buffer_;
    int ghost_raw_width_ = 0;
    int ghost_raw_height_ = 0;
    size_t ghost_raw_stride_ = 0; // Bytes per row

    /// Background thread management (works for both streaming and non-streaming modes)
    std::thread ghost_thread_;
    std::atomic<bool> ghost_thread_cancel_{false};
    std::atomic<bool> ghost_thread_running_{false};
    std::atomic<bool> ghost_thread_ready_{false}; // True when raw buffer is complete

    /// Start background ghost rendering (called when new gcode loaded)
    void start_background_ghost_render();

    /// Cancel any in-progress background ghost render
    void cancel_background_ghost_render();

    /// Background thread entry point (renders all layers to raw buffer)
    void background_ghost_render_thread();

    /// Copy completed raw buffer to LVGL ghost_buf_ (called on main thread)
    void copy_raw_to_ghost_buf();

    /// Software Bresenham line drawing to raw ARGB8888 buffer (ghost)
    void draw_line_bresenham(int x0, int y0, int x1, int y1, uint32_t color);

    /// Thick line drawing using parallel Bresenham lines (ghost buffer)
    void draw_thick_line_bresenham(int x0, int y0, int x1, int y1, uint32_t color, int width);

    /// Blend a pixel with alpha into the ghost raw buffer
    void blend_pixel(int x, int y, uint32_t color);

    /// Software Bresenham line drawing to solid cache buffer
    /// Used for solid layer rendering, bypassing LVGL draw API for AD5M compatibility
    void draw_line_bresenham_solid(int x0, int y0, int x1, int y1, uint32_t color);

    /// Thick line drawing using parallel Bresenham lines (solid cache)
    void draw_thick_line_bresenham_solid(int x0, int y0, int x1, int y1, uint32_t color, int width);

    /// Blend a pixel directly into the solid LVGL cache buffer
    void blend_pixel_solid(int x, int y, uint32_t color);

    /// Blend a pixel with coverage (0-255) into solid cache buffer (for AA)
    void blend_pixel_solid_alpha(int x, int y, uint32_t color, uint8_t coverage);

    /// Anti-aliased line drawing (Wu's algorithm) to solid cache buffer
    void draw_line_aa_solid(int x0, int y0, int x1, int y1, uint32_t color);

    /// Thick anti-aliased line drawing (parallel Wu's lines) to solid cache
    void draw_thick_line_aa_solid(int x0, int y0, int x1, int y1, uint32_t color, int width);

    /// Compute line width in pixels from extrusion width metadata and current scale
    int get_extrusion_pixel_width() const;
};

} // namespace gcode
} // namespace helix
