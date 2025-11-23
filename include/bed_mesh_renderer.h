// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef BED_MESH_RENDERER_H
#define BED_MESH_RENDERER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

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
 * HelixScreen architecture. See docs/GUPPYSCREEN_BEDMESH_ANALYSIS.md for
 * algorithm details.
 *
 * Performance target: 20×20 mesh at 30+ FPS on embedded hardware
 * Rendering complexity: O(n log n) for sorting + O(pixels) for rasterization
 */

// Rendering configuration constants
#define BED_MESH_SCALE 50.0            // Base spacing between mesh points (world units)
#define BED_MESH_CAMERA_DISTANCE 450.0 // Virtual camera distance (moderate perspective: ~33% depth)
#define BED_MESH_CAMERA_ZOOM_LEVEL                                                                 \
    1.176 // Default zoom (1.0 = neutral, <1.0 = zoomed out, >1.0 = zoomed in)
#define BED_MESH_DEFAULT_Z_SCALE 60.0         // Default height amplification factor
#define BED_MESH_DEFAULT_Z_TARGET_HEIGHT 80.0 // Target projected height range (world units)
#define BED_MESH_MIN_Z_SCALE 35.0             // Min Z scale (prevents flatness)
#define BED_MESH_MAX_Z_SCALE 120.0            // Max Z scale (prevents extreme projection)
#define BED_MESH_COLOR_COMPRESSION 0.8        // Color range compression (0.8 = 80% of data range)
#define BED_MESH_Z_ORIGIN_VERTICAL_POS                                                             \
    0.5 // Canvas Y position for Z=0 plane (0=top, 0.5=center, 1=bottom)
#define BED_MESH_GRADIENT_SEGMENTS 6       // Max gradient segments per scanline
#define BED_MESH_GRADIENT_MIN_LINE_WIDTH 3 // Use solid color for lines narrower than this

// 3D point in world space after perspective projection
typedef struct {
    double x, y, z;         // 3D world coordinates
    int screen_x, screen_y; // 2D screen coordinates after projection
    double depth;           // Z-depth from camera (for sorting)
} bed_mesh_point_3d_t;

// 3D vertex with color information
typedef struct {
    double x, y, z;   // 3D position in world space
    lv_color_t color; // Vertex color for gradient interpolation
} bed_mesh_vertex_3d_t;

// Quad surface (4 vertices) representing one mesh cell
typedef struct {
    bed_mesh_vertex_3d_t vertices[4]; // Four corners in WORLD space: [0]=BL, [1]=BR, [2]=TL, [3]=TR

    // Cached screen-space projections (computed once per frame, reused for rendering)
    int screen_x[4];  // Screen X coordinates for vertices[0..3]
    int screen_y[4];  // Screen Y coordinates for vertices[0..3]
    double depths[4]; // Z-depths for vertices[0..3] (for sorting/debugging)

    double avg_depth;        // Average depth for back-to-front sorting (computed from depths[])
    lv_color_t center_color; // Fallback solid color for fast rendering (drag mode)
} bed_mesh_quad_3d_t;

// View/camera state for interactive rotation
typedef struct {
    double angle_x;   // Tilt angle (up/down rotation in degrees)
    double angle_z;   // Spin angle (horizontal rotation in degrees)
    double z_scale;   // Height amplification multiplier
    double fov_scale; // Perspective field-of-view scale
    bool is_dragging; // True during interactive drag (use fast rendering)

    // Cached trigonometric values (computed once per angle change)
    double cached_cos_x;   // cos(angle_x in radians)
    double cached_sin_x;   // sin(angle_x in radians)
    double cached_cos_z;   // cos(angle_z in radians)
    double cached_sin_z;   // sin(angle_z in radians)
    bool trig_cache_valid; // True if cached values match current angles

    // Centering offsets (computed after scaling to fit canvas)
    int center_offset_x; // Horizontal centering offset in screen pixels
    int center_offset_y; // Vertical centering offset in screen pixels
} bed_mesh_view_state_t;

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
 * @brief Set camera rotation angles
 *
 * @param renderer Renderer instance
 * @param angle_x Tilt angle in degrees (typically -85 to -10, negative = looking down)
 * @param angle_z Spin angle in degrees (horizontal rotation around vertical axis)
 */
void bed_mesh_renderer_set_rotation(bed_mesh_renderer_t* renderer, double angle_x, double angle_z);

/**
 * @brief Get current view state (for interactive controls)
 *
 * @param renderer Renderer instance
 * @return Pointer to internal view state, or NULL if renderer is NULL
 */
const bed_mesh_view_state_t* bed_mesh_renderer_get_view_state(bed_mesh_renderer_t* renderer);

/**
 * @brief Set view state (for interactive controls)
 *
 * @param renderer Renderer instance
 * @param state New view state to apply
 */
void bed_mesh_renderer_set_view_state(bed_mesh_renderer_t* renderer,
                                      const bed_mesh_view_state_t* state);

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
 * @brief Set Z-scale multiplier (height amplification)
 *
 * @param renderer Renderer instance
 * @param z_scale Height multiplier (clamped to BED_MESH_MIN_Z_SCALE .. BED_MESH_MAX_Z_SCALE)
 */
void bed_mesh_renderer_set_z_scale(bed_mesh_renderer_t* renderer, double z_scale);

/**
 * @brief Set FOV scale (perspective zoom)
 *
 * @param renderer Renderer instance
 * @param fov_scale Field-of-view scale multiplier
 */
void bed_mesh_renderer_set_fov_scale(bed_mesh_renderer_t* renderer, double fov_scale);

/**
 * @brief Set explicit color range for height mapping
 *
 * By default, renderer auto-scales colors based on mesh data min/max.
 * Call this to override with explicit range.
 *
 * @param renderer Renderer instance
 * @param min_z Minimum Z value (maps to purple/blue colors)
 * @param max_z Maximum Z value (maps to yellow/red colors)
 */
void bed_mesh_renderer_set_color_range(bed_mesh_renderer_t* renderer, double min_z, double max_z);

/**
 * @brief Enable auto-scaling of color range (default)
 *
 * Automatically computes color range from mesh data min/max values.
 *
 * @param renderer Renderer instance
 */
void bed_mesh_renderer_auto_color_range(bed_mesh_renderer_t* renderer);

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
 * @return true on success, false on error (NULL pointers, no mesh data)
 */
bool bed_mesh_renderer_render(bed_mesh_renderer_t* renderer, lv_layer_t* layer, int canvas_width,
                              int canvas_height);

#ifdef __cplusplus
}
#endif

#endif // BED_MESH_RENDERER_H
