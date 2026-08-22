// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
namespace helix {

/**
 * @brief Render mode for bed mesh visualization
 *
 * Controls whether 3D perspective or 2D heatmap rendering is used.
 */
enum class BedMeshRenderMode {
    Auto,    ///< Automatically choose based on measured FPS
    Force3D, ///< Always use 3D perspective (may be slow)
    Force2D  ///< Always use 2D heatmap (fast)
};

} // namespace helix

extern "C" {
#endif

#include <stdbool.h>

/**
 * @file bed_mesh_renderer.h
 * @brief 3D bed mesh visualization renderer using LVGL canvas
 *
 * Implements complete 3D rendering pipeline for printer bed mesh height maps:
 * - Perspective projection with interactive rotation
 *
 * COORDINATE SPACES (transformation pipeline):
 *
 * 1. MESH SPACE (input):
 *    - Indices: row ∈ [0, rows-1], col ∈ [0, cols-1]
 *    - Heights: Z ∈ [mesh_min_z, mesh_max_z] (millimeters from bed)
 *    - Origin: mesh[0][0] = front-left corner
 *
 * 2. WORLD SPACE (3D scene):
 *    - Coordinates: (X, Y, Z) in world units (scaled by BED_MESH_SCALE)
 *    - Origin: Center of mesh at (0, 0, Z_center)
 *    - X-axis: Left (negative) to right (positive)
 *    - Y-axis: Front (positive) to back (negative) [inverted from mesh rows]
 *    - Z-axis: Down (negative) to up (positive)
 *    - Transform: mesh_*_to_world_*() helpers (src/bed_mesh_renderer.cpp)
 *
 * 3. CAMERA SPACE (after rotation):
 *    - After applying angle_x (tilt) and angle_z (spin) rotations
 *    - Camera positioned at (0, 0, -CAMERA_DISTANCE) looking toward origin
 *    - Computed internally in project_3d_to_2d()
 *
 * 4. SCREEN SPACE (2D pixels, FINAL OUTPUT):
 *    - Coordinates: (screen_x, screen_y) in pixels
 *    - Origin: Top-left corner of canvas/layer at (0, 0)
 *    - After perspective projection + center_offset_x/y
 *    - All rendering uses screen space coordinates
 *
 * IMPORTANT NAMING CONVENTION:
 * - Functions accepting "x, y, z" parameters expect WORLD SPACE coordinates
 * - Functions returning/storing "screen_x, screen_y" provide SCREEN SPACE coordinates
 * - Cached coordinates in structs (e.g., quad.screen_x[]) are always SCREEN SPACE
 *
 * LAYER OFFSET HANDLING:
 * - center_offset_x/y: Converts mesh-centered coords to layer-centered coords
 * - Accounts for overlay panel position on screen (e.g., panel at x=136)
 * - Calculated once on first render, stable across rotations
 * - Scanline triangle rasterization with gradient interpolation
 * - Painter's algorithm depth sorting
 * - Scientific heat-map color mapping (purple → blue → cyan → yellow → red)
 *
 * Based on GuppyScreen's bed mesh visualization with adaptations for
 * HelixScreen architecture. See docs/devel/BED_MESH_RENDERING_INTERNALS.md
 * for algorithm details.
 *
 * Performance target: 20×20 mesh at 30+ FPS on embedded hardware
 * Rendering complexity: O(n log n) for sorting + O(pixels) for rasterization
 */

// Rendering configuration constants
#define BED_MESH_SCALE 50.0                // Base spacing between mesh points (world units)
#define BED_MESH_PERSPECTIVE_STRENGTH 0.13 // 0.0 = orthographic, 1.0 = max perspective distortion

// Default camera angles (Mainsail-style)
// Standard 3D camera conventions:
//   angle_x (pitch): 0° = horizontal (edge-on), -90° = top-down, positive = looking up
//   angle_z (yaw):   0° = front view, negative = clockwise rotation from above
#define BED_MESH_DEFAULT_ANGLE_X                                                                   \
    (-25.0) // 25° down from horizontal (very shallow Mainsail-like view)
#define BED_MESH_DEFAULT_ANGLE_Z (-45.0) // 45° clockwise rotation (Mainsail-like)

// Rotation limits (pitch angle range)
#define BED_MESH_ANGLE_X_MIN (-89.0)          // Near top-down (looking almost straight down)
#define BED_MESH_ANGLE_X_MAX (0.0)            // Edge-on view (horizontal)
#define BED_MESH_DEFAULT_Z_SCALE 60.0         // Default height amplification factor
#define BED_MESH_DEFAULT_Z_TARGET_HEIGHT 80.0 // Target projected height range (world units)
#define BED_MESH_MIN_Z_SCALE 35.0             // Min Z scale (prevents flatness)
#define BED_MESH_MAX_Z_SCALE 120.0            // Max Z scale (prevents extreme projection)
#define BED_MESH_COLOR_COMPRESSION 0.8        // Color range compression (0.8 = 80% of data range)
#define BED_MESH_Z_ORIGIN_VERTICAL_POS                                                             \
    0.5 // Canvas Y position for Z=0 plane (0=top, 0.5=center, 1=bottom)
#define BED_MESH_GRADIENT_SEGMENTS 6       // Max gradient segments per scanline
#define BED_MESH_GRADIENT_MIN_LINE_WIDTH 3 // Use solid color for lines narrower than this
#define BED_MESH_GRID_MARGIN 25.0          // Grid margin in world units (extends past mesh edges)

// FPS threshold for auto-degrading to 2D mode
#define BED_MESH_FPS_THRESHOLD 15.0 // Switch to 2D if FPS drops below this
#define BED_MESH_FPS_WINDOW_SIZE 10 // Rolling window for FPS averaging

// 3D point in world space after perspective projection
struct bed_mesh_point_3d_t {
    double x, y, z;         // 3D world coordinates
    int screen_x, screen_y; // 2D screen coordinates after projection
    double depth;           // Z-depth from camera (for sorting)
};

// 3D vertex with color information
struct bed_mesh_vertex_3d_t {
    double x, y, z;   // 3D position in world space
    lv_color_t color; // Vertex color for gradient interpolation
};

// Quad surface (4 vertices) representing one mesh cell
struct bed_mesh_quad_3d_t {
    bed_mesh_vertex_3d_t vertices[4]; // Four corners in WORLD space: [0]=BL, [1]=BR, [2]=TL, [3]=TR

    // Cached screen-space projections (computed once per frame, reused for rendering)
    int screen_x[4];  // Screen X coordinates for vertices[0..3]
    int screen_y[4];  // Screen Y coordinates for vertices[0..3]
    double depths[4]; // Z-depths for vertices[0..3] (for sorting/debugging)

    double avg_depth;        // Average depth for back-to-front sorting (computed from depths[])
    lv_color_t center_color; // Fallback solid color for fast rendering (drag mode)
    lv_opa_t opacity;        // Quad opacity (LV_OPA_COVER for mesh, lower for zero plane)
};

// View/camera state for interactive rotation
struct bed_mesh_view_state_t {
    double angle_x;         // Tilt angle (up/down rotation in degrees)
    double angle_z;         // Spin angle (horizontal rotation in degrees)
    double z_scale;         // Height amplification multiplier
    double fov_scale;       // Perspective field-of-view scale
    double camera_distance; // Computed from mesh size and perspective strength
    bool is_dragging;       // True during interactive drag (use fast rendering)

    // Cached trigonometric values (computed once per angle change)
    double cached_cos_x;   // cos(angle_x in radians)
    double cached_sin_x;   // sin(angle_x in radians)
    double cached_cos_z;   // cos(angle_z in radians)
    double cached_sin_z;   // sin(angle_z in radians)
    bool trig_cache_valid; // True if cached values match current angles

    // Centering offsets (computed after scaling to fit canvas, canvas-relative)
    int center_offset_x; // Horizontal centering offset in canvas pixels
    int center_offset_y; // Vertical centering offset in canvas pixels

    // Layer offset (updated every frame to track panel position during animations)
    int layer_offset_x; // Layer's X position on screen (from clip area)
    int layer_offset_y; // Layer's Y position on screen (from clip area)
};

// Main renderer instance (opaque handle)
typedef struct bed_mesh_renderer bed_mesh_renderer_t;

/**
 * @brief Create a new bed mesh renderer
 *
 * @return Pointer to renderer instance, or NULL on allocation failure
 */
bed_mesh_renderer_t* bed_mesh_renderer_create(void);

/**
 * @brief Destroy renderer and free all resources
 *
 * @param renderer Renderer instance to destroy (can be NULL)
 */
void bed_mesh_renderer_destroy(bed_mesh_renderer_t* renderer);

/**
 * @brief Set mesh height data
 *
 * Copies the mesh data into internal storage. Mesh layout is row-major:
 * - mesh[row][col] where row = Y-axis (front to back)
 * - col = X-axis (left to right)
 * - values are absolute Z heights from printer bed
 *
 * @param renderer Renderer instance
 * @param mesh 2D array of height values (row-major)
 * @param rows Number of rows in mesh
 * @param cols Number of columns in mesh
 * @return true on success, false on error (NULL pointer, invalid dimensions)
 */
bool bed_mesh_renderer_set_mesh_data(bed_mesh_renderer_t* renderer, const float* const* mesh,
                                     int rows, int cols);

/**
 * @brief Check if the renderer has mesh data loaded
 *
 * @param renderer Renderer instance
 * @return true if mesh data has been set via set_mesh_data(), false otherwise
 */
bool bed_mesh_renderer_has_data(const bed_mesh_renderer_t* renderer);

/**
 * @brief Set coordinate bounds for bed and mesh
 *
 * The bed bounds define the full print bed area (used for grid/walls).
 * The mesh bounds define where probing occurred (mesh is rendered within these).
 *
 * Call this AFTER set_mesh_data() to position the mesh correctly within the bed.
 * If not called, mesh bounds are used for both (legacy behavior).
 *
 * @param renderer Renderer instance
 * @param bed_x_min Full bed minimum X coordinate
 * @param bed_x_max Full bed maximum X coordinate
 * @param bed_y_min Full bed minimum Y coordinate
 * @param bed_y_max Full bed maximum Y coordinate
 * @param mesh_x_min Mesh probe area minimum X coordinate
 * @param mesh_x_max Mesh probe area maximum X coordinate
 * @param mesh_y_min Mesh probe area minimum Y coordinate
 * @param mesh_y_max Mesh probe area maximum Y coordinate
 */
void bed_mesh_renderer_set_bounds(bed_mesh_renderer_t* renderer, double bed_x_min, double bed_x_max,
                                  double bed_y_min, double bed_y_max, double mesh_x_min,
                                  double mesh_x_max, double mesh_y_min, double mesh_y_max);

/**
 * @brief Set camera rotation angles
 *
 * The renderer owns the camera orientation: angle_x is clamped to
 * [BED_MESH_ANGLE_X_MIN, BED_MESH_ANGLE_X_MAX] and angle_z is normalized to
 * [0, 360). Callers driving an interactive orbit should read the current angles
 * from bed_mesh_renderer_get_view_state() and pass back the accumulated values
 * rather than tracking their own copy.
 *
 * @param renderer Renderer instance
 * @param angle_x Tilt angle in degrees (negative = looking down; clamped)
 * @param angle_z Spin angle in degrees (horizontal rotation around vertical axis; wrapped)
 */
void bed_mesh_renderer_set_rotation(bed_mesh_renderer_t* renderer, double angle_x, double angle_z);

/**
 * @brief Get current view state (for interactive controls)
 *
 * The returned pointer aliases the renderer's live state — it stays valid for
 * the lifetime of the renderer and reflects later mutations (e.g. a subsequent
 * bed_mesh_renderer_set_rotation() call).
 *
 * @param renderer Renderer instance
 * @return Pointer to internal view state, or NULL if renderer is NULL
 */
const bed_mesh_view_state_t* bed_mesh_renderer_get_view_state(bed_mesh_renderer_t* renderer);

/**
 * @brief Set dragging state (affects rendering quality)
 *
 * During drag, uses solid colors for faster rendering.
 * When static, uses gradient interpolation for higher quality.
 *
 * @param renderer Renderer instance
 * @param is_dragging true if user is currently dragging (fast render), false otherwise
 */
void bed_mesh_renderer_set_dragging(bed_mesh_renderer_t* renderer, bool is_dragging);

/**
 * @brief Set the layer offset used for screen-space positioning
 *
 * In async (buffer) rendering the mesh is rendered at (0,0).  Overlay
 * elements drawn on the LVGL layer (axis labels, tick marks) need the
 * widget's actual screen position.  Call this before rendering overlays
 * in async mode so projected 3D→2D coordinates land at the correct
 * screen location.
 *
 * @param renderer Renderer instance
 * @param offset_x Widget's absolute screen X position
 * @param offset_y Widget's absolute screen Y position
 */
void bed_mesh_renderer_set_layer_offset(bed_mesh_renderer_t* renderer, int offset_x, int offset_y);

/**
 * @brief Get the current layer offset
 *
 * @param renderer Renderer instance
 * @param[out] offset_x Current X offset (NULL-safe)
 * @param[out] offset_y Current Y offset (NULL-safe)
 */
void bed_mesh_renderer_get_layer_offset(const bed_mesh_renderer_t* renderer, int* offset_x,
                                        int* offset_y);

/**
 * @brief Main rendering function
 *
 * Renders the 3D bed mesh to the provided LVGL layer (DRAW_POST pattern).
 *
 * Rendering pipeline:
 * 1. Clear background
 * 2. Compute projection parameters (Z-scale, FOV-scale)
 * 3. Generate 3D quads from mesh data with colors
 * 4. Project quads to 2D screen space
 * 5. Sort quads by depth (painter's algorithm)
 * 6. Render quads (gradient or solid based on dragging state)
 *
 * @param renderer Renderer instance
 * @param layer LVGL draw layer (from DRAW_POST event callback)
 * @param canvas_width Viewport width in pixels
 * @param canvas_height Viewport height in pixels
 * @param widget_x Widget's absolute screen X position (from lv_obj_get_coords)
 * @param widget_y Widget's absolute screen Y position (from lv_obj_get_coords)
 * @return true on success, false on error (NULL pointers, no mesh data)
 */
bool bed_mesh_renderer_render(bed_mesh_renderer_t* renderer, lv_layer_t* layer, int canvas_width,
                              int canvas_height, int widget_x, int widget_y);

/**
 * @brief Set render mode (auto, force 3D, or force 2D)
 *
 * In AUTO mode, the renderer tracks FPS and automatically switches to 2D
 * heatmap mode if frame rate drops below BED_MESH_FPS_THRESHOLD.
 *
 * @param renderer Renderer instance
 * @param mode Desired render mode
 */
void bed_mesh_renderer_set_render_mode(bed_mesh_renderer_t* renderer,
                                       helix::BedMeshRenderMode mode);

/**
 * @brief Get current render mode setting
 *
 * @param renderer Renderer instance
 * @return Current render mode (AUTO, FORCE_3D, or FORCE_2D)
 */
helix::BedMeshRenderMode bed_mesh_renderer_get_render_mode(bed_mesh_renderer_t* renderer);

/**
 * @brief Check if currently using 2D fallback mode
 *
 * Returns true if the renderer is currently using 2D heatmap mode, either
 * because it auto-degraded due to low FPS or because FORCE_2D is set.
 *
 * @param renderer Renderer instance
 * @return true if rendering in 2D mode, false if rendering in 3D mode
 */
bool bed_mesh_renderer_is_using_2d(bed_mesh_renderer_t* renderer);

/**
 * @brief Evaluate render mode based on measured FPS
 *
 * Call this ONCE when the bed mesh panel is opened (not during viewing).
 * In AUTO mode, checks if FPS is below threshold and sets 2D fallback flag.
 * Mode is then locked for the duration of panel viewing.
 *
 * @param renderer Renderer instance
 */
void bed_mesh_renderer_evaluate_render_mode(bed_mesh_renderer_t* renderer);

/**
 * @brief Handle touch event in 2D mode
 *
 * When in 2D heatmap mode, converts touch coordinates to mesh cell and
 * stores the cell info for tooltip display. Call this on touch/press events.
 *
 * @param renderer Renderer instance
 * @param touch_x Touch X coordinate (screen space, relative to canvas)
 * @param touch_y Touch Y coordinate (screen space, relative to canvas)
 * @param canvas_width Current canvas width
 * @param canvas_height Current canvas height
 * @return true if touch hit a valid cell, false otherwise
 */
bool bed_mesh_renderer_handle_touch(bed_mesh_renderer_t* renderer, int touch_x, int touch_y,
                                    int canvas_width, int canvas_height);

/**
 * @brief Clear touched cell state
 *
 * Call this on touch release to clear the tooltip.
 *
 * @param renderer Renderer instance
 */
void bed_mesh_renderer_clear_touch(bed_mesh_renderer_t* renderer);

/**
 * @brief Show or hide the zero reference plane
 *
 * The zero plane is a translucent reference surface at Z=0 (or Z-offset)
 * that intersects the mesh, showing where the nozzle touches the bed.
 * Parts of the mesh above the plane obscure it; parts below are visible
 * through the translucent plane.
 *
 * @param renderer Renderer instance
 * @param visible true to show the zero plane, false to hide it
 */
void bed_mesh_renderer_set_zero_plane_visible(bed_mesh_renderer_t* renderer, bool visible);

/**
 * @brief Set Z display offset for axis labels
 *
 * When mesh data is normalized (mean-subtracted), this offset is added back
 * to Z values in axis labels so they show the original probe heights.
 *
 * @param renderer Renderer instance
 * @param offset_mm The mean Z value subtracted during normalization
 */
void bed_mesh_renderer_set_z_display_offset(bed_mesh_renderer_t* renderer, double offset_mm);

#ifdef __cplusplus
}

