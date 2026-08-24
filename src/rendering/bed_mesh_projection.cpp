// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "bed_mesh_projection.h"

bed_mesh_point_3d_t bed_mesh_projection_project_3d_to_2d(double x, double y, double z,
                                                         int canvas_width, int canvas_height,
                                                         const bed_mesh_view_state_t* view) {
    bed_mesh_point_3d_t result;

    // Step 1: Z-axis rotation (spin around vertical axis)
    // Convention: negative angle = clockwise rotation when viewed from above
    // Use cached trig values (computed once per frame instead of per-vertex)
    double rotated_x = x * view->cached_cos_z + y * view->cached_sin_z;
    double rotated_y = -x * view->cached_sin_z + y * view->cached_cos_z;
    double rotated_z = z;

    // Step 2: X-axis rotation (tilt up/down)
    // Standard rotation matrix around X-axis:
    //   y' = y*cos(θ) - z*sin(θ)
    //   z' = y*sin(θ) + z*cos(θ)
    // This ensures: when tilting, high-Z points move UP on screen (correct 3D perspective)
    double final_x = rotated_x;
    double final_y = rotated_y * view->cached_cos_x - rotated_z * view->cached_sin_x;
    double final_z = rotated_y * view->cached_sin_x + rotated_z * view->cached_cos_x;

    // Step 3: Transform to camera space (camera looking down -Z axis)
    // After rotation, negative final_z = farther from camera (back of scene)
    // We need: farther = larger final_z for correct perspective division
    // So: final_z = camera_distance - final_z
    final_z = view->camera_distance - final_z;

    // Safety: Ensure we never divide by zero or negative (object behind camera)
    constexpr double MIN_CAMERA_Z = 1.0;
    if (final_z < MIN_CAMERA_Z) {
        final_z = MIN_CAMERA_Z;
    }

    // Step 4: Perspective projection (similar triangles)
    double perspective_x = (final_x * view->fov_scale) / final_z;
    double perspective_y = (final_y * view->fov_scale) / final_z;

    // Step 5: Convert to screen coordinates (centered in canvas, then offset to layer position)
    // center_offset_x/y = canvas-relative centering adjustment
    // layer_offset_x/y = layer position on screen (updated every frame for animations)
    result.screen_x = static_cast<int>(canvas_width / 2 + perspective_x) + view->center_offset_x +
                      view->layer_offset_x;
    result.screen_y =
        static_cast<int>(canvas_height * BED_MESH_Z_ORIGIN_VERTICAL_POS + perspective_y) +
        view->center_offset_y + view->layer_offset_y;
    result.depth = final_z;

    return result;
}

#endif // HELIX_HAS_BED_MESH_3D
