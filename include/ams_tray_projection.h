// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ams_tray_projection.h
 * @brief One camera for the AMS detail unit: spools, their box and its glass lid.
 *
 * x runs along the slot row, y is screen-down at the front plane, z is depth
 * from the front wall (0) to the back wall (DZ). A point at depth z draws
 * DEPTH_SKEW * z to the right and RISE * z / DZ higher, the same skew the spool
 * flanges use, so the spools stand in the box rather than float in front of it.
 */

#include "ams_types.h"

#include <cstdint>

namespace helix::ui::tray {

inline constexpr float DEPTH_SKEW = 0.45f;          // sideways shift per unit of depth
inline constexpr float SPOOL_FLANGE_RADIUS = 0.42f; // flange vertical radius / spool size
inline constexpr float BOX_DEPTH_MARGIN = 0.10f;    // box depth: a spool diameter plus 10%
inline constexpr float DEPTH_RISE = 0.09f;          // back edges rise this share of the depth
inline constexpr float SPOOL_FLOOR_GAP = 2.0f;      // flange bottom above the floor's front edge
inline constexpr float LID_GAP = 1.0f;              // lid crest above the spool tops
inline constexpr float SPOOL_WIDTH = 0.35f;         // flange-to-flange width / spool size
inline constexpr float PITCH_CLEARANCE = 16.0f;     // free space between neighbouring spools

struct PointF {
    float x = 0;
    float y = 0;
};

/// Front face (fl..fr, ft..fb), depth DZ, RISE of the back edges, and EB, the
/// back wall's extra height over the front wall.
struct TrayBox {
    float fl, fr, ft, fb, depth, rise, back_extra;
};

float box_depth(float spool_size);
float box_rise(float depth);
/// Slot pitch for a spool size: its footprint, half the lid-cap tuck, and the clearance.
float spool_pitch(float spool_size);
PointF proj(const TrayBox& b, float x, float y, float z);

struct TrayFaces {
    PointF back_wall[4], floor[4], left_wall[4], front[4], right_side[4];
};
TrayFaces tray_faces(const TrayBox& b);

/// Front-plane centre y of a spool standing on the floor.
float spool_front_cy(const TrayBox& b, float flange_ry);
/// Screen centre of a spool at front-plane x @p slot_x, drawn at mid-depth.
PointF spool_center(const TrayBox& b, float slot_x, float flange_ry);

/// Lid height over the chord from the front-wall top to the back-wall top,
/// so the crest at mid-depth clears the spool tops by LID_GAP.
float lid_height(const TrayBox& b, float flange_ry);
/// A point of the lid's end-cap profile at @p x_end; theta runs pi (front) to 0 (back).
PointF cap_point(const TrayBox& b, float lid_h, float x_end, float theta);
/// @p n points of the cap profile, front to back. Returns n.
int cap_polyline(const TrayBox& b, float lid_h, float x_end, PointF* out, int n);
/// Highest drawn y of the unit: the lid silhouette's top, or the back-wall top.
float unit_top_y(const TrayBox& b, float lid_h, bool has_lid);

enum class LidMode : uint8_t { None, Unit, PerLane };
/// Any climate data gets glass: a unit reading or a dryer covers the row, per-lane
/// readings get a lid per lane. No physical tray, no lid.
LidMode lid_mode(const AmsUnit& unit, bool has_physical_tray, bool dryer_supported);
/// Per-lane lid half width: each lid's right cap tucks halfway behind the next lid.
float lane_lid_half_width(float slot_spacing, const TrayBox& b);

// The lid's sheen: one diffuse band, drawn as one horizontal gradient per row.
inline constexpr int SHEEN_MAX_ROWS = 25;
inline constexpr float SHEEN_THETA_DEG = 112.0f; // profile angle of the band
struct SheenRow {
    float y;
    uint8_t opa;
};
/// Horizontal fade: in from x0 to full0, full strength to full1, out to x1.
struct SheenSpan {
    float x0, full0, full1, x1;
};
/// Rows of the band for a lid over [b.fl, b.fr] at peak opacity @p alpha (0..1).
/// Returns the row count (0 when the lid is too short to fade in and out).
int sheen_rows(const TrayBox& b, float lid_h, float alpha, SheenSpan& span, SheenRow* rows);

} // namespace helix::ui::tray
