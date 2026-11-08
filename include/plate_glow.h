// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "callout_layout.h"

#include <array>
#include <cstdint>
#include <vector>

/// The heat glow over a printer picture's bed plate: the plate's top filled, and
/// a haze rising above it that fades out. The geometry and the rasterizing are
/// pure, so the look is testable without LVGL; ui_plate_glow.h draws the mask.
namespace helix {

/// How strongly a bed at `current_deci` (tenths of a degree) glows: 0 for a
/// plate too cool to burn, then 1 (warm), 2 (hot) and 3 (very hot). It reads
/// the temperature, not the target, so a plate cooling from 100°C still glows.
int bed_glow_level(int current_deci);

/// An 8-bit coverage mask placed in its owner's coordinates.
struct PlateGlowMask {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> alpha; ///< w * h bytes, row-major
};

/// The glow for a plate pictured at `quad` (near-left, near-right, far-right,
/// far-left, in pixels), with the haze reaching `rise` pixels above the plate's
/// top edge. Empty (w or h 0) for a quad with no width.
PlateGlowMask render_plate_glow(const std::array<CalloutPoint, 4>& quad, int rise);

} // namespace helix
