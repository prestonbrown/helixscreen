// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "screensaver_analog_clock_sim.h"
#include "screensaver_analog_paint.h"

#include <array>
#include <vector>

/**
 * @file screensaver_analog_faces.h
 * @brief What each analog clock face draws: its dial, its hands, and how far each hand reaches
 *
 * AnalogClockSim decides when to draw; these draw. No LVGL.
 */

namespace helix::ui {

/// How far one moving part of a face reaches, to box what it covers: a hand from `tail` px
/// behind the centre to `len` px out, or with `orbit` > 0 a body `orbit` px out; `pad` px of
/// width, glow and shadow round either, plus an afterglow band from `trail_r0` to `trail_r1`
/// over `trail_span` degrees behind it.
struct AnalogPartSpec {
    float tail = 0.0f;
    float len = 0.0f;
    float orbit = 0.0f;
    float pad = 0.0f;
    float trail_span = 0.0f;
    float trail_r0 = 0.0f;
    float trail_r1 = 0.0f;
};

/// The hour, minute and second parts of `style` on `g`, at a level whose `smooth` is given.
std::array<AnalogPartSpec, 3> analog_part_specs(AnalogStyle style, const AnalogGeom& g,
                                                bool smooth);

/// The box part `spec` covers pointing at `deg`.
DirtyRect analog_part_box(const AnalogPartSpec& spec, const AnalogGeom& g, float deg);

/// Draws `style`'s dial for `reading` (everything but the hands), and places its texts.
void analog_draw_dial(APainter& p, AnalogStyle style, const AnalogGeom& g,
                      const ClockReading& reading, std::vector<AnalogText>& texts);

/// Draws `style`'s hands at `hands`; `mode` colours a print's state and `smooth` adds trails
/// and glows.
void analog_draw_hands(APainter& p, AnalogStyle style, const AnalogGeom& g,
                       const AnalogHands& hands, ClockMode mode, bool smooth);

/// The colour `style` fills the frame with round its face.
Rgb analog_background(AnalogStyle style);

} // namespace helix::ui
