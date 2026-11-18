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

/// Coverage (0..1) of the pixel at (@p x, @p y) by @p g's cylinder of radius @p radius,
/// softened over @p px_per_unit pixels per flange radius.
float art_lane_coverage(const ArtLaneGeom& g, float x, float y, float radius, float px_per_unit);

/// Pixels per flange radius at a drawing @p drawn_w pixels wide.
float art_lane_px_per_unit(const ArtLaneGeom& g, float drawn_w);

} // namespace helix::ui::tray
