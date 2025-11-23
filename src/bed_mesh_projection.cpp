// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "bed_mesh_projection.h"

bed_mesh_point_3d_t bed_mesh_projection_project_3d_to_2d(double x, double y, double z,
                                                         int canvas_width, int canvas_height,
                                                         const bed_mesh_view_state_t* view) {
    bed_mesh_point_3d_t result;

    // Step 1: Z-axis rotation (spin around vertical axis)
    // Use cached trig values (computed once per frame instead of per-vertex)
    double rotated_x = x * view->cached_cos_z - y * view->cached_sin_z;
    double rotated_y = x * view->cached_sin_z + y * view->cached_cos_z;
    double rotated_z = z;

    // Step 2: X-axis rotation (tilt up/down)
    // Use cached trig values (computed once per frame instead of per-vertex)
    double final_x = rotated_x;
    double final_y = rotated_y * view->cached_cos_x + rotated_z * view->cached_sin_x;
    double final_z = rotated_y * view->cached_sin_x - rotated_z * view->cached_cos_x;

    // Step 3: Translate camera back
    final_z += BED_MESH_CAMERA_DISTANCE;

    // Step 4: Perspective projection (similar triangles)
    double perspective_x = (final_x * view->fov_scale) / final_z;
    double perspective_y = (final_y * view->fov_scale) / final_z;

    // Step 5: Convert to screen coordinates (centered in canvas)
    result.screen_x = static_cast<int>(canvas_width / 2 + perspective_x) + view->center_offset_x;
    result.screen_y =
        static_cast<int>(canvas_height * BED_MESH_Z_ORIGIN_VERTICAL_POS + perspective_y) +
        view->center_offset_y;
    result.depth = final_z;

    return result;
}
