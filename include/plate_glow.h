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

/// The same scale for a chamber's air: 0 at room temperature, then 1 past 30°C,
/// 2 from 45°C and 3 from 60°C.
int chamber_glow_level(int current_deci);

/// An 8-bit coverage mask placed in its owner's coordinates.
struct PlateGlowMask {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> alpha; ///< w * h bytes, row-major
    int step = 1;               ///< on-screen pixels per mask pixel; draw it at w*step x h*step
};

/// A region mask never holds more than this many bytes; a larger one renders
/// coarser (PlateGlowMask::step) and is drawn scaled up.
constexpr size_t kMaxGlowMaskBytes = 160 * 1024;

/// One four-cornered region of a picture, in pixels.
using GlowQuad = std::array<CalloutPoint, 4>;

/// A wash of light confined to `quads`, brightest at `source` and easing to
/// `floor` (0..1 of `peak`) at `reach` pixels from it. Edges feather inward, so
/// nothing lands outside a quad, and `cutouts` take it away again where they
/// cover it. Empty for no quads or quads with no area.
PlateGlowMask render_region_glow(const std::vector<GlowQuad>& quads, CalloutPoint source,
                                 float reach, float floor, float peak,
                                 const std::vector<GlowQuad>& cutouts = {});

/// A soft disc: full at `center`, fading to nothing at `radius` pixels.
PlateGlowMask render_disc_glow(CalloutPoint center, int radius);

/// The glow for a plate pictured at `quad` (near-left, near-right, far-right,
/// far-left, in pixels), with the haze reaching `rise` pixels above the plate's
/// top edge. Empty (w or h 0) for a quad with no width.
PlateGlowMask render_plate_glow(const std::array<CalloutPoint, 4>& quad, int rise);

} // namespace helix
