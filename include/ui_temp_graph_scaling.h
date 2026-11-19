// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

/**
 * @file ui_temp_graph_scaling.h
 * @brief Dynamic Y-axis scaling for temperature graphs
 *
 * Single auto-range implementation used by all temperature graph contexts:
 * TemperatureService (mini graph), TempGraphWidget (dashboard), TempGraphOverlay (full-screen).
 */

/// Default auto-range parameters (overridable per-context)
struct TempGraphScaleParams {
    float step = 50.0f;
    float floor = 150.0f;
    float ceiling = 300.0f;
    float expand_threshold = 0.80f; // Expand when max_temp > current * this
    float shrink_threshold = 0.60f; // Shrink when max_temp < current * this
};

/**
 * @brief Calculate the optimal Y-axis maximum for a temperature graph
 *
 * Implements dynamic scaling with hysteresis and a buffer-max floor:
 * - Expands when max_temp > expand_threshold * current_max (in step increments)
 * - Shrinks when max_temp < shrink_threshold * current_max (in step decrements)
 * - Never shrinks below the highest value still in the chart buffer
 *
 * @param current_max Current Y-axis maximum
 * @param max_temp    Highest relevant temperature (current temps + targets)
 * @param buffer_max  Highest value in the chart circular buffer (from max_visible_temp)
 * @param p           Scaling parameters (step, floor, ceiling, thresholds)
 * @return New Y-axis maximum (unchanged if no scaling needed)
 */
inline float calculate_temp_graph_y_max(float current_max, float max_temp, float buffer_max,
                                        const TempGraphScaleParams& p = {}) {
    float new_max = current_max;

    // Expand: approaching current max
    if (max_temp > current_max * p.expand_threshold && current_max < p.ceiling) {
        new_max = (std::floor(max_temp / p.step) + 1.0f) * p.step;
        // Ensure we actually expand (rounding can land on current_max)
        if (new_max <= current_max)
            new_max = current_max + p.step;
    }
    // Shrink: well below current max
    else if (max_temp < current_max * p.shrink_threshold && current_max > p.floor) {
        new_max = std::max(p.floor, (std::floor(max_temp / p.step) + 1.0f) * p.step);
    }

    // Never shrink below the highest value still in the chart buffer —
    // otherwise visible historical data gets clipped at max Y
    float min_for_data = (std::floor(buffer_max / p.step) + 1.0f) * p.step;
    new_max = std::max(new_max, min_for_data);

    return std::clamp(new_max, p.floor, p.ceiling);
}

/**
 * @brief Two-temperature form of calculate_temp_graph_y_max()
 *
 * Scales to the higher of @p nozzle_temp and @p bed_temp, with the default
 * TempGraphScaleParams (floor 150, ceiling 300, expand 0.80, shrink 0.60).
 */
inline float calculate_mini_graph_y_max(float current_max, float nozzle_temp, float bed_temp,
                                        float buffer_max = 0.0f) {
    float max_temp = std::max(nozzle_temp, bed_temp);
    return calculate_temp_graph_y_max(current_max, max_temp, buffer_max);
}
