// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "bed_mesh_renderer.h"

#include "bed_grid.h"
#include "bed_mesh_buffer.h"
#include "bed_mesh_coordinate_transform.h"
#include "bed_mesh_geometry.h"
#include "bed_mesh_gradient.h"
#include "bed_mesh_internal.h"
#include "bed_mesh_overlays.h"
#include "bed_mesh_projection.h"
#include "bed_mesh_rasterizer.h"
#include "memory_monitor.h"

#include <spdlog/spdlog.h>

using namespace helix;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// Constants
// ============================================================================

namespace {

// Use the default angles from the public header (bed_mesh_renderer.h)
// This ensures consistency between the renderer and any code that reads those constants

// Canvas rendering
constexpr double kCanvasPaddingFactor = 0.98; // Margin for axis labels and tick marks at edges
constexpr double kInitialFovScale = 150.0;    // Starting point for auto-scale (gets adjusted)

} // anonymous namespace

// ============================================================================
// Helper Function Forward Declarations
// ============================================================================
static void compute_mesh_bounds(bed_mesh_renderer_t* renderer);
static double compute_dynamic_z_scale(double z_range);
static void update_trig_cache(bed_mesh_view_state_t* view_state);
static void project_and_cache_vertices(bed_mesh_renderer_t* renderer, int canvas_width,
                                       int canvas_height);
static void project_and_cache_quads(bed_mesh_renderer_t* renderer, int canvas_width,
                                    int canvas_height);
static void compute_projected_mesh_bounds(const bed_mesh_renderer_t* renderer, int* out_min_x,
                                          int* out_max_x, int* out_min_y, int* out_max_y);
static void compute_centering_offset(int mesh_min_x, int mesh_max_x, int mesh_min_y, int mesh_max_y,
                                     int layer_offset_x, int layer_offset_y, int canvas_width,
                                     int canvas_height, int* out_offset_x, int* out_offset_y);
static void calibrate_fov_scale(bed_mesh_renderer_t* renderer, int canvas_width, int canvas_height);
static void compute_initial_centering(bed_mesh_renderer_t* renderer, int canvas_width,
                                      int canvas_height, int layer_offset_x, int layer_offset_y);
static void prepare_render_frame(bed_mesh_renderer_t* renderer, int canvas_width, int canvas_height,
                                 int layer_offset_x, int layer_offset_y);

static void render_quad_to_buffer(helix::mesh::PixelBuffer& buf, const bed_mesh_quad_3d_t& quad,
                                  bool use_gradient);
static void render_mesh_surface_to_buffer(helix::mesh::PixelBuffer& buf,
                                          bed_mesh_renderer_t* renderer, int canvas_width,
                                          int canvas_height);
static void render_decorations_to_buffer(helix::mesh::PixelBuffer& buf,
                                         bed_mesh_renderer_t* renderer, int canvas_width,
                                         int canvas_height, uint8_t grid_r, uint8_t grid_g,
                                         uint8_t grid_b);

// Adaptive render mode and 2D heatmap
static void record_frame_time(bed_mesh_renderer_t* renderer, float frame_ms);
static float calculate_average_fps(const bed_mesh_renderer_t* renderer);
static bool is_fps_below_threshold(const bed_mesh_renderer_t* renderer, float min_fps);
static void render_2d_heatmap_to_buffer(helix::mesh::PixelBuffer& buf,
                                        const bed_mesh_renderer_t* renderer);

// ============================================================================
// Public API Implementation
// ============================================================================

bed_mesh_renderer_t* bed_mesh_renderer_create(void) {
    bed_mesh_renderer_t* renderer = new (std::nothrow) bed_mesh_renderer_t;
    if (!renderer) {
        spdlog::error("[Bed Mesh Renderer] Failed to allocate bed mesh renderer");
        return nullptr;
    }

    renderer->state = RendererState::UNINITIALIZED;

    renderer->rows = 0;
    renderer->cols = 0;
    renderer->mesh_min_z = 0.0;
    renderer->mesh_max_z = 0.0;
    renderer->has_mesh_data = false;

    renderer->auto_color_range = true;
    renderer->color_min_z = 0.0;
    renderer->color_max_z = 0.0;

    // Initialize bed bounds (will be set via set_bed_bounds)
    renderer->bed_min_x = 0.0;
    renderer->bed_min_y = 0.0;
    renderer->bed_max_x = 0.0;
    renderer->bed_max_y = 0.0;

    // Initialize mesh bounds (probe area, will be set via set_bounds)
    renderer->mesh_area_min_x = 0.0;
    renderer->mesh_area_min_y = 0.0;
    renderer->mesh_area_max_x = 0.0;
    renderer->mesh_area_max_y = 0.0;
    renderer->has_mesh_bounds = false;

    renderer->bed_center_x = 0.0;
    renderer->bed_center_y = 0.0;
    renderer->coord_scale = 1.0;
    renderer->geometry_computed = false;
    renderer->circular_bed = false;

    // Default view state (Mainsail-style: looking from front-right toward back-left)
    renderer->view_state.angle_x = BED_MESH_DEFAULT_ANGLE_X;
    renderer->view_state.angle_z = BED_MESH_DEFAULT_ANGLE_Z;
    renderer->view_state.z_scale = BED_MESH_DEFAULT_Z_SCALE;
    renderer->view_state.fov_scale = kInitialFovScale;
    renderer->view_state.camera_distance = 1000.0; // Default, recomputed when mesh data is set
    renderer->view_state.is_dragging = false;

    // Initialize trig cache as invalid (will be computed on first render)
    renderer->view_state.trig_cache_valid = false;
    renderer->view_state.cached_cos_x = 0.0;
    renderer->view_state.cached_sin_x = 0.0;
    renderer->view_state.cached_cos_z = 0.0;
    renderer->view_state.cached_sin_z = 0.0;

    // Initialize centering offsets to zero (will be computed after projection)
    renderer->view_state.center_offset_x = 0;
    renderer->view_state.center_offset_y = 0;

    // Initialize layer offsets to zero (updated every frame during render)
    renderer->view_state.layer_offset_x = 0;
    renderer->view_state.layer_offset_y = 0;

    bed_mesh_projection_reset_zoom(&renderer->view_state);

    spdlog::debug("[Bed Mesh Renderer] Created bed mesh renderer");
    return renderer;
}

void bed_mesh_renderer_destroy(bed_mesh_renderer_t* renderer) {
    if (!renderer) {
        return;
    }

    spdlog::debug("[Bed Mesh Renderer] Destroying bed mesh renderer");
    delete renderer;
}

