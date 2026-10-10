// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

/**
 * @file filament_path_geometry.h
 * @brief Pure-geometry foundation for AMS filament-path rendering.
 *
 * Orthogonal lane routing: straight vertical runs joined by true quarter-circle
 * arc fillets. This module is intentionally LVGL-free (plain floats) so it can
 * be unit-tested headlessly; the canvas renderer (filament_tube_stroker)
 * consumes the resulting ARC segments by walking their exact float
 * parametrization as a fan of short straight chords (lv_draw_line), so arc and
 * line bands stay flush at every joint.
 *
 * Coordinate system: SCREEN coordinates, +y is DOWN.
 *   - Angle 0 = +x axis.
 *   - Positive angles rotate toward +y (standard atan2 in screen space).
 *   - Positive sweep = CLOCKWISE on screen.
 */

namespace helix {
namespace ui {
namespace pathgeo {

struct PathPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct PathSeg {
    enum Type { LINE, ARC };
    Type type = LINE;

    // LINE: p0 -> p1
    PathPoint p0;
    PathPoint p1;

    // ARC: circle center, centerline radius, start angle, signed sweep (radians).
    // A point on the arc at parameter t in [0, |sweep|] is:
    //   angle = start_angle + sign(sweep) * t
    //   (cx + r*cos(angle), cy + r*sin(angle))
    PathPoint center;
    float radius = 0.0f;
    float start_angle = 0.0f;
    float sweep = 0.0f;
};

struct FilamentPath {
    static constexpr int MAX_SEGS = 16;

    PathSeg segs[MAX_SEGS];
    int count = 0;

    /// Append a straight segment. Zero-length lines are skipped.
    void add_line(float x0, float y0, float x1, float y1);

    /// Append an arc segment (center, centerline radius, start angle, signed sweep).
    void add_arc(float cx, float cy, float r, float a0, float sweep);

