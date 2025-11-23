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

#include "bed_mesh_coordinate_transform.h"

namespace BedMeshCoordinateTransform {

double mesh_col_to_world_x(int col, int cols, double scale) {
    return (col - (cols - 1) / 2.0) * scale;
}

double mesh_row_to_world_y(int row, int rows, double scale) {
    return ((rows - 1 - row) - (rows - 1) / 2.0) * scale;
}

double mesh_z_to_world_z(double z_height, double z_center, double z_scale) {
    return (z_height - z_center) * z_scale;
}

double compute_mesh_z_center(double mesh_min_z, double mesh_max_z) {
    return (mesh_min_z + mesh_max_z) / 2.0;
}

double compute_grid_z(double z_center, double z_scale) {
    return -z_center * z_scale;
}

} // namespace BedMeshCoordinateTransform