bool bed_mesh_renderer_set_mesh_data(bed_mesh_renderer_t* renderer, const float* const* mesh,
                                     int rows, int cols) {
    if (!renderer || !mesh || rows <= 0 || cols <= 0) {
        spdlog::error("[Bed Mesh Renderer] Invalid parameters for set_mesh_data: renderer={}, "
                      "mesh={}, rows={}, cols={}",
                      (void*)renderer, (void*)mesh, rows, cols);
        if (renderer) {
            renderer->state = RendererState::ERROR;
        }
        return false;
    }

    spdlog::debug("[Bed Mesh Renderer] Setting mesh data: {}x{} points", rows, cols);

    renderer->mesh.clear();
    renderer->mesh.resize(static_cast<size_t>(rows));
    for (int row = 0; row < rows; row++) {
        renderer->mesh[static_cast<size_t>(row)].resize(static_cast<size_t>(cols));
        for (int col = 0; col < cols; col++) {
            renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col)] =
                static_cast<double>(mesh[row][col]);
        }
    }

    renderer->rows = rows;
    renderer->cols = cols;
    renderer->has_mesh_data = true;
    helix::MemoryMonitor::log_now("bed_mesh_data_set");

    compute_mesh_bounds(renderer);

    if (renderer->auto_color_range) {
        renderer->color_min_z = renderer->mesh_min_z;
        renderer->color_max_z = renderer->mesh_max_z;
    }

    spdlog::debug("[Bed Mesh Renderer] Mesh bounds: min_z={:.3f}, max_z={:.3f}, range={:.3f}",
                  renderer->mesh_min_z, renderer->mesh_max_z,
                  renderer->mesh_max_z - renderer->mesh_min_z);

    // Compute camera distance from mesh size and perspective strength
    // Formula: camera_distance = mesh_diagonal / perspective_strength
    // Where 0 = orthographic (very far), 1 = max perspective (close)
    double mesh_width = (cols - 1) * BED_MESH_SCALE;
    double mesh_height = (rows - 1) * BED_MESH_SCALE;
    double mesh_diagonal = std::sqrt(mesh_width * mesh_width + mesh_height * mesh_height);

    if (BED_MESH_PERSPECTIVE_STRENGTH > 0.001) {
        renderer->view_state.camera_distance = mesh_diagonal / BED_MESH_PERSPECTIVE_STRENGTH;
    } else {
        // Near-orthographic: very far camera
        renderer->view_state.camera_distance = mesh_diagonal * 100.0;
    }
    spdlog::debug(
        "[Bed Mesh Renderer] Camera distance: {:.1f} (mesh_diagonal={:.1f}, perspective={:.2f})",
        renderer->view_state.camera_distance, mesh_diagonal, BED_MESH_PERSPECTIVE_STRENGTH);

    // Pre-generate geometry quads: constant for this mesh data, so they are
    // regenerated only on a data change, never per frame
    spdlog::debug("[MESH_DATA] Initial quad generation with z_scale={:.2f}",
                  renderer->view_state.z_scale);
    helix::mesh::generate_mesh_quads(renderer);
    spdlog::debug("[Bed Mesh Renderer] Pre-generated {} quads from mesh data",
                  renderer->quads.size());
    helix::MemoryMonitor::log_now("bed_mesh_quads_done");

    // State transition: UNINITIALIZED or READY_TO_RENDER → MESH_LOADED
    renderer->state = RendererState::MESH_LOADED;

    return true;
}

bool bed_mesh_renderer_has_data(const bed_mesh_renderer_t* renderer) {
    return renderer && renderer->has_mesh_data;
}

void bed_mesh_renderer_set_rotation(bed_mesh_renderer_t* renderer, double angle_x, double angle_z) {
    if (!renderer) {
        return;
    }

    // Tilt is clamped to the supported pitch range; beyond it the camera passes
    // through the bed plane and the painter's-algorithm depth sort inverts.
    angle_x = std::clamp(angle_x, BED_MESH_ANGLE_X_MIN, BED_MESH_ANGLE_X_MAX);

    // Spin is periodic - normalize to [0, 360) so repeated orbiting cannot walk
    // the stored angle off toward large magnitudes.
    angle_z = std::fmod(angle_z, 360.0);
    if (angle_z < 0.0) {
        angle_z += 360.0;
    }

    renderer->view_state.angle_x = angle_x;
    renderer->view_state.angle_z = angle_z;

    // Rotation changes invalidate cached projections (READY_TO_RENDER → MESH_LOADED)
    if (renderer->state == RendererState::READY_TO_RENDER) {
        renderer->state = RendererState::MESH_LOADED;
    }
}

// NAMESPACE_OK: joins bed_mesh_renderer_set_rotation, this file's global-scope C API
void bed_mesh_renderer_apply_two_finger(bed_mesh_renderer_t* renderer, double pan_dx, double pan_dy,
                                        double zoom, double anchor_x, double anchor_y,
                                        int canvas_width, int canvas_height) {
    if (!renderer) {
        return;
    }
    bed_mesh_projection_pan(&renderer->view_state, pan_dx, pan_dy);
    bed_mesh_projection_zoom_at(&renderer->view_state, zoom, anchor_x, anchor_y, canvas_width,
                                canvas_height);
    // Zoom and pan change every projected vertex (READY_TO_RENDER -> MESH_LOADED)
    if (renderer->state == RendererState::READY_TO_RENDER) {
        renderer->state = RendererState::MESH_LOADED;
    }
}

void bed_mesh_renderer_set_bounds(bed_mesh_renderer_t* renderer, double bed_x_min, double bed_x_max,
                                  double bed_y_min, double bed_y_max, double mesh_x_min,
                                  double mesh_x_max, double mesh_y_min, double mesh_y_max,
                                  bool circular_bed) {
    if (!renderer) {
        return;
    }
    renderer->circular_bed = circular_bed;

    // Set bed bounds (full print bed area - used for grid/walls)
    renderer->bed_min_x = bed_x_min;
    renderer->bed_max_x = bed_x_max;
    renderer->bed_min_y = bed_y_min;
    renderer->bed_max_y = bed_y_max;

    // Set mesh bounds (probe area - used for positioning mesh surface within bed)
    renderer->mesh_area_min_x = mesh_x_min;
    renderer->mesh_area_max_x = mesh_x_max;
    renderer->mesh_area_min_y = mesh_y_min;
    renderer->mesh_area_max_y = mesh_y_max;
    renderer->has_mesh_bounds = true;

    // Compute derived geometry parameters
    renderer->bed_center_x = (bed_x_min + bed_x_max) / 2.0;
    renderer->bed_center_y = (bed_y_min + bed_y_max) / 2.0;

    // Compute scale factor: normalize larger bed dimension to target world size
    // Target world size matches the BED_MESH_SCALE-based index sizing (~200 world units)
    constexpr double kTargetWorldSize = 200.0;
    double bed_size_x = bed_x_max - bed_x_min;
    double bed_size_y = bed_y_max - bed_y_min;
    double larger_dimension = std::max(bed_size_x, bed_size_y);
    renderer->coord_scale =
        helix::mesh::compute_bed_scale_factor(larger_dimension, kTargetWorldSize);
    renderer->geometry_computed = true;

    spdlog::debug("[Bed Mesh Renderer] Set bounds: bed [{:.1f}, {:.1f}] x [{:.1f}, {:.1f}], mesh "
                  "[{:.1f}, {:.1f}] x "
                  "[{:.1f}, {:.1f}], center=({:.1f}, {:.1f}), scale={:.4f}",
                  bed_x_min, bed_x_max, bed_y_min, bed_y_max, mesh_x_min, mesh_x_max, mesh_y_min,
                  mesh_y_max, renderer->bed_center_x, renderer->bed_center_y,
                  renderer->coord_scale);

    // The auto-fit that follows projects through the magnify, so it must see the fitted view.
    bed_mesh_projection_reset_zoom(&renderer->view_state);

    // Reset FOV scale and centering to trigger auto-calibration on next render
    // This ensures the view zooms to fit the new bed bounds
    renderer->view_state.fov_scale = kInitialFovScale;
    renderer->view_state.center_offset_x = 0;
    renderer->view_state.center_offset_y = 0;
    renderer->initial_centering_computed = false;

    // Bounds changes require regenerating quads with new coord_scale and centers
    if (renderer->state == RendererState::READY_TO_RENDER ||
        renderer->state == RendererState::MESH_LOADED) {
        // Regenerate quads with new coordinate transform
        helix::mesh::generate_mesh_quads(renderer);
        renderer->state = RendererState::MESH_LOADED;
    }
}

