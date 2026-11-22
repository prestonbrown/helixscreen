// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_spool_canvas.h"

#include "lvgl/lvgl.h"
#include "spool_spin.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace helix::ui {

static constexpr int32_t kDefaultSize = 64;
static constexpr uint32_t kDefaultColor = 0xE0E0E0; // Default white/light filament

enum class SpoolGlow : uint8_t { None, Outline, Halo };

struct SpoolCacheKey {
    uint32_t color_rgb; // 0x00RRGGBB
    uint16_t size;
    uint8_t fill_bucket; // 0..kFillBuckets
    SpoolGlow glow;
    uint32_t filament_hash = 0; // multi-color list, finish, translucency; 0 for plain
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
    bool translucent = false; // the filament is drawn over the swatch checkerboard
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

/// The most bytes of rendered spools the render cache keeps. Where spools turn,
/// it holds a full turn of a large tray spool (24 vent phases of 36 KB at 96 px)
/// beside a screen of still ones; where they never turn (@p reduced_effects), only
/// a few still renders, since every canvas keeps its own pixels.
size_t spool_render_cache_budget(bool reduced_effects);

/// Bytes of rendered pixels the render cache holds now.
size_t spool_render_cache_bytes();

} // namespace helix::ui
