// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ui_toolhead_view.h
 * @brief The configured toolhead, drawn: the one glyph every screen shares.
 *
 * Draws the head in the user's Toolhead Style (Stealthburner, Bambu, A4T, the
 * Creality heads...) the way the AMS panel's path does. Canvases that place the
 * head inside a larger picture call draw_toolhead() at their own position and
 * scale; anything that just wants the head in a box asks fit_toolhead(), or
 * uses the `<toolhead_view>` XML widget, which fits it to the widget's size.
 */

#include "ui_toolhead_badge.h"

#include "lvgl/lvgl.h"
#include "settings_manager.h"

#include <cstdint>
#include <optional>

namespace helix::ui {

/// Where and how large to draw a head so it fills @p box: the scale for
/// draw_toolhead() and the centre it is drawn at.
struct ToolheadFit {
    int32_t cx;
    int32_t nozzle_y;
    int32_t scale;
};

/// The scale @p style's glyph is drawn at for base scale @p scale: the A4T
/// glyph is drawn 6/5 larger than the others wherever a head is drawn.
[[nodiscard]] int32_t toolhead_drawn_scale(ToolheadStyle style, int32_t scale);

/// The largest scale (at most @p max_scale, at least 1) at which @p style's
/// glyph fits inside @p box, centred in it.
[[nodiscard]] ToolheadFit fit_toolhead(ToolheadStyle style, const lv_area_t& box,
                                       int32_t max_scale = 1000);

} // namespace helix::ui

/// Toolhead glyph in the user's configured style. Applies the A4T style's 6/5
/// scale boost internally: pass the base scale. @p filament is the colour at
/// the nozzle, or nullopt when nothing is loaded there.
void draw_toolhead(lv_layer_t* layer, int32_t cx, int32_t cy, std::optional<lv_color_t> filament,
                   int32_t scale, lv_opa_t opa = LV_OPA_COVER);

/// Drawn bounds of draw_toolhead()'s glyph at (@p cx, @p cy) for the
/// configured style, at the same base scale.
helix::ui::GlyphBounds toolhead_glyph_bounds(int32_t cx, int32_t cy, int32_t scale);

/// The configured head fitted to @p box (fit_toolhead() for the configured style).
void draw_toolhead_fitted(lv_layer_t* layer, const lv_area_t& box,
                          std::optional<lv_color_t> filament, int32_t max_scale = 1000,
                          lv_opa_t opa = LV_OPA_COVER);

/// Registers the `<toolhead_view>` XML widget: the configured head, fitted to
/// the widget's content box, redrawn when the Toolhead Style setting changes.
void ui_toolhead_view_register();