const bed_mesh_view_state_t* bed_mesh_renderer_get_view_state(bed_mesh_renderer_t* renderer) {
    if (!renderer) {
        return nullptr;
    }
    return &renderer->view_state;
}

void bed_mesh_renderer_set_dragging(bed_mesh_renderer_t* renderer, bool is_dragging) {
    if (!renderer) {
        return;
    }
    renderer->view_state.is_dragging = is_dragging;
}

// ============================================================================
// Rendering (no LVGL calls - runs on the render thread)
// ============================================================================

bool bed_mesh_renderer_render_to_buffer(bed_mesh_renderer_t* renderer,
                                        helix::mesh::PixelBuffer& buffer,
                                        const bed_mesh_render_colors_t& colors) {
    if (!renderer) {
        spdlog::error("[Bed Mesh Renderer] render_to_buffer: NULL renderer");
        return false;
    }

    if (renderer->state == RendererState::UNINITIALIZED) {
        spdlog::debug("[Bed Mesh Renderer] render_to_buffer: no mesh data (UNINITIALIZED)");
        return false;
    }
    if (renderer->state == RendererState::ERROR) {
        spdlog::error("[Bed Mesh Renderer] render_to_buffer: renderer in ERROR state");
        return false;
    }
    if (!renderer->has_mesh_data) {
        spdlog::debug("[Bed Mesh Renderer] render_to_buffer: no mesh data");
        return false;
    }

    int canvas_width = buffer.width();
    int canvas_height = buffer.height();
    if (canvas_width <= 0 || canvas_height <= 0) {
        spdlog::debug("[Bed Mesh Renderer] render_to_buffer: invalid dimensions {}x{}",
                      canvas_width, canvas_height);
        return false;
    }

    spdlog::debug("[Bed Mesh Renderer] render_to_buffer: {}x{} (dragging={})", canvas_width,
                  canvas_height, renderer->view_state.is_dragging);

    // Step 1: Clear buffer with background color
    buffer.clear(colors.bg_r, colors.bg_g, colors.bg_b, 255);

    auto t_frame_start = std::chrono::high_resolution_clock::now();

    buffer.info.heatmap = bed_mesh_renderer_is_using_2d(renderer);
    buffer.info.rows = renderer->rows;
    buffer.info.cols = renderer->cols;
    if (buffer.info.heatmap) {
        render_2d_heatmap_to_buffer(buffer, renderer);
        auto ms_total = std::chrono::duration<double, std::milli>(
                            std::chrono::high_resolution_clock::now() - t_frame_start)
                            .count();
        record_frame_time(renderer, static_cast<float>(ms_total));
        spdlog::trace("[Bed Mesh Renderer] [2D] Heatmap render: {:.2f}ms (FPS: {:.1f})", ms_total,
                      calculate_average_fps(renderer));
        return true;
    }

    // Step 2: Prepare render frame (pure math, no LVGL calls)
    // For buffer rendering, layer offset is (0,0) since we render into local widget space
    prepare_render_frame(renderer, canvas_width, canvas_height, 0, 0);
    auto t_prepare = std::chrono::high_resolution_clock::now();

    // Step 3: Render reference grids FIRST (behind mesh)
    helix::mesh::render_reference_grids(buffer, renderer, canvas_width, canvas_height,
                                        colors.grid_r, colors.grid_g, colors.grid_b);

    // Step 4: Render mesh surface (quads with gradient/solid colors)
    render_mesh_surface_to_buffer(buffer, renderer, canvas_width, canvas_height);
    auto t_surface = std::chrono::high_resolution_clock::now();

    // Step 5: Render overlay decorations (grid lines on top of mesh)
    render_decorations_to_buffer(buffer, renderer, canvas_width, canvas_height, colors.grid_r,
                                 colors.grid_g, colors.grid_b);
    auto t_decorations = std::chrono::high_resolution_clock::now();

    // Record frame time for FPS tracking
    auto ms_prepare = std::chrono::duration<double, std::milli>(t_prepare - t_frame_start).count();
    auto ms_surface = std::chrono::duration<double, std::milli>(t_surface - t_prepare).count();
    auto ms_decorations =
        std::chrono::duration<double, std::milli>(t_decorations - t_surface).count();
    auto ms_total =
        std::chrono::duration<double, std::milli>(t_decorations - t_frame_start).count();

    record_frame_time(renderer, static_cast<float>(ms_total));

    spdlog::trace("[Bed Mesh Renderer] [PERF] Buffer render: {:.2f}ms | Prepare: {:.2f}ms | "
                  "Surface: {:.2f}ms | Decorations: {:.2f}ms | FPS: {:.1f}",
                  ms_total, ms_prepare, ms_surface, ms_decorations,
                  calculate_average_fps(renderer));

    if (renderer->state == RendererState::MESH_LOADED) {
        renderer->state = RendererState::READY_TO_RENDER;
    }

    return true;
}

static void compute_mesh_bounds(bed_mesh_renderer_t* renderer) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    double min_z = renderer->mesh[0][0];
    double max_z = renderer->mesh[0][0];

    for (int row = 0; row < renderer->rows; row++) {
        for (int col = 0; col < renderer->cols; col++) {
            double z = renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col)];
            if (z < min_z)
                min_z = z;
            if (z > max_z)
                max_z = z;
        }
    }

    renderer->mesh_min_z = min_z;
    renderer->mesh_max_z = max_z;
    // Cache z_center to avoid repeated computation (computed once per mesh data change)
    renderer->cached_z_center = helix::mesh::compute_mesh_z_center(min_z, max_z);
}

static double compute_dynamic_z_scale(double z_range) {
    // Compute scale to amplify Z range to target height
    double z_scale = BED_MESH_DEFAULT_Z_TARGET_HEIGHT / z_range;

    z_scale = std::max(BED_MESH_MIN_Z_SCALE, std::min(BED_MESH_MAX_Z_SCALE, z_scale));

    return z_scale;
}

/**
 * Update cached trigonometric values when angles change
 * Call this once per frame before projection loop to eliminate redundant trig computations
 * @param view_state Mutable view state to update (const-cast required)
 */
static inline void update_trig_cache(bed_mesh_view_state_t* view_state) {
    // Angle conversion for looking DOWN at the bed from above:
    // - angle_x uses +90° offset so user's -90° = top-down, -45° = tilted view
    // - angle_z is used directly (negative = clockwise from above)
    //
    // Convention:
    //   angle_x = -90° → top-down view (internal 0°)
    //   angle_x = -45° → 45° tilt from top-down (internal 45°)
    //   angle_x = 0°   → edge-on view (internal 90°)
    //   angle_z = 0°   → front view
    //   angle_z = -45° → rotated 45° clockwise (from above)
    double x_angle_rad = (view_state->angle_x + 90.0) * M_PI / 180.0;
    double z_angle_rad = view_state->angle_z * M_PI / 180.0;

    view_state->cached_cos_x = std::cos(x_angle_rad);
    view_state->cached_sin_x = std::sin(x_angle_rad);
    view_state->cached_cos_z = std::cos(z_angle_rad);
    view_state->cached_sin_z = std::sin(z_angle_rad);
    view_state->trig_cache_valid = true;
}

