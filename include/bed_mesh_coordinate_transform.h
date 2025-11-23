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

#ifndef BED_MESH_COORDINATE_TRANSFORM_H
#define BED_MESH_COORDINATE_TRANSFORM_H

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

namespace BedMeshCoordinateTransform {

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

} // namespace BedMeshCoordinateTransform

#endif // BED_MESH_COORDINATE_TRANSFORM_H
