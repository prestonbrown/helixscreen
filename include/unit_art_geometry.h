// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file unit_art_geometry.h
 * @brief Where a baked unit drawing's spools stand, from the bake's lane geometry.
 *
 * Coordinates are shares of the drawing's width from its top-left corner, as
 * ArtLaneGeom stores them. A spool is every disc of radius 1 in its flange basis between
 * its two flange centers, so one distance answers both "is this pixel on the spool" (radius
 * 1) and "is it on the wound filament" (the wound radius).
 */

#include "ams_unit_shell.h"

namespace helix::ui::tray {

/// The wound filament's radius at @p fill (0..1) between the bare @p core and the @p full
/// spool, the one rule the drawn spool and the baked art share.
float wound_radius(float core, float full, float fill);

/// Distance, in flange radii, from (@p x, @p y) to @p g's spool axis.
float art_lane_distance(const ArtLaneGeom& g, float x, float y);

/// Distance, in flange radii, from (@p x, @p y) to the center of @p g's near flange (or its
/// far one): within 1, the point is on that flange's disc.
float art_lane_flange_distance(const ArtLaneGeom& g, float x, float y, bool near_flange);

/// Coverage (0..1) of the pixel at (@p x, @p y) by @p g's cylinder of radius @p radius,
/// softened over @p px_per_unit pixels per flange radius.
float art_lane_coverage(const ArtLaneGeom& g, float x, float y, float radius, float px_per_unit);

/// A flange face's turn in its own plane (u toward the back, v up), positive from u toward v.
struct FlangeTurn {
    float c = 1.0f, s = 0.0f;
};

/// The turn of @p degrees.
FlangeTurn flange_turn(float degrees);

/// A point (@p u, @p v) of a face turned by @p turn, mapped back to where it stood before the
/// turn: what the unturned face shows there. The front edge of a face turned positively shows
/// what stood above it.
inline void flange_unturn(const FlangeTurn& turn, float u, float v, float& rest_u, float& rest_v) {
    rest_u = u * turn.c + v * turn.s;
    rest_v = v * turn.c - u * turn.s;
}

/// Pixels per flange radius at a drawing @p drawn_w pixels wide.
float art_lane_px_per_unit(const ArtLaneGeom& g, float drawn_w);

/// Whether the unit view can stand its slots on @p geom's lanes: each lane's outlet has a column
/// of its own, and each lane's tap band (ams_slot_lane_hit_band) holds its spool's middle and its
/// outlet. A drawing that fails keeps the drawn tray.
bool unit_view_fits(const UnitArtGeom& geom);

} // namespace helix::ui::tray