/**
 * Project all mesh vertices to screen space and cache for reuse
 * Avoids redundant projections in grid/axis rendering (15-20% speedup)
 * @param renderer Renderer with mesh data
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 */
static void project_and_cache_vertices(bed_mesh_renderer_t* renderer, int canvas_width,
                                       int canvas_height) {
    if (!renderer || !renderer->has_mesh_data) {
        return;
    }

    // Resize SOA caches if needed (avoid reallocation on every frame)
    if (renderer->projected_screen_x.size() != static_cast<size_t>(renderer->rows)) {
        renderer->projected_screen_x.resize(static_cast<size_t>(renderer->rows));
        renderer->projected_screen_y.resize(static_cast<size_t>(renderer->rows));
    }

    // Use cached z_center (computed once in compute_mesh_bounds)

    // Project all vertices once (projection handles centering internally)
    for (int row = 0; row < renderer->rows; row++) {
        if (renderer->projected_screen_x[static_cast<size_t>(row)].size() !=
            static_cast<size_t>(renderer->cols)) {
            renderer->projected_screen_x[static_cast<size_t>(row)].resize(
                static_cast<size_t>(renderer->cols));
            renderer->projected_screen_y[static_cast<size_t>(row)].resize(
                static_cast<size_t>(renderer->cols));
        }

        for (int col = 0; col < renderer->cols; col++) {
            // Convert mesh coordinates to world space
            double world_x, world_y;

            if (renderer->geometry_computed) {
                // Mainsail-style: Position mesh within bed using mesh_area bounds
                double cols_minus_1 = static_cast<double>(renderer->cols - 1);
                double rows_minus_1 = static_cast<double>(renderer->rows - 1);

                double printer_x =
                    renderer->mesh_area_min_x +
                    col / cols_minus_1 * (renderer->mesh_area_max_x - renderer->mesh_area_min_x);
                double printer_y =
                    renderer->mesh_area_min_y +
                    row / rows_minus_1 * (renderer->mesh_area_max_y - renderer->mesh_area_min_y);

                world_x = helix::mesh::printer_x_to_world_x(printer_x, renderer->bed_center_x,
                                                            renderer->coord_scale);
                world_y = helix::mesh::printer_y_to_world_y(printer_y, renderer->bed_center_y,
                                                            renderer->coord_scale);
            } else {
                // Bed geometry not computed: index-based coordinates
                world_x = helix::mesh::mesh_col_to_world_x(col, renderer->cols, BED_MESH_SCALE);
                world_y = helix::mesh::mesh_row_to_world_y(row, renderer->rows, BED_MESH_SCALE);
            }

            double world_z = helix::mesh::mesh_z_to_world_z(
                renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col)],
                renderer->cached_z_center, renderer->view_state.z_scale);

            // Project to screen space and cache only screen coordinates (SOA)
            bed_mesh_point_3d_t projected = bed_mesh_projection_project_3d_to_2d(
                world_x, world_y, world_z, canvas_width, canvas_height, &renderer->view_state);

            renderer->projected_screen_x[static_cast<size_t>(row)][static_cast<size_t>(col)] =
                projected.screen_x;
            renderer->projected_screen_y[static_cast<size_t>(row)][static_cast<size_t>(col)] =
                projected.screen_y;

            // DEBUG: Log sample point (center of mesh)
            if (row == renderer->rows / 2 && col == renderer->cols / 2) {
                spdlog::debug("[Bed Mesh Renderer] [GRID_VERTEX] mesh[{},{}] -> "
                              "world({:.2f},{:.2f},{:.2f}) -> screen({},{})",
                              row, col, world_x, world_y, world_z, projected.screen_x,
                              projected.screen_y);
            }
        }
    }
}

/**
 * @brief Project all quad vertices to screen space and cache results
 *
 * Computes screen coordinates and depths for all vertices of all quads in a single pass.
 * Depth sorting, bounds tracking and rendering all read this cache, so each quad
 * is projected once per view change instead of three times.
 *
 * Must be called whenever view state changes (rotation, FOV, centering offset).
 *
 * @param renderer Renderer with quads already generated
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 *
 * Side effects:
 * - Updates quad.screen_x[], quad.screen_y[], quad.depths[] for all quads
 * - Updates quad.avg_depth for depth sorting
 */
static void project_and_cache_quads(bed_mesh_renderer_t* renderer, int canvas_width,
                                    int canvas_height) {
    if (!renderer || renderer->quads.empty()) {
        return;
    }

    for (auto& quad : renderer->quads) {
        double total_depth = 0.0;

        for (int i = 0; i < 4; i++) {
            bed_mesh_point_3d_t projected = bed_mesh_projection_project_3d_to_2d(
                quad.vertices[i].x, quad.vertices[i].y, quad.vertices[i].z, canvas_width,
                canvas_height, &renderer->view_state);

            quad.screen_x[i] = projected.screen_x;
            quad.screen_y[i] = projected.screen_y;
            quad.depths[i] = projected.depth;
            total_depth += projected.depth;
        }

        quad.avg_depth = total_depth / 4.0;
    }

    // DEBUG: Log a sample quad vertex (TL of center quad corresponds to mesh center)
    // For an NxN grid, center quad is at index ((N-1)/2 * (N-1) + (N-1)/2)
    if (!renderer->quads.empty()) {
        int center_row = (renderer->rows - 1) / 2;
        int center_col = (renderer->cols - 1) / 2;
        size_t center_quad_idx =
            static_cast<size_t>(center_row * (renderer->cols - 1) + center_col);
        if (center_quad_idx < renderer->quads.size()) {
            const auto& q = renderer->quads[center_quad_idx];
            // TL vertex (index 2) corresponds to mesh[row][col]
            spdlog::debug("[Bed Mesh Renderer] [QUAD_VERTEX] quad[{}] TL -> "
                          "world({:.2f},{:.2f},{:.2f}) -> screen({},{})",
                          center_quad_idx, q.vertices[2].x, q.vertices[2].y, q.vertices[2].z,
                          q.screen_x[2], q.screen_y[2]);
        }
    }

    spdlog::trace("[Bed Mesh Renderer] [CACHE] Projected {} quads to screen space",
                  renderer->quads.size());
}

/**
 * @brief Compute 2D bounding box of projected mesh points
 *
 * Scans all cached projected_points to find min/max X and Y coordinates in screen space.
 * Used for FOV scaling and centering calculations.
 *
 * @param renderer Renderer with projected_points cache populated
 * @param[out] out_min_x Minimum screen X coordinate
 * @param[out] out_max_x Maximum screen X coordinate
 * @param[out] out_min_y Minimum screen Y coordinate
 * @param[out] out_max_y Maximum screen Y coordinate
 */
