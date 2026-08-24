// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix {
namespace ui {

/// A point in screen (pixel) space produced by pluck_iso_project().
struct PluckPoint {
    float x = 0.0f;
    float y = 0.0f;
};

/// 30-degree isometric projection, matching
/// .superpowers/brainstorm/3846895-1786305129/content/pluck-iso-generator.py
/// exactly:
///   screen_x = ox + (x - y) * cos(30deg) * scale
///   screen_y = oy + ((x + y) * 0.5 - z) * scale
/// (x, y, z) are scene-space coordinates: x sideways, y front-to-back
/// (larger y is nearer the viewer), z up. (ox, oy) is the pixel origin and
/// `scale` folds together the isometric foreshortening and whatever zoom is
/// needed to fit the widget's actual pixel size.
PluckPoint pluck_iso_project(float x, float y, float z, float ox, float oy, float scale);

/// The instruction loop's period. Three phases within it - reach for the
/// span, hook the top edge and pull, release and let it ring - each
/// occupying an equal third.
inline constexpr uint32_t PLUCK_LOOP_MS = 2500;

/// Which of the three phases `t` (milliseconds into the loop; wraps at
/// PLUCK_LOOP_MS) falls in: 0 = reach, 1 = hook and pull, 2 = release/ring.
int pluck_frame_at_ms(uint32_t t);

/// How far the belt is pulled at time `t`, continuous across the whole loop
/// rather than jumping between three static poses: 0 through the reach
/// phase (belt undisturbed, hand still approaching), easing up to 1 (fully
/// pulled, held) through the hook phase, then a decaying oscillation back
/// through and past 0 during release - the ring-down. 0 means taut; the
/// range is approximately [-0.4, 1.0].
float pluck_deflection_at_ms(uint32_t t);

/// Register the <pluck_animation> custom XML widget. Must run before
/// register_xml("panel_belt_tension.xml") - same contract as
/// register_belt_trace_widget(): the tag silently resolves to nothing
/// otherwise, and the POSITION/LISTEN states lose the illustration.
void register_pluck_animation_widget();

} // namespace ui
} // namespace helix
