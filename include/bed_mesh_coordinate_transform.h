// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file bed_mesh_coordinate_transform.h
 * @brief Coordinate transformation utilities for bed mesh 3D rendering
 *
 * Provides unified interface for transforming coordinates through the
 * rendering pipeline:
 *
 * MESH SPACE → WORLD SPACE → CAMERA SPACE → SCREEN SPACE
 *
 * This consolidates all coordinate math into a single namespace,
 * eliminating duplication across multiple rendering functions.
 */

namespace helix {
namespace mesh {

/**
 * @brief Convert mesh column index to centered world X coordinate
 *
 * Centers the mesh around origin: col=0 maps to negative X, col=cols-1 to positive X.
 * Works correctly for both odd (7x7) and even (8x8) mesh sizes.
 *
 * @param col Column index in mesh [0, cols-1]
 * @param cols Total number of columns in mesh
 * @param scale Spacing between mesh points in world units (BED_MESH_SCALE)
 * @return World X coordinate (centered around origin)
 */
double mesh_col_to_world_x(int col, int cols, double scale);

/**
 * @brief Convert mesh row index to centered world Y coordinate
 *
 * Inverts Y-axis and centers: row=0 (front edge) maps to positive Y.
 * Works correctly for both odd and even mesh sizes.
 *
 * @param row Row index in mesh [0, rows-1]
 * @param rows Total number of rows in mesh
 * @param scale Spacing between mesh points in world units (BED_MESH_SCALE)
 * @return World Y coordinate (centered around origin, Y-axis inverted)
 */
double mesh_row_to_world_y(int row, int rows, double scale);

/**
 * @brief Convert mesh Z height to centered/scaled world Z coordinate
 *
 * Centers Z values around z_center and applies scale factor for visualization.
 *
 * @param z_height Raw Z height from mesh data (millimeters)
 * @param z_center Center point for Z values (typically (min_z + max_z) / 2)
 * @param z_scale Vertical amplification factor for visualization
 * @return World Z coordinate (centered and scaled)
 */
double mesh_z_to_world_z(double z_height, double z_center, double z_scale);

/**
 * @brief Convert world Z coordinate back to mesh Z value (inverse of mesh_z_to_world_z)
 * @param world_z The world-space Z coordinate
 * @param z_center The Z center offset
 * @param z_scale The Z scaling factor
 * @return The mesh Z value in mm
 */
double world_z_to_mesh_z(double world_z, double z_center, double z_scale);

/**
 * @brief Compute Z-center value for mesh rendering
 *
 * Calculates the midpoint of mesh Z values for centering the mesh around origin.
 * This value is used across all rendering functions for consistent Z positioning.
 *
 * @param mesh_min_z Minimum Z value in mesh data
 * @param mesh_max_z Maximum Z value in mesh data
 * @return Center Z value (min_z + max_z) / 2
 */
double compute_mesh_z_center(double mesh_min_z, double mesh_max_z);

/**
 * @brief Compute grid plane Z coordinate in world space
 *
 * Calculates the Z coordinate for the base grid plane used in axis rendering.
 * The grid sits at the base of the mesh after centering and scaling.
 *
 * @param z_center Mesh Z center value (from compute_mesh_z_center)
 * @param z_scale Vertical amplification factor
 * @return Grid plane Z coordinate in world space
 */
double compute_grid_z(double z_center, double z_scale);

// Printer coordinate transforms (Mainsail-style: separate bed grid from mesh)
// Works with any printer origin (corner at 0,0 or center at origin)

/**
 * @brief Convert printer X coordinate (mm) to world X coordinate
 *
 * Maps printer coordinates to world space, centered around the bed center.
 * Works for any origin convention:
 * - Corner origin (0 to 200mm): center=100, transforms to [-100*s, +100*s]
 * - Center origin (-125 to +125mm): center=0, transforms to [-125*s, +125*s]
 *
 * @param x_mm Printer X coordinate in millimeters
 * @param bed_center_x Center of bed in mm: (bed_min_x + bed_max_x) / 2
 * @param scale_factor World units per millimeter
 * @return World X coordinate (centered around bed center)
 */
double printer_x_to_world_x(double x_mm, double bed_center_x, double scale_factor);

/**
 * @brief Convert printer Y coordinate (mm) to world Y coordinate
 *
 * Maps printer coordinates to world space, centered around the bed center.
 * Y-axis is inverted (front of bed = positive Y in world space for 3D view).
 *
 * @param y_mm Printer Y coordinate in millimeters
 * @param bed_center_y Center of bed in mm: (bed_min_y + bed_max_y) / 2
 * @param scale_factor World units per millimeter
 * @return World Y coordinate (centered around bed center, Y inverted)
 */
double printer_y_to_world_y(double y_mm, double bed_center_y, double scale_factor);

/**
 * @brief Compute scale factor for printer coordinate transforms
 *
 * Calculates scale factor to normalize bed size to a target world size.
 * This ensures consistent visualization across different bed sizes.
 *
 * @param bed_size_mm Bed dimension in mm (e.g., bed_max_x - bed_min_x)
 * @param target_world_size Desired size in world units (default ~200 for good visualization)
 * @return Scale factor (world units per mm)
 */
double compute_bed_scale_factor(double bed_size_mm, double target_world_size);

// Wall/floor/ceiling bounds for reference grids

/**
 * @brief Computed wall/floor/ceiling bounds for reference grids
 */
struct WallBounds {
    double floor_z;     ///< Z coordinate of floor plane
    double ceiling_z;   ///< Z coordinate of ceiling (top of walls)
    double wall_height; ///< Total height from floor to ceiling
};

/**
 * @brief Compute wall bounds from mesh Z extent and bed dimensions
 *
 * Calculates floor, ceiling, and wall height for reference grid rendering.
 * The mesh "floats" inside these bounds with 25% padding below and 100% above.
 *
 * @param z_min_world Minimum Z in world coordinates (from mesh_z_to_world_z)
 * @param z_max_world Maximum Z in world coordinates (from mesh_z_to_world_z)
 * @param bed_half_width Half-width of bed in world units
 * @param bed_half_height Half-height of bed in world units
 * @return WallBounds with floor_z, ceiling_z, and wall_height
 */
WallBounds compute_wall_bounds(double z_min_world, double z_max_world, double bed_half_width,
                               double bed_half_height);

} // namespace mesh
} // namespace helix
