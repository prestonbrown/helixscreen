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

#pragma once

#include "bed_mesh_renderer.h" // For bed_mesh_point_3d_t, bed_mesh_view_state_t

/**
 * @file bed_mesh_projection.h
 * @brief 3D to 2D projection for bed mesh visualization
 *
 * Provides perspective projection from 3D world coordinates to 2D screen space
 * with rotation (Z-axis spin, X-axis tilt) and depth calculation for z-buffering.
 */

/**
 * @brief Project a 3D point to 2D screen space with depth
 *
 * Applies 3D rotation (Z-axis spin, X-axis tilt), camera translation, and
 * perspective projection to convert world coordinates to screen coordinates.
 *
 * Uses cached trigonometric values from view_state for performance.
 *
 * @param x World X coordinate
 * @param y World Y coordinate
 * @param z World Z coordinate
 * @param canvas_width Canvas width in pixels
 * @param canvas_height Canvas height in pixels
 * @param view View/camera state (angles, scale, centering offset)
 * @return Projected point with screen_x, screen_y, and depth
 */
bed_mesh_point_3d_t bed_mesh_projection_project_3d_to_2d(double x, double y, double z,
                                                         int canvas_width, int canvas_height,
                                                         const bed_mesh_view_state_t* view);
