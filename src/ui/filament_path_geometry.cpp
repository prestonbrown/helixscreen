// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_path_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix {
namespace ui {
namespace pathgeo {

namespace {
constexpr float PI = 3.14159265358979323846f;
constexpr float HALF_PI = PI / 2.0f;

// Minimum horizontal offset before we bother routing around a corner.
constexpr float MIN_DX = 2.0f;
// Minimum effective fillet radius before arcs are worthwhile.
constexpr float MIN_FILLET = 2.0f;
// Extra straight room (px) reserved on each leg adjoining a merge-fan corner,
// on top of the fillet radius itself. route_polyline_filleted clamps the fillet
// trim to half the shorter adjoining leg, so a leg of length >= fillet_r + slack
// guarantees the corner can sweep at (close to) the full radius instead of
// collapsing to a starved arc or a sharp miter.
constexpr float FILLET_SLACK = 4.0f;
} // namespace

void FilamentPath::add_line(float x0, float y0, float x1, float y1) {
    if (count >= MAX_SEGS)
        return;
    // Skip zero-length lines.
    float dx = x1 - x0;
    float dy = y1 - y0;
    if (dx * dx + dy * dy <= 0.0f)
        return;
    PathSeg& s = segs[count++];
    s.type = PathSeg::LINE;
    s.p0 = {x0, y0};
    s.p1 = {x1, y1};
    s.center = {0.0f, 0.0f};
    s.radius = 0.0f;
    s.start_angle = 0.0f;
    s.sweep = 0.0f;
}

void FilamentPath::add_arc(float cx, float cy, float r, float a0, float sweep) {
    if (count >= MAX_SEGS)
        return;
    PathSeg& s = segs[count++];
    s.type = PathSeg::ARC;
    s.center = {cx, cy};
    s.radius = r;
    s.start_angle = a0;
    s.sweep = sweep;
    // p0/p1 left as defaults for ARC; consumers use center/angle.
    s.p0 = {cx + r * std::cos(a0), cy + r * std::sin(a0)};
    s.p1 = {cx + r * std::cos(a0 + sweep), cy + r * std::sin(a0 + sweep)};
}

void FilamentPath::clear() {
    count = 0;
}

float seg_length(const PathSeg& s) {
    if (s.type == PathSeg::LINE) {
        float dx = s.p1.x - s.p0.x;
        float dy = s.p1.y - s.p0.y;
        return std::sqrt(dx * dx + dy * dy);
    }
    return std::fabs(s.sweep) * s.radius;
}

float path_length(const FilamentPath& p) {
    float total = 0.0f;
    for (int i = 0; i < p.count; ++i)
        total += seg_length(p.segs[i]);
    return total;
}

PathPoint path_point_at(const FilamentPath& p, float d, PathPoint* tangent_out) {
    if (p.count <= 0) {
        if (tangent_out)
            *tangent_out = {0.0f, 0.0f};
        return {0.0f, 0.0f};
    }

    float total = path_length(p);
    if (d < 0.0f)
        d = 0.0f;
    if (d > total)
        d = total;

    float accum = 0.0f;
    for (int i = 0; i < p.count; ++i) {
        const PathSeg& s = p.segs[i];
        float len = seg_length(s);
        // Use the last segment for the trailing boundary so d == total resolves
        // to the final endpoint rather than falling through.
        bool last = (i == p.count - 1);
        if (d <= accum + len || last) {
            float into = d - accum;
            if (into < 0.0f)
                into = 0.0f;
            if (into > len)
                into = len;

            if (s.type == PathSeg::LINE) {
                float t = (len > 0.0f) ? (into / len) : 0.0f;
                PathPoint pt{s.p0.x + (s.p1.x - s.p0.x) * t, s.p0.y + (s.p1.y - s.p0.y) * t};
                if (tangent_out) {
                    float dx = s.p1.x - s.p0.x;
                    float dy = s.p1.y - s.p0.y;
                    float l = std::sqrt(dx * dx + dy * dy);
                    *tangent_out = (l > 0.0f) ? PathPoint{dx / l, dy / l} : PathPoint{0.0f, 0.0f};
                }
                return pt;
            }

            // ARC: angle advances at unit rate per centerline arc length.
            float sgn = (s.sweep >= 0.0f) ? 1.0f : -1.0f;
            float angle = s.start_angle + sgn * (s.radius > 0.0f ? into / s.radius : 0.0f);
            PathPoint pt{s.center.x + s.radius * std::cos(angle),
                         s.center.y + s.radius * std::sin(angle)};
            if (tangent_out) {
                // Forward tangent = d/dt of (cos,sin) scaled by sweep sign, unit length.
                *tangent_out = {-std::sin(angle) * sgn, std::cos(angle) * sgn};
            }
            return pt;
        }
        accum += len;
    }

    // Unreachable (last-segment branch handles d == total), but be defensive.
    const PathSeg& s = p.segs[p.count - 1];
    if (tangent_out)
        *tangent_out = {0.0f, 0.0f};
    // p1 is the segment endpoint for both LINE and ARC (add_arc precomputes the
    // ARC's end point into p1), so it's the correct trailing point either way.
    return s.p1;
}

void route_orthogonal(FilamentPath& out, float x0, float y0, float x1, float y1, float fillet_r) {
    float dx = x1 - x0;
    float dy = y1 - y0;

    // Defensive fallback: not strictly going down.
    if (dy <= 0.0f) {
        out.add_line(x0, y0, x1, y1);
        return;
    }

    // Negligible horizontal offset: a single vertical run.
    if (std::fabs(dx) < MIN_DX) {
        out.add_line(x0, y0, x1, y1);
        return;
    }

    float dir = (dx > 0.0f) ? 1.0f : -1.0f;
    float r_eff = std::min({fillet_r, std::fabs(dx) / 2.0f, dy / 2.0f});

    // Fillet too small to draw meaningful arcs: 45-degree jog (3 straight lines).
    if (r_eff < MIN_FILLET) {
        float y_a = y0 + dy / 3.0f;
        float y_b = y0 + 2.0f * dy / 3.0f;
        out.add_line(x0, y0, x0, y_a);  // vertical
        out.add_line(x0, y_a, x1, y_b); // diagonal
        out.add_line(x1, y_b, x1, y1);  // vertical
        return;
    }

    float ymid = y0 + dy / 2.0f;
    float r = r_eff;

    // Seg 0: vertical drop to the top of the first fillet.
    out.add_line(x0, y0, x0, ymid - r);

    // Arc 1: vertical -> horizontal fillet.
    // Center sits one radius horizontally toward the target and one radius up
    // from the mid-height line. Start angle / sweep chosen so the tangent leaves
    // downward and arrives horizontal toward the target.
    {
        float cx = x0 + dir * r;
        float cy = ymid - r;
        float a0 = (dir > 0.0f) ? PI : 0.0f;
        float sweep = -dir * HALF_PI;
        out.add_arc(cx, cy, r, a0, sweep);
    }

    // Seg 2: horizontal run at mid-height.
    out.add_line(x0 + dir * r, ymid, x1 - dir * r, ymid);

    // Arc 2: horizontal -> vertical fillet into the target.
    {
        float cx = x1 - dir * r;
        float cy = ymid + r;
        float a0 = -HALF_PI;
        float sweep = dir * HALF_PI;
        out.add_arc(cx, cy, r, a0, sweep);
    }

    // Seg 4: vertical drop into the target.
    out.add_line(x1, ymid + r, x1, y1);
}

void route_polyline_filleted(FilamentPath& out, const PathPoint* pts, int n, float fillet_r) {
    if (!pts || n < 2)
        return;
    if (n == 2) {
        out.add_line(pts[0].x, pts[0].y, pts[1].x, pts[1].y);
        return;
    }

    // Walking cursor: the current "pen" position. Starts at pts[0] and advances
    // to each fillet's start-trim point, then jumps to its end-trim point.
    PathPoint cursor = pts[0];

    for (int i = 1; i < n - 1; ++i) {
        const PathPoint V = pts[i];
        const PathPoint A = pts[i - 1]; // previous waypoint
        const PathPoint B = pts[i + 1]; // next waypoint

        // Incoming leg direction (A->V) and outgoing (V->B), as unit vectors.
        float in_dx = V.x - A.x, in_dy = V.y - A.y;
        float out_dx = B.x - V.x, out_dy = B.y - V.y;
        float in_len = std::sqrt(in_dx * in_dx + in_dy * in_dy);
        float out_len = std::sqrt(out_dx * out_dx + out_dy * out_dy);

        // Degenerate / duplicate waypoint (a zero-length leg). The vertex carries
        // no turn of its own, but the cursor must still advance THROUGH it so a
        // later real turn doesn't cut the corner back to the running cursor. Emit
        // the straight run cursor->V and move on.
        if (in_len < 1e-4f || out_len < 1e-4f) {
            out.add_line(cursor.x, cursor.y, V.x, V.y);
            cursor = V;
            continue;
        }

        float d1x = in_dx / in_len, d1y = in_dy / in_len;
        float d2x = out_dx / out_len, d2y = out_dy / out_len;

        // Turn measure. cross > 0 => clockwise on screen (+y down) => +sweep.
        float cross = d1x * d2y - d1y * d2x;
        float dot = d1x * d2x + d1y * d2y;

        // Collinear (no turn): no fillet, just continue the line.
        if (std::fabs(cross) < 1e-6f) {
            // Straight-through or 180 reversal — emit nothing here; the line to
            // the next trim point (or final point) covers it.
            continue;
        }

        // Interior half-angle alpha = half the angle between -d1 and d2.
        // angle(-d1, d2): cos = (-d1).d2 = -dot ; the turn angle theta between
        // legs has cos(theta) = dot, and alpha = (pi - theta)/2.
        float theta = std::atan2(std::fabs(cross), dot); // [0, pi]
        float alpha = (PI - theta) / 2.0f;               // (0, pi/2)

        float tan_a = std::tan(alpha);
        float sin_a = std::sin(alpha);
        if (tan_a < 1e-4f || sin_a < 1e-4f) {
            // Near-180 reversal with no resolvable bisector: treat as a sharp
            // corner. Advance the cursor through V so the corner isn't cut.
            out.add_line(cursor.x, cursor.y, V.x, V.y);
            cursor = V;
            continue;
        }

        // Trim length back along each leg for the desired radius.
        // Clamp r so the trim never exceeds half of either adjoining segment.
        // The incoming segment's available length is from the running cursor
        // (its real start may already be a previous fillet's end-trim point) to
        // V; the outgoing's is V to B.
        float avail_in =
            std::sqrt((V.x - cursor.x) * (V.x - cursor.x) + (V.y - cursor.y) * (V.y - cursor.y));
        float avail_out = out_len;
        float max_trim = std::min(avail_in, avail_out) * 0.5f;

        float r_eff = fillet_r;
        float t = r_eff / tan_a;
        if (t > max_trim) {
            t = max_trim;
            r_eff = t * tan_a;
        }

        // Below the meaningful-arc floor: leave a sharp corner. This is still a
        // real turn, so advance the cursor THROUGH V (emit cursor->V) — a plain
        // `continue` here would let the next emitted line run from the stale
        // cursor straight to the following trim point, cutting this corner off.
        if (r_eff < MIN_FILLET) {
            out.add_line(cursor.x, cursor.y, V.x, V.y);
            cursor = V;
            continue;
        }

        // Trim points on each leg.
        PathPoint t1{V.x - d1x * t, V.y - d1y * t}; // arc start (end of incoming line)
        PathPoint t2{V.x + d2x * t, V.y + d2y * t}; // arc end   (start of outgoing line)

        // Arc center: from V along the inward bisector at distance r/sin(alpha).
        // Inward bisector unit dir = normalize(d2 - d1) flipped to the inside of
        // the turn. Equivalently, center = t1 + r * (left/right normal of d1)
        // pointing toward the turn interior. Use the normal of d1 rotated toward
        // d2 (sign = sign(cross)).
        float sgn = (cross > 0.0f) ? 1.0f : -1.0f;
        // Normal of d1 rotated by +90 (screen, +y down): (-d1y, d1x). For a
        // clockwise turn (cross>0) the center is on that side; flip by sgn.
        float n1x = -d1y * sgn;
        float n1y = d1x * sgn;
        PathPoint center{t1.x + r_eff * n1x, t1.y + r_eff * n1y};

        // Emit the straight run from the running cursor to t1.
        out.add_line(cursor.x, cursor.y, t1.x, t1.y);

        // Arc start angle = angle of the radius vector center->t1. The signed
        // sweep magnitude is theta (= pi - 2*alpha, the central angle between the
        // two trim points), sign from the turn direction. Because t1/t2 are
        // symmetric about the bisector with exactly that central angle, the
        // parametrization start_angle + sign*t lands on t2 at t == theta.
        float a0 = std::atan2(t1.y - center.y, t1.x - center.x);
        float sweep = sgn * theta;

        out.add_arc(center.x, center.y, r_eff, a0, sweep);

        cursor = t2;
    }

    // Final straight run from the cursor into the last waypoint.
    out.add_line(cursor.x, cursor.y, pts[n - 1].x, pts[n - 1].y);
}

void build_merge_fan(const MergeLaneIn* lanes, int n, float hub_cx, float hub_top, float hub_w,
                     float entry_margin, float fillet_r, float max_slope, MergeLaneOut* out) {
    if (!lanes || !out || n <= 0)
        return;

    // Reserve straight room for the fillet on both fan corners. The final
    // vertical (approach_y -> hub_top) and the first vertical (start_y -> y_bend)
    // must each be at least this long so route_polyline_filleted's half-leg trim
    // clamp can't starve the corner down to a sharp miter (#... merge-fan notch).
    const float leg_reserve = fillet_r + FILLET_SLACK;

    // Common approach height above the hub top; every lane's short final vertical
    // drops from here into its own entry_x at one clean level. Pushed up by the
    // full reserve so the last leg is always >= leg_reserve.
    float approach_y = hub_top - leg_reserve;

    // Entry x positions spread evenly across the usable hub-top width (leaving
    // entry_margin inside each end), ordered identically to lane order so no two
    // lanes cross. Single lane lands dead-center on the hub.
    float usable = hub_w - 2.0f * entry_margin;
    if (usable < 0.0f)
        usable = 0.0f;
    float left_end = hub_cx - usable * 0.5f;
    float entry_step = (n > 1) ? usable / (float)(n - 1) : 0.0f;

    float entry_x[FilamentPath::MAX_SEGS];
    int count = std::min(n, FilamentPath::MAX_SEGS);
    for (int i = 0; i < count; ++i)
        entry_x[i] = (n == 1) ? hub_cx : (left_end + entry_step * (float)i);

    // The bends must sit below every lane's sensor/clearance: take the lowest
    // (largest-y) start_y as the ceiling so a bend never rises above its source.
    float deepest_start_y = lanes[0].start_y;
    for (int i = 1; i < count; ++i)
        deepest_start_y = std::max(deepest_start_y, lanes[i].start_y);
    // Reserve the full first-leg room (not just a token 2px): the bend can't rise
    // closer than leg_reserve below the deepest sensor, so its incoming vertical
    // always has enough straight run to host the fillet.
    float min_bend_y = deepest_start_y + leg_reserve;

    // Recompute the common slope from the REDUCED vertical budget (min_bend_y is
    // now higher and approach_y lower than before), so the steepest lane still
    // bends at or below min_bend_y and the reservation holds for every lane.
    float vertical_budget = approach_y - min_bend_y;
    if (vertical_budget < 0.0f)
        vertical_budget = 0.0f;

    // Per-side max horizontal travel (|entry_x - slot_x|). Left = entries that
    // land left of (or at) the hub center; right = the rest. Each side gets ONE
    // common slope so its diagonals stay parallel and never converge.
    float max_dx_left = 0.0f;
    float max_dx_right = 0.0f;
    for (int i = 0; i < count; ++i) {
        float dx = std::fabs(entry_x[i] - lanes[i].slot_x);
        if (entry_x[i] <= hub_cx) {
            if (dx > max_dx_left)
                max_dx_left = dx;
        } else {
            if (dx > max_dx_right)
                max_dx_right = dx;
        }
    }
    float m_left =
        (max_dx_left > 1e-3f) ? std::min(max_slope, vertical_budget / max_dx_left) : max_slope;
    float m_right =
        (max_dx_right > 1e-3f) ? std::min(max_slope, vertical_budget / max_dx_right) : max_slope;

    for (int i = 0; i < count; ++i) {
        float ex = entry_x[i];
        float sx = lanes[i].slot_x;
        float dx = std::fabs(ex - sx);
        float m = (ex <= hub_cx) ? m_left : m_right;

        // Bend height derived from this lane's own dx at the side's common slope.
        // Lanes farther from their entry bend higher (longer diagonal); a lane
        // directly above its entry (dx ~ 0) bends right at the approach -> a plain
        // vertical (route_polyline_filleted collapses the collinear waypoint).
        float y_bend = approach_y - m * dx;
        // Reserve the first-leg room for THIS lane: its incoming vertical
        // (start_y -> y_bend) must be at least leg_reserve long. min_bend_y already
        // enforces this for the deepest sensor; this per-lane floor covers any lane
        // whose own start_y sits below the common ceiling.
        float lane_bend_floor = std::max(min_bend_y, lanes[i].start_y + leg_reserve);
        if (y_bend < lane_bend_floor)
            y_bend = lane_bend_floor;
        if (y_bend > approach_y)
            y_bend = approach_y;

        out[i].pts[0] = {sx, lanes[i].start_y};
        out[i].pts[1] = {sx, y_bend};
        out[i].pts[2] = {ex, approach_y};
        out[i].pts[3] = {ex, hub_top};
    }
}

} // namespace pathgeo
} // namespace ui
} // namespace helix