static void compute_projected_mesh_bounds(const bed_mesh_renderer_t* renderer, int* out_min_x,
                                          int* out_max_x, int* out_min_y, int* out_max_y) {
    if (!renderer || !renderer->has_mesh_data) {
        *out_min_x = *out_max_x = *out_min_y = *out_max_y = 0;
        return;
    }

    int min_x = INT_MAX, max_x = INT_MIN;
    int min_y = INT_MAX, max_y = INT_MIN;

    for (int row = 0; row < renderer->rows; row++) {
        for (int col = 0; col < renderer->cols; col++) {
            int screen_x =
                renderer->projected_screen_x[static_cast<size_t>(row)][static_cast<size_t>(col)];
            int screen_y =
                renderer->projected_screen_y[static_cast<size_t>(row)][static_cast<size_t>(col)];
            min_x = std::min(min_x, screen_x);
            max_x = std::max(max_x, screen_x);
            min_y = std::min(min_y, screen_y);
            max_y = std::max(max_y, screen_y);
        }
    }

    *out_min_x = min_x;
    *out_max_x = max_x;
    *out_min_y = min_y;
    *out_max_y = max_y;
}

/**
 * @brief Compute centering offset to center mesh in layer
 *
 * Compares mesh bounding box center (in screen space) to layer center
 * (in screen space) and returns offset needed to align them.
 *
 * COORDINATE SPACE: All inputs and outputs are in SCREEN SPACE (absolute pixels).
 *
 * @param mesh_min_x Minimum projected mesh X (screen space)
 * @param mesh_max_x Maximum projected mesh X (screen space)
 * @param mesh_min_y Minimum projected mesh Y (screen space)
 * @param mesh_max_y Maximum projected mesh Y (screen space)
 * @param layer_offset_x Layer's screen position X (from clip_area->x1)
 * @param layer_offset_y Layer's screen position Y (from clip_area->y1)
 * @param canvas_width Layer width in pixels
 * @param canvas_height Layer height in pixels
 * @param[out] out_offset_x Horizontal centering offset
 * @param[out] out_offset_y Vertical centering offset
 */
static void compute_centering_offset(int mesh_min_x, int mesh_max_x, int mesh_min_y, int mesh_max_y,
                                     int /*layer_offset_x*/, int /*layer_offset_y*/,
                                     int canvas_width, int canvas_height, int* out_offset_x,
                                     int* out_offset_y) {
    // Calculate centers in CANVAS space (not screen space)
    // The mesh bounds are relative to canvas origin, so we center within the canvas
    // Layer offset is handled separately in projection to support animations
    int mesh_center_x = (mesh_min_x + mesh_max_x) / 2;
    int mesh_center_y = (mesh_min_y + mesh_max_y) / 2;
    int canvas_center_x = canvas_width / 2;
    int canvas_center_y = canvas_height / 2;

    // Offset needed to move mesh center to canvas center (canvas-relative coords)
    *out_offset_x = canvas_center_x - mesh_center_x;
    *out_offset_y = canvas_center_y - mesh_center_y;

    spdlog::debug("[Bed Mesh Renderer] [CENTERING] Mesh center: ({},{}) -> Canvas center: ({},{}) "
                  "= offset ({},{})",
                  mesh_center_x, mesh_center_y, canvas_center_x, canvas_center_y, *out_offset_x,
                  *out_offset_y);
}

/**
 * @brief Calibrate FOV scale to fit mesh and walls within canvas bounds
 *
 * Computes a scale factor that ensures the projected mesh and reference walls
 * fit within the canvas with appropriate padding. Only runs on first render
 * (when fov_scale equals kInitialFovScale).
 *
 * @param renderer Renderer instance with mesh data
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 */
static void calibrate_fov_scale(bed_mesh_renderer_t* renderer, int canvas_width,
                                int canvas_height) {
    // Project all mesh vertices with initial scale to get actual bounds
    project_and_cache_vertices(renderer, canvas_width, canvas_height);

    int min_x, max_x, min_y, max_y;
    compute_projected_mesh_bounds(renderer, &min_x, &max_x, &min_y, &max_y);

    // ALSO include wall corners in bounds calculation, so walls that extend above
    // the mesh are not clipped. Same extent render_reference_grids() draws.
    const auto ext = helix::mesh::compute_bed_extent(renderer);
    const double bed_half_width = ext.half_width;
    const double bed_half_height = ext.half_height;
    double wall_z_max = ext.walls.ceiling_z;
    double wall_z_floor = ext.walls.floor_z;

    // Project wall/floor corners and expand bounds
    // Include grid margin to account for tick label positions
    double x_min = -bed_half_width - BED_MESH_GRID_MARGIN;
    double x_max = bed_half_width + BED_MESH_GRID_MARGIN;
    double y_min = -bed_half_height - BED_MESH_GRID_MARGIN;
    double y_max = bed_half_height + BED_MESH_GRID_MARGIN;

    // Project all 8 corners (4 at ceiling, 4 at floor) to get full bounds
    bed_mesh_point_3d_t corners[8] = {
        // Ceiling corners (top of walls)
        bed_mesh_projection_project_3d_to_2d(x_min, y_min, wall_z_max, canvas_width, canvas_height,
                                             &renderer->view_state),
        bed_mesh_projection_project_3d_to_2d(x_max, y_min, wall_z_max, canvas_width, canvas_height,
                                             &renderer->view_state),
        bed_mesh_projection_project_3d_to_2d(x_min, y_max, wall_z_max, canvas_width, canvas_height,
                                             &renderer->view_state),
        bed_mesh_projection_project_3d_to_2d(x_max, y_max, wall_z_max, canvas_width, canvas_height,
                                             &renderer->view_state),
        // Floor corners (where tick labels are drawn)
        bed_mesh_projection_project_3d_to_2d(x_min, y_min, wall_z_floor, canvas_width,
                                             canvas_height, &renderer->view_state),
        bed_mesh_projection_project_3d_to_2d(x_max, y_min, wall_z_floor, canvas_width,
                                             canvas_height, &renderer->view_state),
        bed_mesh_projection_project_3d_to_2d(x_min, y_max, wall_z_floor, canvas_width,
                                             canvas_height, &renderer->view_state),
        bed_mesh_projection_project_3d_to_2d(x_max, y_max, wall_z_floor, canvas_width,
                                             canvas_height, &renderer->view_state),
    };
    for (const auto& corner : corners) {
        min_x = std::min(min_x, corner.screen_x);
        max_x = std::max(max_x, corner.screen_x);
        min_y = std::min(min_y, corner.screen_y);
        max_y = std::max(max_y, corner.screen_y);
    }

    // Calculate scale needed to fit projected bounds into canvas
    int projected_width = max_x - min_x;
    int projected_height = max_y - min_y;
    double scale_x = (canvas_width * kCanvasPaddingFactor) / projected_width;
    double scale_y = (canvas_height * kCanvasPaddingFactor) / projected_height;
    double scale_factor = std::min(scale_x, scale_y);

    spdlog::info("[Bed Mesh Renderer] [FOV] Canvas: {}x{}, Projected (incl walls): {}x{}, "
                 "Padding: {:.2f}, Scale: {:.2f}",
                 canvas_width, canvas_height, projected_width, projected_height,
                 kCanvasPaddingFactor, scale_factor);

    // Apply scale (only once, not every frame)
    renderer->view_state.fov_scale *= scale_factor;
    spdlog::info("[Bed Mesh Renderer] [FOV] Final fov_scale: {:.2f} (initial {} * scale {:.2f})",
                 renderer->view_state.fov_scale, kInitialFovScale, scale_factor);
}

