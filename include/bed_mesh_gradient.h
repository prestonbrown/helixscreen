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

#include <lvgl/lvgl.h>
#include <cstdint>

/**
 * @file bed_mesh_gradient.h
 * @brief Heat-map color gradient calculation for bed mesh visualization
 *
 * Provides a 5-band gradient (Purple→Blue→Cyan→Yellow→Red) with pre-computed
 * lookup table for fast color mapping from Z-height values to RGB colors.
 *
 * Thread-safe via std::call_once initialization.
 */

/**
 * @brief RGB color structure for gradient calculations
 */
struct bed_mesh_rgb_t {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

/**
 * @brief Map Z-height value to heat-map color
 *
 * Converts a mesh Z-height value to an RGB color using a 5-band gradient:
 * Purple (low) → Blue → Cyan → Yellow → Red (high)
 *
 * Thread-safe: Initializes gradient LUT on first call via std::call_once
 *
 * @param value Z-height value to map
 * @param min_val Minimum Z-height in mesh (maps to purple)
 * @param max_val Maximum Z-height in mesh (maps to red)
 * @return LVGL color for the given height value
 */
lv_color_t bed_mesh_gradient_height_to_color(double value, double min_val, double max_val);

/**
 * @brief Linearly interpolate between two RGB colors
 *
 * @param a Start color (t=0.0)
 * @param b End color (t=1.0)
 * @param t Interpolation factor [0.0, 1.0]
 * @return Interpolated color
 */
bed_mesh_rgb_t bed_mesh_gradient_lerp_color(bed_mesh_rgb_t a, bed_mesh_rgb_t b, double t);
