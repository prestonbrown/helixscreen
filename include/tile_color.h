// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "json_fwd.h"

#include <lvgl/lvgl.h>

#include <cstddef>
#include <cstdint>

/// A home tile's user colour: the palette its configure UI offers, the subjects
/// the shared tile_color_grid component binds, and the rule for when a tile
/// paints with that colour. The favourite-macro and power-device tiles share it.
namespace helix::tile_color {

/// Number of palette entries. The last one is 0, "no colour": the tile keeps
/// its theme colours.
size_t palette_size();

/// The RGB of palette entry @p index (0 = no colour), or 0 when out of range.
uint32_t palette_at(int index);

/// The index of @p rgb in the palette, or -1.
int palette_index(uint32_t rgb);

/// The colour stored under "color" in a tile's config: an RGB integer, with
/// 0 (or a missing, mistyped or out-of-range value) meaning no colour.
uint32_t color_from_config(const nlohmann::json& config);

/// What a tile shows in the place its colour goes.
enum class TileState {
    Unconfigured, ///< nothing assigned yet
    Active,       ///< configured and on (a macro tile is always Active)
    Inactive,     ///< configured and off
    Locked,       ///< configured but not operable right now
};

/// How a tile paints its icon and badge.
enum class Paint {
    Theme,       ///< the tile's own theme colours for that state
    Custom,      ///< the user's colour
    CustomMuted, ///< the user's colour, toned down so off still reads as off
};

/// The paint for a tile with user colour @p rgb (0 = none) in @p state. The
/// user's colour never replaces the unconfigured or locked look.
Paint paint_for(uint32_t rgb, TileState state);

/// @p custom pulled toward @p muted_ref (the theme's muted text colour), so an
/// inactive tile keeps its hue but drops to the weight of the theme's off state.
lv_color_t muted(lv_color_t custom, lv_color_t muted_ref);

/// Publish tile_color_swatch_color_<i> and tile_color_selected. Process
/// lifetime; idempotent. Call before parsing XML that uses tile_color_grid.
void register_subjects();

/// Fill the swatches (the "no colour" entry shows the theme's secondary colour)
/// and ring @p rgb's swatch. Call when a configure UI opens.
void publish_palette(uint32_t rgb);

/// Ring @p rgb's swatch, or none when it is not in the palette.
void publish_selection(uint32_t rgb);

} // namespace helix::tile_color