/**
 * @brief Compute initial centering offset for mesh in canvas
 *
 * Calculates the offset needed to center the projected mesh within the canvas.
 * Only runs on first render (when center_offset_x/y are both 0).
 *
 * @param renderer Renderer instance with mesh data
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 * @param layer_offset_x Layer's screen position X (from clip area)
 * @param layer_offset_y Layer's screen position Y (from clip area)
 */
static void compute_initial_centering(bed_mesh_renderer_t* renderer, int canvas_width,
                                      int canvas_height, int layer_offset_x, int layer_offset_y) {
    // Compute bounds with current projection
    int inner_min_x, inner_max_x, inner_min_y, inner_max_y;
    compute_projected_mesh_bounds(renderer, &inner_min_x, &inner_max_x, &inner_min_y, &inner_max_y);

    compute_centering_offset(inner_min_x, inner_max_x, inner_min_y, inner_max_y, layer_offset_x,
                             layer_offset_y, canvas_width, canvas_height,
                             &renderer->view_state.center_offset_x,
                             &renderer->view_state.center_offset_y);

    spdlog::debug("[Bed Mesh Renderer] [CENTER] Computed centering offset: ({}, {})",
                  renderer->view_state.center_offset_x, renderer->view_state.center_offset_y);
}

/**
 * @brief Prepare rendering frame - compute projection parameters and update view state
 *
 * Performs one-time and per-frame preparation:
 * - Dynamic Z scale calculation (if mesh is too flat/tall)
 * - Trig cache update (avoids recomputing sin/cos for every vertex)
 * - FOV scaling on first render (prevents grow/shrink during rotation)
 * - Centering offset on first render (keeps mesh centered during rotation)
 *
 * @param renderer Renderer instance
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 * @param layer_offset_x Layer's screen position X (from clip area)
 * @param layer_offset_y Layer's screen position Y (from clip area)
 */
static void prepare_render_frame(bed_mesh_renderer_t* renderer, int canvas_width, int canvas_height,
                                 int layer_offset_x, int layer_offset_y) {
    // Compute dynamic Z scale if needed
    double z_range = renderer->mesh_max_z - renderer->mesh_min_z;
    double new_z_scale;
    if (z_range < 1e-6) {
        // Flat mesh, use default scale
        new_z_scale = BED_MESH_DEFAULT_Z_SCALE;
    } else {
        // Compute dynamic scale to fit mesh in reasonable height
        new_z_scale = compute_dynamic_z_scale(z_range);
    }

    if (renderer->view_state.z_scale != new_z_scale) {
        spdlog::debug(
            "[Bed Mesh Renderer] [Z_SCALE] Changing z_scale from {:.2f} to {:.2f} (z_range={:.4f})",
            renderer->view_state.z_scale, new_z_scale, z_range);
        renderer->view_state.z_scale = new_z_scale;
        helix::mesh::generate_mesh_quads(renderer);
        spdlog::debug(
            "[Bed Mesh Renderer] Regenerated quads due to dynamic z_scale change to {:.2f}",
            new_z_scale);
    } else {
        spdlog::debug("[Bed Mesh Renderer] [Z_SCALE] Keeping z_scale at {:.2f} (z_range={:.4f})",
                      renderer->view_state.z_scale, z_range);
    }

    // Update cached trigonometric values (avoids recomputing sin/cos for every vertex)
    update_trig_cache(&renderer->view_state);

    // Compute FOV scale ONCE on first render (when fov_scale is still at default)
    // This prevents grow/shrink effect when rotating - scale stays constant
    if (renderer->view_state.fov_scale == kInitialFovScale) {
        calibrate_fov_scale(renderer, canvas_width, canvas_height);
    }

    // Project vertices with current (stable) fov_scale
    // IMPORTANT: Project with layer_offset=0 to get canvas-relative coordinates for centering
    renderer->view_state.layer_offset_x = 0;
    renderer->view_state.layer_offset_y = 0;
    project_and_cache_vertices(renderer, canvas_width, canvas_height);

    // Center mesh once on first render
    // Use dedicated flag instead of checking offset==(0,0) since (0,0) can be a valid computed
    // offset
    if (!renderer->initial_centering_computed) {
        compute_initial_centering(renderer, canvas_width, canvas_height, layer_offset_x,
                                  layer_offset_y);
        renderer->initial_centering_computed = true;
    }

    // Apply layer offset for final rendering (updated every frame for animation support)
    // IMPORTANT: Must set BEFORE projecting vertices/quads so both use the same offsets!
    renderer->view_state.layer_offset_x = layer_offset_x;
    renderer->view_state.layer_offset_y = layer_offset_y;

    // Re-project grid vertices with final view state (fov_scale, centering, AND layer offset)
    // This ensures grid lines and quads are projected with identical view parameters
    project_and_cache_vertices(renderer, canvas_width, canvas_height);
}

// ============================================================================
// Quad, surface and decorations
// ============================================================================

/**
 * @brief Render a single quad into a pixel buffer using cached screen coordinates
 *
 * For translucent quads (zero plane), renders as two solid-color triangles.
 * For opaque mesh quads, uses gradient or solid fill via buffer rasterizer.
 */
static void render_quad_to_buffer(helix::mesh::PixelBuffer& buf, const bed_mesh_quad_3d_t& quad,
                                  bool use_gradient) {
    lv_opa_t opacity = quad.opacity;

    // For both translucent and opaque quads, render as 2 triangles
    // (PixelBuffer doesn't have polygon fill, but triangle seams are acceptable
    // for translucent quads in off-screen rendering)
    if (use_gradient && opacity == LV_OPA_COVER) {
        // Triangle 1: [0]BL -> [1]BR -> [2]TL
        helix::mesh::fill_triangle_gradient(
            buf, quad.screen_x[0], quad.screen_y[0], quad.vertices[0].color, quad.screen_x[1],
            quad.screen_y[1], quad.vertices[1].color, quad.screen_x[2], quad.screen_y[2],
            quad.vertices[2].color, opacity);
        // Triangle 2: [1]BR -> [3]TR -> [2]TL
        helix::mesh::fill_triangle_gradient(
            buf, quad.screen_x[1], quad.screen_y[1], quad.vertices[1].color, quad.screen_x[2],
            quad.screen_y[2], quad.vertices[2].color, quad.screen_x[3], quad.screen_y[3],
            quad.vertices[3].color, opacity);
    } else {
        // Solid color (dragging mode or translucent zero plane)
        helix::mesh::fill_triangle_solid(buf, quad.screen_x[0], quad.screen_y[0], quad.screen_x[1],
                                         quad.screen_y[1], quad.screen_x[2], quad.screen_y[2],
                                         quad.center_color, opacity);
        helix::mesh::fill_triangle_solid(buf, quad.screen_x[1], quad.screen_y[1], quad.screen_x[2],
                                         quad.screen_y[2], quad.screen_x[3], quad.screen_y[3],
                                         quad.center_color, opacity);
    }
}

/**
 * @brief Render mesh surface into a pixel buffer
 *
 * Projects quad vertices, sorts by depth, and renders each quad into the buffer.
 */
static void render_mesh_surface_to_buffer(helix::mesh::PixelBuffer& buf,
                                          bed_mesh_renderer_t* renderer, int canvas_width,
                                          int canvas_height) {
    // Project all quad vertices and cache screen coordinates + depths
    project_and_cache_quads(renderer, canvas_width, canvas_height);

    // Sort quads by depth (painter's algorithm - furthest first)
    helix::mesh::sort_quads_by_depth(renderer->quads);

    // Render quads using cached screen coordinates
    bool use_gradient = !renderer->view_state.is_dragging;
    for (const auto& quad : renderer->quads) {
        render_quad_to_buffer(buf, quad, use_gradient);
    }
}

