// SPDX-License-Identifier: GPL-3.0-or-later

#include "hub_buffer_meter.h"

#include "buffer_reading.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

constexpr int32_t kMeterPad = 6;

void fill(lv_layer_t* layer, const lv_area_t& a, lv_color_t color, int32_t radius) {
    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_draw_fill(layer, &dsc, &a);
}

} // namespace

float buffer_meter_position(float bias, int state) {
    if (bias > -1.5f)
        return std::clamp(bias, -1.0f, 1.0f);
    return state == 1 ? kSwitchedBufferBias : state == 2 ? -kSwitchedBufferBias : 0.0f;
}

BufferMeterTone buffer_meter_tone(bool fault, float position) {
    if (fault)
        return BufferMeterTone::Fault;
    return position > kBufferMeterOffCenter    ? BufferMeterTone::Compressed
           : position < -kBufferMeterOffCenter ? BufferMeterTone::Expanded
                                               : BufferMeterTone::Centered;
}

lv_color_t buffer_meter_fill(BufferMeterTone tone, const BufferMeterPalette& palette) {
    switch (tone) {
    case BufferMeterTone::Fault:
        return palette.error;
    case BufferMeterTone::Compressed:
        return palette.accent;
    case BufferMeterTone::Expanded:
        return palette.warning;
    case BufferMeterTone::Centered:
        break;
    }
    return palette.success;
}

int32_t buffer_meter_length(int32_t box_h) {
    return LV_MAX(16, box_h * 6 / 5);
}

int32_t buffer_meter_slot_width(int32_t box_h) {
    return buffer_meter_length(box_h) + 2 * kMeterPad;
}

void draw_buffer_meter(lv_layer_t* layer, const lv_area_t& slot, float position, int fault_severity,
                       const BufferMeterPalette& palette) {
    const int32_t h = lv_area_get_height(&slot);
    const int32_t len = LV_MIN(buffer_meter_length(h), lv_area_get_width(&slot) - 2);
    if (len < 6)
        return;
    const int32_t t = LV_MAX(6, h / 3);
    const int32_t cx = (slot.x1 + slot.x2) / 2;
    const int32_t cy = (slot.y1 + slot.y2) / 2;
    const lv_area_t track = {cx - len / 2, cy - t / 2, cx - len / 2 + len - 1, cy - t / 2 + t - 1};
    const int32_t thumb_x = cx + (int32_t)std::lround(position * (float)(len - 4) / 2.0f);
    const lv_color_t tone_fill = buffer_meter_fill(
        buffer_meter_tone(buffer_meter_faulted(fault_severity), position), palette);
    fill(layer, track, palette.track, 2);
    fill(layer, {track.x1, track.y1, thumb_x, track.y2}, tone_fill, 2);
    lv_draw_border_dsc_t border;
    lv_draw_border_dsc_init(&border);
    border.color = palette.border;
    border.width = 1;
    border.radius = 2;
    lv_draw_border(layer, &border, &track);
    fill(layer, {thumb_x - 1, track.y1 - 2, thumb_x + 1, track.y2 + 2}, palette.thumb, 1);
}

} // namespace helix::ui
