// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bed_mesh_renderer.h"

#include <array>
#include <vector>

/**
 * @file bed_mesh_internal.h
 * @brief Internal structures for bed mesh renderer module
 *
 * This header exposes the internal bed_mesh_renderer struct definition
 * for use by bed mesh rendering modules (overlays, geometry, etc.).
 *
 * DO NOT include this header from UI code - use bed_mesh_renderer.h instead.
 */

/**
 * @brief Renderer lifecycle state
 *
 * State transitions:
 * - UNINITIALIZED → MESH_LOADED: set_mesh_data() called
 * - MESH_LOADED → MESH_LOADED: set_z_scale() or set_color_range() invalidates quads
 * - MESH_LOADED → READY_TO_RENDER: quads generated and projected
 * - READY_TO_RENDER → MESH_LOADED: view state changes (rotation, FOV)
 * - ANY → ERROR: validation failure in public API
 *
 * Invariants:
 * - UNINITIALIZED: has_mesh_data == false, quads.empty()
 * - MESH_LOADED: has_mesh_data == true, quads may be stale (regenerate before render)
 * - READY_TO_RENDER: has_mesh_data == true, quads valid, projections cached
 * - ERROR: renderer unusable, must be destroyed
 */
enum class RendererState {
    UNINITIALIZED,   // Created, no mesh data
    MESH_LOADED,     // Mesh data loaded, quads may need regeneration
    READY_TO_RENDER, // Projection cached, ready for render()
    ERROR            // Invalid state (e.g., set_mesh_data failed)
};

// Internal renderer state structure
struct bed_mesh_renderer {
    // State machine
    RendererState state;

    // Mesh data storage
    std::vector<std::vector<double>> mesh; // mesh[row][col] = Z height
    int rows;
    int cols;
    double mesh_min_z;
    double mesh_max_z;
    double cached_z_center; // (mesh_min_z + mesh_max_z) / 2, updated by compute_mesh_bounds()
    bool has_mesh_data;     // Redundant with state, kept for backwards compatibility

    // Bed XY bounds (full print bed in mm - used for grid/walls)
    double bed_min_x;
    double bed_min_y;
    double bed_max_x;
    double bed_max_y;

    // Mesh XY bounds (probe area in mm - used for positioning mesh surface)
    double mesh_area_min_x;
    double mesh_area_min_y;
    double mesh_area_max_x;
    double mesh_area_max_y;
    bool has_mesh_bounds;

    // Computed geometry parameters (derived from bounds)
    double bed_center_x;    // (bed_min_x + bed_max_x) / 2
    double bed_center_y;    // (bed_min_y + bed_max_y) / 2
    double coord_scale;     // World units per mm (normalizes bed to target world size)
    bool geometry_computed; // True if bed_center and coord_scale are valid
    bool circular_bed;      // Round bed (delta): the circle inscribed in the bed bounds

    // Color range configuration
    bool auto_color_range;
    double color_min_z;
    double color_max_z;

    // View/camera state
    bed_mesh_view_state_t view_state;

    // Computed rendering state
    std::vector<bed_mesh_quad_3d_t> quads; // Generated geometry

    // Cached projected screen coordinates (SOA layout for better cache efficiency)
    // Only stores screen_x/screen_y - no unused fields (world x/y/z, depth)
    // Old AOS: 40 bytes/point (5 doubles + 2 ints), New SOA: 8 bytes/point (2 ints)
    // Memory savings: 80% reduction (16 KB → 3.2 KB for 20×20 mesh)
    std::vector<std::vector<int>> projected_screen_x; // [row][col] → screen X coordinate
    std::vector<std::vector<int>> projected_screen_y; // [row][col] → screen Y coordinate

    // ===== Adaptive Render Mode =====

    // Render mode control
    helix::BedMeshRenderMode render_mode = helix::BedMeshRenderMode::Auto;
    bool using_2d_fallback = false; // True if currently rendering as 2D heatmap

    // FPS tracking (rolling window average)
    std::array<float, BED_MESH_FPS_WINDOW_SIZE> frame_times{}; // Frame times in ms
    size_t fps_write_idx = 0;                                  // Next write position
    size_t fps_sample_count = 0;                               // Number of valid samples

    // Touch state for 2D mode tooltip
    bool touch_valid = false; // True if touched_* fields are valid
    int touched_row = 0;      // Mesh row of touched cell
    int touched_col = 0;      // Mesh column of touched cell
    float touched_z = 0.0f;   // Z value of touched cell

    // Initial calibration state (prevents recalculating on subsequent frames)
    bool initial_centering_computed = false; // True after first centering offset computation

    // ===== Z Display Offset =====
    // When mesh data is normalized (mean-subtracted) for visualization, this offset
    // is added back to Z values for axis labels so they show real probe heights.
    double z_display_offset = 0.0;

    // ===== Zero Plane Feature =====
    // Translucent reference plane at Z=0 (or Z-offset) showing where nozzle touches bed
    bool show_zero_plane = true;             // Enable/disable the zero plane visualization
    double zero_plane_z_offset = 0.0;        // Offset from Z=0 in mm (e.g., printer's Z-offset)
    lv_opa_t zero_plane_opacity = LV_OPA_20; // Opacity of the zero plane (20% default)
};