/**
 * @brief Render decorations (grid lines) into a pixel buffer
 *
 * Renders wireframe grid on top of mesh surface.
 * Note: axis labels and tick labels are NOT rendered to buffer
 * (text rendering requires LVGL font engine, will be handled on main thread).
 */
static void render_decorations_to_buffer(helix::mesh::PixelBuffer& buf,
                                         bed_mesh_renderer_t* renderer, int canvas_width,
                                         int canvas_height, uint8_t grid_r, uint8_t grid_g,
                                         uint8_t grid_b) {
    // Render wireframe grid on top of mesh surface
    helix::mesh::render_grid_lines(buf, renderer, canvas_width, canvas_height, grid_r, grid_g,
                                   grid_b);

    // Note: axis labels and numeric tick labels are skipped for buffer rendering.
    // They require LVGL's font engine and will be composited on the main thread.
}

// ============================================================================
// Adaptive Render Mode (FPS-based 3D/2D switching)
// ============================================================================

/**
 * @brief Record frame time for FPS tracking
 */
static void record_frame_time(bed_mesh_renderer_t* renderer, float frame_ms) {
    renderer->frame_times[renderer->fps_write_idx] = frame_ms;
    renderer->fps_write_idx = (renderer->fps_write_idx + 1) % BED_MESH_FPS_WINDOW_SIZE;
    if (renderer->fps_sample_count < BED_MESH_FPS_WINDOW_SIZE) {
        renderer->fps_sample_count++;
    }
}

/**
 * @brief Calculate average FPS from recorded frame times
 */
static float calculate_average_fps(const bed_mesh_renderer_t* renderer) {
    if (renderer->fps_sample_count == 0) {
        return 60.0f; // Assume good until measured
    }

    float total_ms = 0.0f;
    for (size_t i = 0; i < renderer->fps_sample_count; i++) {
        total_ms += renderer->frame_times[i];
    }
    float avg_ms = total_ms / static_cast<float>(renderer->fps_sample_count);
    return avg_ms > 0.0f ? 1000.0f / avg_ms : 60.0f;
}

/**
 * @brief Check if FPS is below threshold (requires full sample window)
 */
static bool is_fps_below_threshold(const bed_mesh_renderer_t* renderer, float min_fps) {
    return renderer->fps_sample_count >= BED_MESH_FPS_WINDOW_SIZE &&
           calculate_average_fps(renderer) < min_fps;
}

/**
 * @brief Render mesh as 2D heatmap with triangle-based color blending
 *
 * Each cell is rendered as 4 triangles meeting at center, with colors
 * averaged from the corner Z values. This provides smooth color transitions
 * while maintaining honest probe resolution (N-1 cells for N probe points).
 * The border and touch tooltip are drawn on the main thread
 * (helix::mesh::render_heatmap_overlay).
 */
namespace {

/// Heatmap pixels <-> printer mm over the mesh area, for a round bed's circle.
struct HeatmapBedMap {
    const bed_mesh_renderer_t* r;
    const helix::mesh::HeatmapLayout& l;
    double mm_per_px_x() const {
        return (r->mesh_area_max_x - r->mesh_area_min_x) / (l.cells_x * l.cell_w);
    }
    double mm_per_px_y() const {
        return (r->mesh_area_max_y - r->mesh_area_min_y) / (l.cells_y * l.cell_h);
    }
    helix::BedPoint to_mm(double px, double py) const {
        return {r->mesh_area_min_x + (px - l.grid_x) * mm_per_px_x(),
                r->mesh_area_min_y + (py - l.grid_y) * mm_per_px_y()};
    }
    std::pair<int, int> to_px(const helix::BedPoint& p) const {
        return {
            static_cast<int>(std::lround(l.grid_x + (p.x - r->mesh_area_min_x) / mm_per_px_x())),
            static_cast<int>(std::lround(l.grid_y + (p.y - r->mesh_area_min_y) / mm_per_px_y()))};
    }
    helix::RoundBed circle() const {
        return helix::inscribed_round_bed(r->mesh_area_min_x, r->mesh_area_max_x,
                                          r->mesh_area_min_y, r->mesh_area_max_y);
    }
};

/// The 2D heatmap shows a round bed's mesh only inside its circle.
bool heatmap_is_round(const bed_mesh_renderer_t* renderer) {
    return renderer->circular_bed && renderer->has_mesh_bounds &&
           renderer->mesh_area_max_x > renderer->mesh_area_min_x &&
           renderer->mesh_area_max_y > renderer->mesh_area_min_y;
}

} // namespace

static void render_2d_heatmap_to_buffer(helix::mesh::PixelBuffer& buf,
                                        const bed_mesh_renderer_t* renderer) {
    const auto l = helix::mesh::compute_heatmap_layout(renderer->rows, renderer->cols, buf.width(),
                                                       buf.height());
    if (!l.valid) {
        spdlog::warn("[Bed Mesh] 2D heatmap requires at least 2x2 mesh (got {}x{})", renderer->cols,
                     renderer->rows);
        return;
    }

    double z_min = renderer->auto_color_range ? renderer->mesh_min_z : renderer->color_min_z;
    double z_max = renderer->auto_color_range ? renderer->mesh_max_z : renderer->color_max_z;
    auto color_of = [&](double z) { return bed_mesh_gradient_height_to_color(z, z_min, z_max); };
    auto blend = [](lv_color_t c1, lv_color_t c2, lv_color_t c3) -> lv_color_t {
        return lv_color_make((c1.red + c2.red + c3.red) / 3, (c1.green + c2.green + c3.green) / 3,
                             (c1.blue + c2.blue + c3.blue) / 3);
    };

    const bool round = heatmap_is_round(renderer);
    const HeatmapBedMap map{renderer, l};
    const helix::RoundBed circle = round ? map.circle() : helix::RoundBed{};
    auto fill = [&](int x1, int y1, int x2, int y2, int x3, int y3, lv_color_t color) {
        if (!round) {
            helix::mesh::fill_triangle_solid(buf, x1, y1, x2, y2, x3, y3, color);
            return;
        }
        const auto poly = helix::clip_to_round_bed(
            {map.to_mm(x1, y1), map.to_mm(x2, y2), map.to_mm(x3, y3)}, circle);
        for (size_t i = 1; i + 1 < poly.size(); i++) {
            const auto [ax, ay] = map.to_px(poly[0]);
            const auto [bx, by] = map.to_px(poly[i]);
            const auto [cx, cy] = map.to_px(poly[i + 1]);
            helix::mesh::fill_triangle_solid(buf, ax, ay, bx, by, cx, cy, color);
        }
    };

    for (int row = 0; row < l.cells_y; row++) {
        const auto& top = renderer->mesh[static_cast<size_t>(row)];
        const auto& bottom = renderer->mesh[static_cast<size_t>(row + 1)];
        for (int col = 0; col < l.cells_x; col++) {
            double z_tl = top[static_cast<size_t>(col)];
            double z_tr = top[static_cast<size_t>(col + 1)];
            double z_bl = bottom[static_cast<size_t>(col)];
            double z_br = bottom[static_cast<size_t>(col + 1)];

            lv_color_t c_tl = color_of(z_tl);
            lv_color_t c_tr = color_of(z_tr);
            lv_color_t c_bl = color_of(z_bl);
            lv_color_t c_br = color_of(z_br);
            lv_color_t c_center = color_of((z_tl + z_tr + z_bl + z_br) / 4.0);

            int x_left = l.grid_x + col * l.cell_w;
            int x_right = l.grid_x + (col + 1) * l.cell_w;
            int y_top = l.grid_y + row * l.cell_h;
            int y_bottom = l.grid_y + (row + 1) * l.cell_h;
            int x_mid = (x_left + x_right) / 2;
            int y_mid = (y_top + y_bottom) / 2;

            fill(x_left, y_top, x_right, y_top, x_mid, y_mid, blend(c_tl, c_tr, c_center));
            fill(x_right, y_top, x_right, y_bottom, x_mid, y_mid, blend(c_tr, c_br, c_center));
            fill(x_right, y_bottom, x_left, y_bottom, x_mid, y_mid, blend(c_br, c_bl, c_center));
            fill(x_left, y_bottom, x_left, y_top, x_mid, y_mid, blend(c_bl, c_tl, c_center));
        }
    }
}

