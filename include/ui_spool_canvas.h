// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_spool_canvas.h
 * @brief Pseudo-3D filament spool visualization using LVGL canvas
 *
 * Creates a visually appealing 3D-style spool with approximately 20 degrees
 * rotation, showing a front face ellipse plus visible side depth.
 *
 * XML usage:
 * @code{.xml}
 * <spool_canvas color="0xFF5722" fill_level="0.75" size="64"/>
 * @endcode
 *
 * Properties:
 *   - color: Filament color as hex (e.g., "0xFF5722")
 *   - fill_level: Amount of filament 0.0 (empty) to 1.0 (full)
 *   - size: Canvas size in pixels (default 64)
 */

void ui_spool_canvas_register(void);

/**
 * @brief Create a spool canvas programmatically
 *
 * Alternative to XML creation for use in C++ code.
 *
 * @param parent Parent LVGL object
 * @param size Canvas size in pixels (default 64 if 0)
 * @return Created canvas object, or NULL on failure
 */
lv_obj_t* ui_spool_canvas_create(lv_obj_t* parent, int32_t size);

void ui_spool_canvas_set_color(lv_obj_t* canvas, lv_color_t color);
void ui_spool_canvas_set_fill_level(lv_obj_t* canvas, float fill_level);
void ui_spool_canvas_set_size(lv_obj_t* canvas, int32_t size);
void ui_spool_canvas_redraw(lv_obj_t* canvas);
float ui_spool_canvas_get_fill_level(lv_obj_t* canvas);
lv_color_t ui_spool_canvas_get_color(lv_obj_t* canvas);

/**
 * @brief Clear the internal render cache.
 *
 * Rendered pixel buffers are cached keyed on (color, fill_bucket, size). The
 * cache uses theme-derived colors (flange, hub) captured at render time; if
 * the theme changes, cached entries would be stale. Call this on theme
 * changes to force re-render.
 */
void ui_spool_canvas_invalidate_cache(void);

#ifdef __cplusplus
}

#include "ui_swatch.h"

#include "filament_tube_stroker.h"
#include "spool_spin.h"

#include <string>

namespace helix::ui {

/**
 * @brief Paint the spool's filament: @p rgb, split into @p multi_color_hexes'
 *        colours when it lists two or more, in @p finish.
 *
 * The split, the matte colour and the silk, marble and sparkle textures are
 * the swatch painter's (ui_swatch.h), so a spool and its swatch agree.
 */
void spool_canvas_set_filament(lv_obj_t* canvas, uint32_t rgb, const std::string& multi_color_hexes,
                               SwatchFinish finish);

/// spool_canvas_set_filament() from a slot's colour, multi-colour list and finish.
void spool_canvas_set_filament(lv_obj_t* canvas, const SlotInfo& slot);

/// Which way a spool canvas looks at its spool: from the side (the pseudo-3D
/// drum between two flanges) or face-on (the vented front flange over the
/// wound filament).
enum class SpoolView : uint8_t { Side, Front };

/// Look at @p canvas's spool from @p view; redraws when it changes.
void spool_canvas_set_view(lv_obj_t* canvas, SpoolView view);

/// The view @p canvas draws.
SpoolView spool_canvas_get_view(lv_obj_t* canvas);

/**
 * @brief Turn @p canvas's spool as @p spin says; a direction of 0 stands it still.
 *
 * The vents step through pre-rendered phases held in the render cache, so a
 * turning spool costs a copy of its buffer per step, not a repaint, and its
 * glow never repaints. The spin ends with the canvas, and a hidden canvas does
 * not step. No-op on a spin already running.
 */
void spool_canvas_set_spin(lv_obj_t* canvas, const SpoolSpin& spin);

/**
 * @brief Create a glow layer: an image the caller places behind a spool,
 *        centered on it and sized like it.
 */
lv_obj_t* spool_glow_create(lv_obj_t* parent);

/**
 * @brief Light the glow in @p color for a silhouette of @p size px and keep it current.
 *
 * The silhouette is @p spool_canvas's own alpha, from the side or face-on.
 * The layer stays bound to the spool: it repaints when the spool renders a new shape, and stays
 * empty until the spool's first render. Capable hardware gets a soft halo plus a lighter tight
 * rim; @p simple a 2 px solid outline. The image overhangs the object by the glow margin,
 * reported as ext draw size, so layout never moves. One image per (shape, color, simple) is shared
 * by every layer showing it, from a small cache of its own. The current lane glows in
 * tube_accent(); a lane in error in its severity color.
 */
void spool_glow_paint(lv_obj_t* glow, lv_obj_t* spool_canvas, int32_t size, lv_color_t color,
                      bool simple = reduced_effects());

/// Unlight the layer and drop its image reference.
void spool_glow_clear(lv_obj_t* glow);
} // namespace helix::ui
#endif
