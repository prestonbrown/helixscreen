// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl/lvgl.h>

#include <cstdint>

/**
 * @file temp_graph_column_map.h
 * @brief Where the gradient's fill starts, per pixel column.
 *
 * The gradient under a temperature trace is drawn column by column: each column
 * is filled from the curve down to the chart floor with the same vertical ramp,
 * so ONE number per column - the top row, series_y - fully determines the
 * pixels. Two things need that number and they must agree exactly:
 *
 *   - gradient_render_columns(), which draws it, and
 *   - the cache signature, which decides whether a redraw can be skipped.
 *
 * They were separate copies of the same integer math. That is a silent-staleness
 * hazard: change the renderer's mapping and the signature keeps answering for
 * the old one, so it reports "unchanged" for frames that now render differently
 * and the graph freezes. One definition, used by both.
 */

namespace helix::temp_graph {

/// A column the renderer does not fill (no data, or the curve at/below the
/// floor). Distinct from any real row so a signature can hash it as its own
/// value - a column appearing or disappearing must change the hash.
inline constexpr int32_t COLUMN_NOT_DRAWN = INT32_MIN;

/// Fractional point index for pixel column @p x, in 8.8 fixed point.
/// Mirrors the renderer: columns (not segments) are walked so each pixel column
/// is produced exactly once, which is what stops overlapping semi-transparent
/// fills compounding into dark bands where segments meet.
inline void column_to_point(int32_t x, int32_t cw, int32_t pc, int32_t& idx, int32_t& frac) {
    const int32_t frac_256 = x * (pc - 1) * 256 / (cw - 1);
    idx = frac_256 / 256;
    frac = frac_256 & 255;
    if (idx >= pc - 1) {
        idx = pc - 2;
        frac = 255;
    }
}

/**
 * @brief Top row of the fill for one column, or COLUMN_NOT_DRAWN.
 *
 * @param v0,v1   Chart values bracketing the column (raw, may be
 *                LV_CHART_POINT_NONE)
 * @param frac    Interpolation weight between them, 0-255
 * @param y_min,y_max  Chart value range
 * @param ch      Content height in pixels; the floor sits at ch
 */
inline int32_t column_series_y(int32_t v0, int32_t v1, int32_t frac, int32_t y_min, int32_t y_max,
                               int32_t ch) {
    const int32_t floor_y = ch;

    if (v0 == LV_CHART_POINT_NONE && v1 == LV_CHART_POINT_NONE) {
        return COLUMN_NOT_DRAWN;
    }
    if (v0 == LV_CHART_POINT_NONE) {
        v0 = v1;
    }
    if (v1 == LV_CHART_POINT_NONE) {
        v1 = v0;
    }

    const int32_t py0 = floor_y - lv_map(v0, y_min, y_max, 0, ch);
    const int32_t py1 = floor_y - lv_map(v1, y_min, y_max, 0, ch);
    int32_t series_y = py0 + (py1 - py0) * frac / 256;

    if (series_y < 0) {
        series_y = 0;
    }
    // At or below the floor there is no column left to fill.
    if (series_y >= floor_y) {
        return COLUMN_NOT_DRAWN;
    }
    return series_y;
}

} // namespace helix::temp_graph