// ============================================================================
// Public API: Render Mode Control
// ============================================================================

void bed_mesh_renderer_set_render_mode(bed_mesh_renderer_t* renderer, BedMeshRenderMode mode) {
    if (!renderer)
        return;
    renderer->render_mode = mode;
    bed_mesh_projection_reset_zoom(&renderer->view_state);

    // If forcing a mode, update the fallback flag immediately
    if (mode == BedMeshRenderMode::Force2D) {
        renderer->using_2d_fallback = true;
    } else if (mode == BedMeshRenderMode::Force3D) {
        renderer->using_2d_fallback = false;
    }
    // AUTO mode: fallback flag is controlled by evaluate_render_mode()
}

BedMeshRenderMode bed_mesh_renderer_get_render_mode(bed_mesh_renderer_t* renderer) {
    if (!renderer)
        return BedMeshRenderMode::Auto;
    return renderer->render_mode;
}

bool bed_mesh_renderer_is_using_2d(bed_mesh_renderer_t* renderer) {
    if (!renderer)
        return false;

    switch (renderer->render_mode) {
    case BedMeshRenderMode::Force2D:
        return true;
    case BedMeshRenderMode::Force3D:
        return false;
    case BedMeshRenderMode::Auto:
    default:
        return renderer->using_2d_fallback;
    }
}

void bed_mesh_renderer_evaluate_render_mode(bed_mesh_renderer_t* renderer) {
    if (!renderer)
        return;
    if (renderer->render_mode != BedMeshRenderMode::Auto) {
        spdlog::debug("[Bed Mesh Renderer] Mode evaluation skipped (mode={}, not AUTO)",
                      static_cast<int>(renderer->render_mode));
        return;
    }

    spdlog::debug("[Bed Mesh Renderer] Evaluating render mode: {} FPS samples, avg={:.1f} FPS",
                  renderer->fps_sample_count, calculate_average_fps(renderer));

    // Check if we have enough samples and FPS is below threshold
    if (is_fps_below_threshold(renderer, BED_MESH_FPS_THRESHOLD)) {
        if (!renderer->using_2d_fallback) {
            renderer->using_2d_fallback = true;
            spdlog::info("[Bed Mesh Renderer] Switching to 2D heatmap (FPS: {:.1f} < {:.0f})",
                         calculate_average_fps(renderer), BED_MESH_FPS_THRESHOLD);
        }
    }
    // Note: We don't auto-upgrade back to 3D (user must explicitly request via settings)
}

// ============================================================================
// Public API: Touch Handling for 2D Mode
// ============================================================================

bool bed_mesh_renderer_handle_touch(bed_mesh_renderer_t* renderer, int touch_x, int touch_y,
                                    const helix::mesh::HeatmapLayout& l) {
    if (!renderer || !renderer->has_mesh_data)
        return false;

    if (!l.valid) {
        renderer->touch_valid = false;
        return false;
    }

    // Convert touch to cell coordinates
    int col = (touch_x - l.grid_x) / l.cell_w;
    int row = (touch_y - l.grid_y) / l.cell_h;

    // Check bounds (N-1 cells); the shown frame may predate a smaller mesh
    if (col < 0 || col >= l.cells_x || row < 0 || row >= l.cells_y || row >= renderer->rows ||
        col >= renderer->cols) {
        renderer->touch_valid = false;
        return false;
    }

    // A round bed has no reading outside its circle.
    if (heatmap_is_round(renderer)) {
        const HeatmapBedMap map{renderer, l};
        const auto p = map.to_mm(touch_x, touch_y);
        const auto c = map.circle();
        if (std::hypot(p.x - c.cx, p.y - c.cy) > c.r) {
            renderer->touch_valid = false;
            return false;
        }
    }

    // Store touched cell info
    // Cell (row, col) has its top-left corner at mesh point (row, col),
    // so cell indices directly map to mesh array indices for the corner Z value
    renderer->touched_row = row;
    renderer->touched_col = col;
    renderer->touched_z =
        static_cast<float>(renderer->mesh[static_cast<size_t>(row)][static_cast<size_t>(col)]);
    renderer->touch_valid = true;

    return true;
}

void bed_mesh_renderer_clear_touch(bed_mesh_renderer_t* renderer) {
    if (!renderer)
        return;
    renderer->touch_valid = false;
}

// ============================================================================
// Public API: Zero Reference Plane
// ============================================================================

void bed_mesh_renderer_set_zero_plane_visible(bed_mesh_renderer_t* renderer, bool visible) {
    if (!renderer)
        return;

    if (renderer->show_zero_plane == visible)
        return; // No change

    renderer->show_zero_plane = visible;
    spdlog::debug("[Bed Mesh Renderer] Zero plane visibility set to {}", visible);

    // Regenerate quads to add/remove plane quads
    if (renderer->has_mesh_data) {
        helix::mesh::generate_mesh_quads(renderer);

        // State transition: READY_TO_RENDER → MESH_LOADED (quads regenerated, projections invalid)
        if (renderer->state == RendererState::READY_TO_RENDER) {
            renderer->state = RendererState::MESH_LOADED;
        }
    }
}

void bed_mesh_renderer_set_z_display_offset(bed_mesh_renderer_t* renderer, double offset_mm) {
    if (!renderer)
        return;
    renderer->z_display_offset = offset_mm;
    spdlog::debug("[Bed Mesh Renderer] Z display offset set to {:.4f}mm", offset_mm);
}

void bed_mesh_renderer_set_layer_offset(bed_mesh_renderer_t* renderer, int offset_x, int offset_y) {
    if (!renderer)
        return;
    renderer->view_state.layer_offset_x = offset_x;
    renderer->view_state.layer_offset_y = offset_y;
}

void bed_mesh_renderer_get_layer_offset(const bed_mesh_renderer_t* renderer, int* offset_x,
                                        int* offset_y) {
    if (!renderer) {
        if (offset_x)
            *offset_x = 0;
        if (offset_y)
            *offset_y = 0;
        return;
    }
    if (offset_x)
        *offset_x = renderer->view_state.layer_offset_x;
    if (offset_y)
        *offset_y = renderer->view_state.layer_offset_y;
}

#endif // HELIX_HAS_BED_MESH_3D