// C++ only: buffer-based rendering for background thread
namespace helix::mesh {
class PixelBuffer;
}

/**
 * @brief Colors needed for off-screen buffer rendering
 *
 * Must be populated on the main thread (where theme_manager_get_color is safe)
 * and passed to render_to_buffer which runs on a background thread.
 */
struct bed_mesh_render_colors_t {
    uint8_t bg_r, bg_g, bg_b;       ///< Background clear color (graph_bg)
    uint8_t grid_r, grid_g, grid_b; ///< Grid/wireframe line color (elevated_bg)
};

/**
 * @brief Render full mesh into an off-screen pixel buffer
 *
 * Thread-safe: does NOT call any lv_* functions. Runs the full 3D rendering
 * pipeline (projection, sorting, rasterization, overlays) into a PixelBuffer.
 * The buffer should already be allocated at the correct size.
 *
 * Does NOT handle 2D heatmap fallback (stays on main thread).
 * Does NOT call lv_obj_invalidate() or any LVGL function.
 *
 * @param renderer Renderer instance with valid mesh data
 * @param buffer   Target pixel buffer (must be allocated at canvas dimensions)
 * @param colors   Pre-fetched theme colors (from main thread)
 * @return true if rendering succeeded, false on error
 */
bool bed_mesh_renderer_render_to_buffer(bed_mesh_renderer_t* renderer,
                                        helix::mesh::PixelBuffer& buffer,
                                        const bed_mesh_render_colors_t& colors);

#endif
