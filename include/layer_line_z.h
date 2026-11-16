// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file layer_line_z.h
 * @brief Which Z height the print status layer line shows.
 *
 * Pure logic: no LVGL. Commanded Z is the height of the layer only while the
 * nozzle prints. A pause, a cancel or the end of a print parks the head, often
 * far above the model, so outside printing the height comes from the slice
 * geometry of the layer the print reached instead.
 */

#include <cmath>

namespace helix {

/**
 * @brief The Z height for the layer line, in centimillimeters; 0 means none.
 *
 * @param printing              the job is printing (not preparing, paused or ended)
 * @param live_z_centimm        the commanded Z right now
 * @param layer                 the layer the line shows, 1-based; 0 = none reached
 * @param first_layer_height_mm the slice's first-layer height, 0 when unknown
 * @param layer_height_mm       the slice's layer height, 0 when unknown
 *
 * While printing, the commanded Z. Otherwise the top of @p layer computed as
 * first + (layer - 1) * height, or 0 when the geometry is unknown or no layer
 * was reached: an honest blank beats the park height.
 */
[[nodiscard]] inline int layer_line_z_centimm(bool printing, int live_z_centimm, int layer,
                                              double first_layer_height_mm,
                                              double layer_height_mm) {
    if (printing) {
        return live_z_centimm;
    }
    if (layer < 1 || layer_height_mm <= 0.0) {
        return 0;
    }
    const double first = first_layer_height_mm > 0.0 ? first_layer_height_mm : layer_height_mm;
    const double top_mm = first + static_cast<double>(layer - 1) * layer_height_mm;
    return static_cast<int>(std::lround(top_mm * 100.0));
}

} // namespace helix
