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

#include "bed_mesh_gradient.h"
#include "bed_mesh_renderer.h" // For BED_MESH_COLOR_COMPRESSION constant

#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>

namespace {

// Color desaturation (for muted heat map appearance)
constexpr double COLOR_SATURATION = 0.65;   // 65% original color
constexpr double COLOR_DESATURATION = 0.35; // 35% grayscale mix

// Heat-map gradient band thresholds (5-band: Purple→Blue→Cyan→Yellow→Red)
constexpr double GRADIENT_BAND_1_END = 0.125; // Purple to Blue transition
constexpr double GRADIENT_BAND_2_END = 0.375; // Blue to Cyan transition
constexpr double GRADIENT_BAND_3_END = 0.625; // Cyan to Yellow transition
constexpr double GRADIENT_BAND_4_END = 0.875; // Yellow to Red transition

// Gradient color RGB values (endpoints of each band)
// Note: Omitted values are 0 (e.g., Purple has G=0, Red has G=0 and B=0)
constexpr uint8_t GRADIENT_PURPLE_R = 128;
constexpr uint8_t GRADIENT_PURPLE_B = 255;
constexpr uint8_t GRADIENT_BLUE_G = 128;
constexpr uint8_t GRADIENT_CYAN_G = 255;
constexpr uint8_t GRADIENT_YELLOW_R = 255;
constexpr uint8_t GRADIENT_YELLOW_G = 255;
constexpr uint8_t GRADIENT_RED_R = 255;

// Color gradient lookup table (pre-computed for performance)
constexpr int COLOR_GRADIENT_LUT_SIZE = 1024; // 1024 samples for smooth gradient
lv_color_t g_color_gradient_lut[COLOR_GRADIENT_LUT_SIZE];
std::once_flag g_color_gradient_lut_init_flag;

} // anonymous namespace

/**
 * Initialize color gradient lookup table (called once at startup)
 * Pre-computes all gradient colors to avoid repeated calculations
 * Thread-safe via std::call_once
 */
static void init_color_gradient_lut() {
    // Pre-compute gradient for normalized values [0.0, 1.0]
    for (int i = 0; i < COLOR_GRADIENT_LUT_SIZE; i++) {
        double normalized = static_cast<double>(i) / (COLOR_GRADIENT_LUT_SIZE - 1);

        // Compute RGB using 5-band heat-map (Purple→Blue→Cyan→Yellow→Red)
        uint8_t r, g, b;

        if (normalized < GRADIENT_BAND_1_END) {
            // Band 1: Purple to Blue
            double t = normalized / GRADIENT_BAND_1_END;
            r = static_cast<uint8_t>(GRADIENT_PURPLE_R * (1.0 - t));
            g = static_cast<uint8_t>(GRADIENT_BLUE_G * t);
            b = GRADIENT_PURPLE_B;
        } else if (normalized < GRADIENT_BAND_2_END) {
            // Band 2: Blue to Cyan
            double band_width = GRADIENT_BAND_2_END - GRADIENT_BAND_1_END;
            double t = (normalized - GRADIENT_BAND_1_END) / band_width;
            r = 0;
            g = static_cast<uint8_t>(GRADIENT_BLUE_G + (GRADIENT_CYAN_G - GRADIENT_BLUE_G) * t);
            b = GRADIENT_PURPLE_B;
        } else if (normalized < GRADIENT_BAND_3_END) {
            // Band 3: Cyan to Yellow
            double band_width = GRADIENT_BAND_3_END - GRADIENT_BAND_2_END;
            double t = (normalized - GRADIENT_BAND_2_END) / band_width;
            r = static_cast<uint8_t>(GRADIENT_YELLOW_R * t);
            g = GRADIENT_CYAN_G;
            b = static_cast<uint8_t>(GRADIENT_PURPLE_B * (1.0 - t));
        } else if (normalized < GRADIENT_BAND_4_END) {
            // Band 4: Yellow to Red
            double band_width = GRADIENT_BAND_4_END - GRADIENT_BAND_3_END;
            double t = (normalized - GRADIENT_BAND_3_END) / band_width;
            r = GRADIENT_YELLOW_R;
            g = static_cast<uint8_t>(GRADIENT_YELLOW_G * (1.0 - t));
            b = 0;
        } else {
            // Band 5: Deep Red (maximum temperature)
            r = GRADIENT_RED_R;
            g = 0;
            b = 0;
        }

        // Desaturate by 35% for muted appearance
        uint8_t gray = (r + g + b) / 3;
        r = static_cast<uint8_t>(r * COLOR_SATURATION + gray * COLOR_DESATURATION);
        g = static_cast<uint8_t>(g * COLOR_SATURATION + gray * COLOR_DESATURATION);
        b = static_cast<uint8_t>(b * COLOR_SATURATION + gray * COLOR_DESATURATION);

        g_color_gradient_lut[i] = lv_color_make(r, g, b);
    }

    spdlog::debug("Initialized bed mesh color gradient LUT with {} samples", COLOR_GRADIENT_LUT_SIZE);
}

lv_color_t bed_mesh_gradient_height_to_color(double value, double min_val, double max_val) {
    // Ensure LUT is initialized (thread-safe, runs exactly once)
    std::call_once(g_color_gradient_lut_init_flag, init_color_gradient_lut);

    // Apply color compression for enhanced contrast
    double data_range = max_val - min_val;
    double adjusted_range = data_range * BED_MESH_COLOR_COMPRESSION;
    double data_center = (min_val + max_val) / 2.0;
    double color_min = data_center - (adjusted_range / 2.0);

    // Normalize to [0, 1]
    double normalized = (value - color_min) / adjusted_range;
    normalized = std::max(0.0, std::min(1.0, normalized));

    // Look up color in pre-computed gradient table (10-15% faster than computing)
    // Map normalized [0.0, 1.0] to LUT index [0, 1023]
    int lut_index = static_cast<int>(normalized * (COLOR_GRADIENT_LUT_SIZE - 1));
    lut_index = std::max(0, std::min(COLOR_GRADIENT_LUT_SIZE - 1, lut_index));

    return g_color_gradient_lut[lut_index];
}

bed_mesh_rgb_t bed_mesh_gradient_lerp_color(bed_mesh_rgb_t a, bed_mesh_rgb_t b, double t) {
    bed_mesh_rgb_t result;
    result.r = static_cast<uint8_t>(a.r + t * (b.r - a.r));
    result.g = static_cast<uint8_t>(a.g + t * (b.g - a.g));
    result.b = static_cast<uint8_t>(a.b + t * (b.b - a.b));
    return result;
}
