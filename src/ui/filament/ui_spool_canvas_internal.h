// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_spool_canvas.h"

#include "lvgl/lvgl.h"
#include "spool_spin.h"

#include <cstdint>
#include <vector>

static constexpr int32_t kDefaultSize = 64;
static constexpr uint32_t kDefaultColor = 0xE0E0E0; // Default white/light filament

enum class SpoolGlow : uint8_t { None, Outline, Halo };

struct SpoolCacheKey {
    uint32_t color_rgb; // 0x00RRGGBB
    uint16_t size;
    uint8_t fill_bucket; // 0..kFillBuckets
    SpoolGlow glow;
    uint32_t filament_hash = 0; // multi-colour list and finish; 0 for a plain single colour
    helix::ui::SpoolView view = helix::ui::SpoolView::Side;
    uint8_t vent_phase = 0; // rotation of the vents; 0 unless the spool has turned

    bool operator==(const SpoolCacheKey& o) const {
        return color_rgb == o.color_rgb && size == o.size && fill_bucket == o.fill_bucket &&
               glow == o.glow && filament_hash == o.filament_hash && view == o.view &&
               vent_phase == o.vent_phase;
    }
};

struct SpoolCanvasData {
    lv_obj_t* canvas = nullptr;
    lv_draw_buf_t* draw_buf = nullptr;
    int32_t size = kDefaultSize;
    lv_color_t color = lv_color_hex(kDefaultColor);
    float fill_level = 1.0f;
    /// A multi-colour filament's colours (two or more), else empty: the body
    /// is split the way every swatch splits (helix::ui::swatch_split_thresholds).
    std::vector<lv_color_t> colors;
    helix::ui::SwatchFinish finish = helix::ui::SwatchFinish::Plain;
    helix::ui::SpoolView view = helix::ui::SpoolView::Side;
    helix::ui::SpoolSpin spin;
    int vent_phase = 0; ///< current rotation, kept across a stop so the vents hold still

    // Last-rendered key, for dedup (idea #1). Valid only when has_rendered.
    SpoolCacheKey last_key{0, 0, 0, SpoolGlow::None};
    bool has_rendered = false;
};

/// The canvas data registered for @p obj, nullptr when it has none.
SpoolCanvasData* spool_canvas_data(lv_obj_t* obj);

/// Paint the spool's pixels; false when the draw buffer is unavailable.
bool redraw_spool_pixels(SpoolCanvasData* data);