    void clear();
};

/// LINE: euclidean distance. ARC: |sweep| * radius.
float seg_length(const PathSeg& s);

/// Sum of all segment lengths.
float path_length(const FilamentPath& p);

/**
 * @brief Point at arc-length distance @p d along the whole path.
 *
 * @p d is clamped to [0, total_length]. If @p tangent_out is non-null it
 * receives the unit tangent direction (pointing in the direction of increasing
 * arc length) at that point.
 */
PathPoint path_point_at(const FilamentPath& p, float d, PathPoint* tangent_out = nullptr);

/// Split @p p at arc-length @p d (clamped to the path): @p head gets the part
/// before it, @p tail the rest, each continuing where the other stops. A
/// segment the cut falls inside becomes one piece on either side.
void split_path(const FilamentPath& p, float d, FilamentPath& head, FilamentPath& tail);

/**
 * @brief Orthogonal lane routing from (x0,y0) DOWN to (x1,y1), y1 > y0.
 *
 * Produces: vertical drop -> quarter-arc fillet -> horizontal run at mid-height
 * -> quarter-arc fillet -> vertical drop into (x1,y1). Segments are appended to
 * @p out (it is NOT cleared first).
 *
 * @p fillet_r is the desired centerline fillet radius; the effective radius is
 * clamped to r_eff = min(fillet_r, |dx|/2, dy/2).
 *
 * Degenerate cases:
 *   - y1 <= y0     -> single straight LINE p0->p1 (defensive fallback)
 *   - |dx| < 2.0   -> single vertical LINE
 *   - r_eff < 2.0  -> 45-degree jog: vertical, diagonal LINE, vertical (no arcs)
 */
void route_orthogonal(FilamentPath& out, float x0, float y0, float x1, float y1, float fillet_r);

/**
 * @brief General filleted-polyline routing through arbitrary waypoints.
 *
 * Appends LINE segments through @p pts (n points) with a tangent circular-arc
 * fillet inserted at each interior vertex. Unlike route_orthogonal this does NOT
 * assume perpendicular bends — corners between non-orthogonal segments are
 * filleted with standard corner-fillet math. Segments are appended to @p out (it
 * is NOT cleared first).
 *
 * At interior vertex V with incoming unit direction d1 and outgoing unit
 * direction d2:
 *   - interior half-angle alpha = half the angle between -d1 and d2,
 *   - tangent trim back along each leg t = r_eff / tan(alpha),
 *   - arc center sits r_eff / sin(alpha) from V along the inward bisector,
 *   - sweep magnitude = pi - 2*alpha, sign from cross(d1, d2) in screen coords
 *     (+y down): positive cross => clockwise on screen => positive sweep
 *     (matching the ARC convention so path_point_at walks it forward).
 *
 * Per-vertex r_eff is clamped so the trim never exceeds half of either adjoining
 * segment length. Collinear vertices (cross ~ 0) get no fillet (the line simply
 * continues); duplicate / degenerate-short legs fall back to plain lines.
 *
 * Degenerate cases:
 *   - n < 2          -> nothing appended
 *   - n == 2         -> single straight LINE pts[0]->pts[1]
 *
 * @p fillet_r is the desired centerline fillet radius (clamped per vertex).
 */
void route_polyline_filleted(FilamentPath& out, const PathPoint* pts, int n, float fillet_r);

/**
 * @brief Input descriptor for one lane feeding a hub merge.
 *
 * @p slot_x is the lane's source column (the x it drops from). @p start_y is the
 * y where the diagonal phase begins (just below the lane's sensor / clearance).
 */
struct MergeLaneIn {
    float slot_x = 0.0f;
    float start_y = 0.0f;
};

/// Output waypoints for one merge lane: the 4-point polyline
/// (slot_x,start_y) -> (slot_x,y_bend) -> (entry_x,approach_y) -> (entry_x,hub_top)
/// ready to feed route_polyline_filleted.
struct MergeLaneOut {
    PathPoint pts[4];
};

/// Fit the hub width to the perpendicular spacing of its incoming tubes, not
/// just the horizontal spacing of their entries. Sources are ordered left to
/// right, as in a spool row. Returns a width in [min_width, max_width]; if the
/// available bounds cannot provide min_separation, returns max_width.
float merge_fan_width(const MergeLaneIn* lanes, int n, float hub_cx, float hub_top, float min_width,
                      float max_width, float entry_margin, float fillet_r, float max_slope,
                      float min_separation, int reserve_each_side = 0);

/// What a fan build worked out beyond the lane routes: the numbers a route that
/// joins the fan without being one of its lanes needs to stay parallel to it.
struct MergeFanInfo {
    float approach_y = 0.0f;  ///< Y every lane's last vertical drops from
    float min_bend_y = 0.0f;  ///< highest Y a diagonal may start at
    float slope_left = 0.0f;  ///< dy/dx common to the diagonals landing left of the center
    float slope_right = 0.0f; ///< the same for the right side
    float entry_left = 0.0f;  ///< X of the entry reserved left of lane 0 (reserve > 0)
    float entry_right = 0.0f; ///< X of the entry reserved right of the last lane
    float entry_step = 0.0f;  ///< distance between neighboring entries
};

/**
 * @brief Hub merge fan with parallel diagonals per side.
 *
 * Computes centerline routes for @p n lanes converging onto a
 * hub box. Lanes on each side of the hub center (left: slot_x < hub_cx, right:
 * mirrored) all run their long diagonal at ONE common slope, so parallel runs
 * cannot converge — perpendicular separation = entry_spacing * sin(atan(m_side)),
 * which can be much smaller than the entry spacing on a short canvas. Callers
 * must allow enough width/height for their stroke gauge (see merge_fan_width).
 *
 * Construction:
 *   - Entry x positions spread evenly across the hub box top edge using most of
 *     its width (leaving ~@p entry_margin px inside each end), ordered identically
 *     to lane order (no crossings).
 *   - Per side, m_side = min(@p max_slope, vertical_budget / max_dx_side), where
 *     vertical_budget = approach_y - min_bend_y and max_dx_side is the largest
 *     |entry_x - slot_x| on that side.
 *   - Fillet room is reserved as a geometry constraint so corners can't starve:
 *     both the final vertical (approach_y -> hub_top) and every first vertical
 *     (start_y -> y_bend) are kept >= fillet_r + 4px. Concretely
 *     approach_y = hub_top - (fillet_r + 4) and min_bend_y = deepest start_y +
 *     (fillet_r + 4). The common slope is recomputed from this reduced budget so
 *     the reservation holds even for the steepest lane.
 *   - lane i: y_bend_i = approach_y - m_side * |entry_x_i - slot_x_i|, clamped to
 *     >= max(min_bend_y, start_y_i + fillet_r + 4). A lane directly above its
 *     entry (dx ~ 0) is a straight vertical (y_bend == approach_y;
 *     route_polyline_filleted handles collinear).
 *
 * @param lanes      lane sources (size @p n), ordered left-to-right by slot index.
 * @param n          lane count.
 * @param hub_cx     hub center x.
 * @param hub_top    y of the hub box top edge (the verticals' final destination).
 * @param hub_w      hub box width (entries spread across this minus margins).
 * @param entry_margin inset from each hub-top end for the outermost entries.
 * @param fillet_r   fillet radius the caller will route with (sets approach gap).
 * @param max_slope  per-side slope ceiling (1.2 keeps angles sane on tall hubs).
 * @param out        per-lane waypoints (size @p n).
 * @param reserve_each_side entry positions left empty at each end of the hub top: the entries
 *                   spread over n + 2 * reserve positions and the lanes take the middle n.
 *                   A route that wants its own entry beside the outermost lanes finds it in
 *                   @p info.
 * @param info       optional: the fan's shared numbers.
 */
void build_merge_fan(const MergeLaneIn* lanes, int n, float hub_cx, float hub_top, float hub_w,
                     float entry_margin, float fillet_r, float max_slope, MergeLaneOut* out,
                     int reserve_each_side = 0, MergeFanInfo* info = nullptr);

/// One straight piece of a dash. A dash that crosses a bend of the polyline is two or more
/// pieces sharing @p dash, each ending where the next begins.
struct DashPiece {
    PathPoint a;
    PathPoint b;
    int dash = 0; ///< index of the dash this piece belongs to
};

/**
 * @brief Cut a polyline into dashes with ONE phase along its whole length.
 *
 * Starts with a dash and alternates @p dash_len on, @p gap_len off, measured along the
 * path, so the pattern flows through every bend instead of restarting on each segment. The
 * last dash is clipped where the path ends; a path shorter than one dash gives one partial
 * dash. Fewer than two points, a zero-length path or a non-positive @p dash_len give none.
 * A non-positive @p gap_len draws the path solid.
 */
std::vector<DashPiece> dash_polyline(const PathPoint* pts, int n, float dash_len, float gap_len);

} // namespace pathgeo
} // namespace ui
} // namespace helix
