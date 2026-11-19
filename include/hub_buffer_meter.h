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

/// Whether a buffer fault of @p severity (-1 untinted, 0 neutral, 1 warning, 2 danger) shows
/// as a fault on the meter: a warning or worse.
inline bool buffer_meter_faulted(int severity) {
    return severity >= 1;
}

/// Positions further than this from center read as compressed or expanded.
inline constexpr float kBufferMeterOffCenter = 0.4f;

/// The meter's length in a box @p box_h tall.
int32_t buffer_meter_length(int32_t box_h);

/// The slot the meter takes in a box @p box_h tall: its length and a margin each side.
int32_t buffer_meter_slot_width(int32_t box_h);

/// The theme colors a meter draws in: its track, outline and thumb, and the fill for each tone.
struct BufferMeterPalette {
    lv_color_t track;   ///< the empty track
    lv_color_t border;  ///< the track's outline
    lv_color_t thumb;   ///< the thumb across the track
    lv_color_t error;   ///< fill: BufferMeterTone::Fault
    lv_color_t accent;  ///< fill: BufferMeterTone::Compressed
    lv_color_t warning; ///< fill: BufferMeterTone::Expanded
    lv_color_t success; ///< fill: BufferMeterTone::Centered
};

/// The fill @p palette gives @p tone.
lv_color_t buffer_meter_fill(BufferMeterTone tone, const BufferMeterPalette& palette);

/// Draws the meter centered in @p slot at @p position, filled for its tone: a fault of
/// @p fault_severity (buffer_meter_faulted()), else where @p position sits.
void draw_buffer_meter(lv_layer_t* layer, const lv_area_t& slot, float position, int fault_severity,
                       const BufferMeterPalette& palette);

} // namespace helix::ui
