// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file layer_estimate.h
 * @brief The arithmetic of the two fabricated current-layer tiers.
 *
 * Pure logic: no LVGL, no state. A printer that never reports a layer number
 * gets one estimated from file progress, or derived from commanded Z when the
 * slice geometry is known. Both land on a layer the print can hold, 1 through
 * the file's total. When each tier may run is PrinterPrintState's decision.
 */

#include <cmath>

namespace helix {

/// A fabricated layer within the print: at least 1, at most @p total.
/// @p total must be positive.
[[nodiscard]] inline int clamp_layer(int layer, int total) {
    if (layer < 1) {
        return 1;
    }
    return layer > total ? total : layer;
}

/// The layer a file-progress percentage implies, rounded to the nearest layer.
[[nodiscard]] inline int layer_from_progress(int progress_pct, int total) {
    return clamp_layer((progress_pct * total + 50) / 100, total);
}

/**
 * @brief The layer commanded Z sits on.
 *
 * While layer N prints, Z is first + (N-1) * height, so the layer is
 * round((z - first) / height) + 1. Rounding rather than ceil absorbs half a
 * layer of commanded-Z float noise either way. @p layer_height must be positive.
 */
[[nodiscard]] inline int layer_from_z(double z_mm, double first_layer_height, double layer_height,
                                      int total) {
    return clamp_layer(
        static_cast<int>(std::lround((z_mm - first_layer_height) / layer_height)) + 1, total);
}

} // namespace helix
