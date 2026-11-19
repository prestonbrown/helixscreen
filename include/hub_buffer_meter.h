// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file hub_buffer_meter.h
 * @brief The buffer's mini meter inside a hub box, shared by the unit view's path and the
 * overview's: a short track on its side, filled from the left up to a thumb at the buffer's
 * place on its travel, in the color its state calls for.
 */

#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix::ui {

/// Where a buffer sits on its travel, +1 fully compressed .. -1 fully expanded: @p bias when
/// the reading has one (anything above -1.5), else the switched @p state (0 neutral,
/// 1 compressed, 2 expanded) snapped like the slot card's.
float buffer_meter_position(float bias, int state);

/// What the meter's fill says.
enum class BufferMeterTone { Fault, Compressed, Expanded, Centered };

/// A fault wins; otherwise a position further than kBufferMeterOffCenter from the middle is
/// compressed or expanded, and the rest is centered.
BufferMeterTone buffer_meter_tone(bool fault, float position);

/// Positions further than this from center read as compressed or expanded.
inline constexpr float kBufferMeterOffCenter = 0.4f;

/// The meter's length in a box @p box_h tall.
int32_t buffer_meter_length(int32_t box_h);

/// The slot the meter takes in a box @p box_h tall: its length and a margin each side.
int32_t buffer_meter_slot_width(int32_t box_h);

struct BufferMeterColors {
    lv_color_t track;  ///< the empty track
    lv_color_t border; ///< the track's outline
    lv_color_t thumb;  ///< the thumb across the track
    lv_color_t fill;   ///< the fill, for the tone
};

/// Draws the meter centered in @p slot at @p position.
void draw_buffer_meter(lv_layer_t* layer, const lv_area_t& slot, float position,
                       const BufferMeterColors& colors);

} // namespace helix::ui
