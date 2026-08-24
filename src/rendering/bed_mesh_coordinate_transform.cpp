// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "bed_mesh_coordinate_transform.h"

#include <algorithm>

namespace helix {
namespace mesh {

double mesh_col_to_world_x(int col, int cols, double scale) {
    return (col - (cols - 1) / 2.0) * scale;
}

double mesh_row_to_world_y(int row, int rows, double scale) {
    return ((rows - 1 - row) - (rows - 1) / 2.0) * scale;
}

double mesh_z_to_world_z(double z_height, double z_center, double z_scale) {
    return (z_height - z_center) * z_scale;
}

double world_z_to_mesh_z(double world_z, double z_center, double z_scale) {
    // Inverse of mesh_z_to_world_z
    if (z_scale == 0.0) {
        return z_center;
    }
    return (world_z / z_scale) + z_center;
}

double compute_mesh_z_center(double mesh_min_z, double mesh_max_z) {
    return (mesh_min_z + mesh_max_z) / 2.0;
}

double compute_grid_z(double z_center, double z_scale) {
    // This function is deprecated - grid_z should be computed from mesh_min_z directly
    // Return 0 as a fallback, but callers should use mesh_z_to_world_z(mesh_min_z, ...) instead
    (void)z_center;
    (void)z_scale;
    return 0.0;
}

// ============================================================================
// Printer coordinate transforms (origin-agnostic)
// ============================================================================

double printer_x_to_world_x(double x_mm, double bed_center_x, double scale_factor) {
    // Simply center around the bed center - works for any origin convention
    return (x_mm - bed_center_x) * scale_factor;
}

double printer_y_to_world_y(double y_mm, double bed_center_y, double scale_factor) {
    // Center around bed center, but invert Y so that mesh[0][*] (front row) appears
    // in front (positive Y in world space, toward the viewer in 3D view)
    // The inversion is about display convention, not printer coordinate system
    return -(y_mm - bed_center_y) * scale_factor;
}

double compute_bed_scale_factor(double bed_size_mm, double target_world_size) {
    if (bed_size_mm <= 0.0) {
        return 1.0; // Fallback to avoid division by zero
    }
    return target_world_size / bed_size_mm;
}

// ============================================================================
// Wall/floor/ceiling bounds for reference grids
// ============================================================================

WallBounds compute_wall_bounds(double z_min_world, double z_max_world, double bed_half_width,
                               double bed_half_height) {
    constexpr double WALL_HEIGHT_TO_BED_RATIO = 1.25;
    constexpr double MESH_Z_TO_WALL_RATIO = 1.5;
    constexpr double FLOOR_BELOW_MESH_RATIO = 0.25;
    constexpr double CEILING_ABOVE_MESH_RATIO = 1.0;

    double mesh_z_range = z_max_world - z_min_world;
    double min_wall_height = std::max(bed_half_width, bed_half_height) * WALL_HEIGHT_TO_BED_RATIO;
    double wall_height = std::max(mesh_z_range * MESH_Z_TO_WALL_RATIO, min_wall_height);

    return WallBounds{z_min_world - wall_height * FLOOR_BELOW_MESH_RATIO,
                      z_max_world + wall_height * CEILING_ABOVE_MESH_RATIO, wall_height};
}

} // namespace mesh
} // namespace helix

#endif // HELIX_HAS_BED_MESH_3D
