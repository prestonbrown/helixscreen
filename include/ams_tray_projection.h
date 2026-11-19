// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ams_tray_projection.h
 * @brief One camera for the AMS detail unit: spools, their box and its glass lid.
 *
 * x runs along the slot row, y is screen-down at the front plane, z is depth
 * from the front wall (0) to the back wall (DZ). A point at depth z draws
 * kDepthSkew * z to the right and RISE * z / DZ higher, the same skew the spool
 * flanges use, so the spools stand in the box rather than float in front of it.
 */

#include "ams_types.h"
#include "ams_unit_shell.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace helix::ui::tray {

inline constexpr float kDepthSkew = 0.45f;         // sideways shift per unit of depth
inline constexpr float kSpoolFlangeRadius = 0.42f; // flange vertical radius / spool size
inline constexpr float kBoxDepthMargin = 0.10f;    // box depth: a spool diameter plus 10%
inline constexpr float kDepthRise = 0.09f;         // back edges rise this share of the depth
inline constexpr float kSpoolFloorGap = 2.0f;      // flange bottom above the floor's front edge
inline constexpr float kLidGap = 1.0f;             // lid crest above the spool tops
inline constexpr float kSpoolWidth = 0.294f;       // flange-to-flange width / spool size
// Lane pitch over flange-to-flange width: an AMS, ACE Pro or QIDI Box stands its
// 67 mm wide spools about 90 mm apart.
inline constexpr float kPackedPitch = 1.35f;

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
/// Slot pitch for a spool size: the spool's width at the real units' packing, so
/// neighbouring flanges overlap in the drawing as they do in a photo.
float spool_pitch(float spool_size);
/// Front-plane distance from an end spool's centre to the unit's end: the spool's half
/// width plus the shell's clearance, or a flange's depth for the generic box.
float end_cap_width(const UnitShell* shell, float spool_size);
PointF proj(const TrayBox& b, float x, float y, float z);
/// Drawn width of a unit of @p lanes spools of @p spool_size: the pitches between its
/// lanes, a generic end cap each side and the right side face.
float unit_width(int lanes, float spool_size);
/// The largest spool size, at most @p max_size, whose unit of @p lanes fits @p width.
int32_t fit_spool_size(float width, int lanes, int32_t max_size);

/// Point @p i of the body's top edge, on the side plane at @p x.
PointF cheek_point(const TrayBox& b, const UnitShell& shell, float spool_size, float x, int i);
/// The screen box of one front part, for the lane at front-plane @p lane_x (ignored unless
/// the part repeats per lane): x0, y0, x1, y1.
void front_part_box(const TrayBox& b, const FrontPart& part, float spool_size, float lane_x,
                    float out[4]);
/// @p n points of a unit's real lid profile at plane @p x, front to back.
void shell_cap_polyline(const TrayBox& b, const UnitShell& shell, float spool_size, float x,
                        PointF* out, int n);
/// Sample @p i of @p n of a unit's lid profile, front to back: its depth share and height.
ProfilePoint shell_cap_sample(const UnitShell& shell, int i, int n);
/// The samples (of @p n) where the lid's flat top starts and ends, front to back, judged by the
/// profile's height: on screen the back of a level top stands higher than its front.
bool shell_lid_top_run(const UnitShell& shell, int n, int& first, int& last);
/// Where the body's top edge lies under @p upper, a point of the lid's end at plane @p x.
PointF cheek_under(const TrayBox& b, const UnitShell& shell, float spool_size, float x,
                   PointF upper);

struct TrayFaces {
    PointF back_wall[4], floor[4], left_wall[4], front[4], right_side[4];
};
TrayFaces tray_faces(const TrayBox& b);

/// One edge of the tray box, between two of its corners.
struct Edge {
    PointF a, b;
};
/// The edges that stand in front of the spools: the front face, the right side and the back's
/// right corner.
std::array<Edge, 7> near_edges(const TrayFaces& f);
/// The far wall's top and floor edges, which run behind the leftmost spool.
std::array<Edge, 2> far_edges(const TrayFaces& f);

/// Front-plane centre y of a spool standing on the floor.
float spool_front_cy(const TrayBox& b, float flange_ry);
/// Screen centre of a spool at front-plane x @p slot_x, drawn at mid-depth.
PointF spool_center(const TrayBox& b, float slot_x, float flange_ry);

/// Lid height over the chord from the front-wall top to the back-wall top,
/// so the crest at mid-depth clears the spool tops by kLidGap.
float lid_height(const TrayBox& b, float flange_ry);
/// A point of the lid's end-cap profile at @p x_end; theta runs pi (front) to 0 (back).
PointF cap_point(const TrayBox& b, float lid_h, float x_end, float theta);
/// @p n points of the cap profile, front to back. Returns n.
int cap_polyline(const TrayBox& b, float lid_h, float x_end, PointF* out, int n);
/// Highest drawn y of the unit: the lid silhouette's top, or the back-wall top.
float unit_top_y(const TrayBox& b, float lid_h, bool has_lid);

/// Top-left of lane @p lane_x's number badge on the box front. It sits inside the
/// front face, twice @p inset above its bottom edge, with its right edge @p inset inside
/// the lane's bay (lane_x + @p lane_half); where the pitch is too tight for that,
/// centered on the lane instead.
PointF lane_badge_origin(const TrayBox& b, float lane_x, float lane_half, float badge_w,
                         float badge_h, float inset);

/// Left edge of the environment pill that stands against the box's right end:
/// the box's back-right corner, where its right side face ends.
float pill_left_x(const TrayBox& b);
/// The same, moved right as far as needed to stand clear of @p clear_of.
float pill_left_x(const TrayBox& b, float clear_of);

/// Dots every @p step px along the polyline @p pts (@p n points), the spacing carried
/// across its corners, each rounded to a whole pixel. Writes at most @p max and
/// returns the count.
int leader_dots(const PointF* pts, int n, float step, PointF* out, int max);

enum class LidMode : uint8_t { None, Unit, PerLane };
/// Any climate data gets glass: a unit reading or a dryer covers the row, per-lane
/// readings get a lid per lane. No physical tray, no lid.
LidMode lid_mode(const AmsUnit& unit, bool has_physical_tray, bool dryer_supported);
/// A drawn unit's lids: none without a dome, else one over the row or one per lane.
LidMode lid_mode(const UnitShell& shell);
/// Per-lane lid half width: each lid's right cap tucks halfway behind the next lid.
float lane_lid_half_width(float slot_spacing, const TrayBox& b);
/// How far a spool's flanges stand above the box's front bottom edge: a hair, or a drawn
/// unit's base below them.
float spool_floor_gap(const UnitShell* shell, float shell_scale);

// The lid's sheen: one diffuse band, drawn as one horizontal gradient per row.
inline constexpr int kSheenMaxRows = 25;
inline constexpr float kSheenThetaDeg = 112.0f; // profile angle of the band
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
