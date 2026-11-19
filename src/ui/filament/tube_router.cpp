// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tube_router.h"

#include <algorithm>
#include <cmath>

namespace helix {
namespace ui {
namespace tuberoute {

namespace {

// A trace whose sink sits closer than this to straight below its source drops
// straight, landing this much off its sink at most.
constexpr float kStraightEps = 0.5f;
// Slack allowed on every distance and height comparison.
constexpr float kTol = 0.01f;
// Chords each fillet arc is checked as; the chord error is far under a pixel.
constexpr int kArcChords = 8;
// Capacity of one flattened trace: three legs and two arcs.
constexpr int kMaxFlat = 3 + 2 * kArcChords + 4;
// Halvings when a jog is lowered until it clears its neighbor.
constexpr int kBisectIters = 14;
// Doublings of the search step before a jog is declared unclearable.
constexpr int kMaxDoublings = 12;

struct Seg {
    PathPoint a;
    PathPoint b;
};

// One trace's filleted shape as straight pieces, with its bounding box.
struct Flat {
    Seg s[kMaxFlat];
    int n = 0;
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
};

// One trace while it is being placed.
struct Work {
    float sx = 0.0f, sy = 0.0f; // source
    float ex = 0.0f, ey = 0.0f; // sink
    int dir = 0;                // +1 jogs right, -1 left, 0 drops straight
    int bus = 0;
    int lane = 0;
    float yb = 0.0f; // where the jog leaves the first drop
    float ye = 0.0f; // where it joins the last drop
};

float dist_point_seg(PathPoint p, const Seg& s) {
    float vx = s.b.x - s.a.x;
    float vy = s.b.y - s.a.y;
    float len2 = vx * vx + vy * vy;
    float t = 0.0f;
    if (len2 > 0.0f)
        t = std::clamp(((p.x - s.a.x) * vx + (p.y - s.a.y) * vy) / len2, 0.0f, 1.0f);
    float dx = s.a.x + t * vx - p.x;
    float dy = s.a.y + t * vy - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

float cross(PathPoint o, PathPoint a, PathPoint b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

bool segs_cross(const Seg& p, const Seg& q) {
    float d1 = cross(q.a, q.b, p.a);
    float d2 = cross(q.a, q.b, p.b);
    float d3 = cross(p.a, p.b, q.a);
    float d4 = cross(p.a, p.b, q.b);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

float dist_seg_seg(const Seg& p, const Seg& q) {
    if (segs_cross(p, q))
        return 0.0f;
    return std::min(std::min(dist_point_seg(p.a, q), dist_point_seg(p.b, q)),
                    std::min(dist_point_seg(q.a, p), dist_point_seg(q.b, p)));
}

void flat_add(Flat& f, PathPoint a, PathPoint b) {
    if (f.n >= kMaxFlat)
        return;
    f.s[f.n++] = {a, b};
    if (f.n == 1) {
        f.x1 = std::min(a.x, b.x);
        f.x2 = std::max(a.x, b.x);
        f.y1 = std::min(a.y, b.y);
        f.y2 = std::max(a.y, b.y);
        return;
    }
    f.x1 = std::min({f.x1, a.x, b.x});
    f.x2 = std::max({f.x2, a.x, b.x});
    f.y1 = std::min({f.y1, a.y, b.y});
    f.y2 = std::max({f.y2, a.y, b.y});
}

// The polyline a placed trace draws, its last drop ending at @p bottom.
int work_points(const Work& w, float bottom, PathPoint* pts) {
    int n = 0;
    auto push = [&](float x, float y) {
        if (n > 0 && pts[n - 1].x == x && pts[n - 1].y == y)
            return;
        pts[n++] = {x, y};
    };
    push(w.sx, w.sy);
    if (w.dir == 0) {
        push(w.sx, bottom);
        return n;
    }
    push(w.sx, w.yb);
    push(w.ex, w.ye);
    push(w.ex, bottom);
    return n;
}

void flatten(const Work& w, float bottom, float fillet_r, Flat& f) {
    PathPoint pts[kMaxTracePts];
    int n = work_points(w, bottom, pts);
    pathgeo::FilamentPath path;
    pathgeo::route_polyline_filleted(path, pts, n, fillet_r);
    f.n = 0;
    for (int i = 0; i < path.count; ++i) {
        const pathgeo::PathSeg& s = path.segs[i];
        if (s.type == pathgeo::PathSeg::LINE) {
            flat_add(f, s.p0, s.p1);
            continue;
        }
        PathPoint prev{s.center.x + s.radius * std::cos(s.start_angle),
                       s.center.y + s.radius * std::sin(s.start_angle)};
        for (int k = 1; k <= kArcChords; ++k) {
            float a = s.start_angle + s.sweep * (float)k / (float)kArcChords;
            PathPoint p{s.center.x + s.radius * std::cos(a), s.center.y + s.radius * std::sin(a)};
            flat_add(f, prev, p);
            prev = p;
        }
    }
}

// Least distance between two flattened traces, or @p enough as soon as their
// boxes stand at least that far apart.
float flat_distance(const Flat& a, const Flat& b, float enough) {
    float gx = std::max(0.0f, std::max(a.x1 - b.x2, b.x1 - a.x2));
    float gy = std::max(0.0f, std::max(a.y1 - b.y2, b.y1 - a.y2));
    if (gx >= enough || gy >= enough)
        return enough;
    float best = enough;
    for (int i = 0; i < a.n; ++i)
        for (int j = 0; j < b.n; ++j)
            best = std::min(best, dist_seg_seg(a.s[i], b.s[j]));
    return best;
}

struct Params {
    float m = 1.0f;     // jog slope, dy/dx
    float norm = 1.0f;  // length of the jog's direction (1, m)
    float lead_in = 0;  // first drop, at least
    float lead_out = 0; // last drop, at least
    float pitch = 0;    // jog spacing inside a bus
    float gap = 0;      // jog spacing between buses
    float clearance = 0;
    float fillet_r = 0;
};

Params params_for(const RouterConfig& cfg, float m) {
    Params p;
    p.m = m;
    p.norm = std::sqrt(1.0f + m * m);
    // Tangent length of a fillet turning from vertical onto the jog; the fillet
    // keeps its radius when the drop beside it is twice that.
    float tangent = cfg.fillet_r / (p.norm + m);
    p.lead_in = std::max(cfg.lead_in, 2.0f * tangent);
    p.lead_out = std::max(cfg.lead_out, 2.0f * tangent);
    p.clearance = cfg.clearance;
    p.pitch = std::max(cfg.bus_pitch, cfg.clearance);
    p.gap = std::max(cfg.group_gap, p.pitch);
    p.fillet_r = cfg.fillet_r;
    return p;
}

void set_jog(Work& w, float yb, const Params& p) {
    w.yb = yb;
    w.ye = yb + p.m * std::fabs(w.ex - w.sx);
}

// The bottom a trace's last drop reaches while jogs are being placed: below its
// own jog by more than any neighbor can reach, independent of the sink row.
float probe_bottom(const Work& w, const Params& p) {
    float top = w.dir == 0 ? w.sy : w.ye;
    return top + p.lead_out + 2.0f * (p.clearance + p.fillet_r);
}

bool pair_clear(const Work& a, const Work& b, const Params& p) {
    Flat fa, fb;
    flatten(a, probe_bottom(a, p), p.fillet_r, fa);
    flatten(b, probe_bottom(b, p), p.fillet_r, fb);
    return flat_distance(fa, fb, p.clearance) >= p.clearance - kTol;
}

// Lower @p w's jog until it clears @p fixed. False when no depth does.
bool lower_until_clear(Work& w, const Work& fixed, const Params& p) {
    if (pair_clear(w, fixed, p))
        return true;
    float lo = w.yb;
    float step = p.clearance;
    float hi = lo + step;
    int doublings = 0;
    for (;;) {
        set_jog(w, hi, p);
        if (pair_clear(w, fixed, p))
            break;
        if (++doublings > kMaxDoublings) {
            set_jog(w, lo, p);
            return false;
        }
        step *= 2.0f;
        hi = lo + step;
    }
    for (int i = 0; i < kBisectIters; ++i) {
        float mid = 0.5f * (lo + hi);
        set_jog(w, mid, p);
        if (pair_clear(w, fixed, p))
            hi = mid;
        else
            lo = mid;
    }
    set_jog(w, hi, p);
    return true;
}

// Places the jogs of one run of traces that all jog the same way, as a ribbon:
// parallel jogs a pitch apart, as high as the sources allow. @p idx lists the
// run in the direction it jogs toward, so the last entry jogs farthest ahead.
// Returns -1, or the index of a trace no depth clears from its neighbor.
int place_run(Work* w, const int* idx, int k, const Params& p, int* fail_other) {
    // Each trace's signed offset across the ribbon is o = (x * m - yb) / norm in
    // the frame where the run jogs toward +x.
    auto local_x = [&](int j) { return (float)w[idx[j]].dir * w[idx[j]].sx; };
    // A pitch tighter than the sinks' own spacing across the ribbon would end
    // each jog lower than the one outside it, so the ribbon is never tighter.
    auto pitch_after = [&](int j) {
        const Work& a = w[idx[j]];
        const Work& b = w[idx[j + 1]];
        float across_sinks = std::fabs(b.ex - a.ex) * p.m / p.norm;
        return std::max(a.bus == b.bus ? p.pitch : p.gap, across_sinks);
    };

    // The highest ribbon that keeps every first drop at least lead_in long.
    float o_lead = 0.0f;
    float behind = 0.0f; // summed pitch from trace j to the lead trace
    for (int j = k - 1; j >= 0; --j) {
        if (j < k - 1)
            behind += pitch_after(j);
        const Work& t = w[idx[j]];
        float cand = (local_x(j) * p.m - (t.sy + p.lead_in)) / p.norm + behind;
        if (j == k - 1 || cand < o_lead)
            o_lead = cand;
    }

    set_jog(w[idx[k - 1]], local_x(k - 1) * p.m - o_lead * p.norm, p);
    for (int j = k - 2; j >= 0; --j) {
        Work& t = w[idx[j]];
        const Work& ahead = w[idx[j + 1]];
        float o_ahead = (local_x(j + 1) * p.m - ahead.yb) / p.norm;
        float o = o_ahead - pitch_after(j);
        float yb = std::max(local_x(j) * p.m - o * p.norm, t.sy + p.lead_in);
        set_jog(t, yb, p);
        if (!lower_until_clear(t, ahead, p)) {
            *fail_other = idx[j + 1];
            return idx[j];
        }
    }
    return -1;
}

// The real geometry: last drops end at their sinks.
void flatten_final(const Work& w, const Params& p, Flat& f) {
    flatten(w, w.ey, p.fillet_r, f);
}

// Least distance over every pair, and the pair that set it.
float verify_all(const Work* w, int n, const Params& p, int* worst_a, int* worst_b) {
    // Two traces farther apart than this read as clear.
    const float far = p.clearance * 4.0f + 1.0f;
    float best = far;
    *worst_a = -1;
    *worst_b = -1;
    Flat flats[kMaxTraces];
    for (int i = 0; i < n; ++i)
        flatten_final(w[i], p, flats[i]);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            float d = flat_distance(flats[i], flats[j], far);
            if (d < best) {
                best = d;
                *worst_a = i;
                *worst_b = j;
            }
        }
    }
    return best;
}

struct Attempt {
    RouteStatus status = RouteStatus::Ok;
    float required_bottom = 0.0f;
    float min_clearance = 0.0f;
    int pair_a = -1;
    int pair_b = -1;
};

// Splits the traces into runs that jog the same way and places each. Fills
// @p a's required_bottom with the deepest sink row the jogs need.
bool place_all(Work* w, int n, const Params& p, Attempt& a) {
    int i = 0;
    while (i < n) {
        int dir = w[i].dir;
        int j = i;
        while (j + 1 < n && w[j + 1].dir == dir)
            ++j;
        if (dir != 0) {
            int idx[kMaxTraces];
            int k = j - i + 1;
            for (int q = 0; q < k; ++q)
                idx[q] = dir > 0 ? i + q : j - q;
            int other = -1;
            int bad = place_run(w, idx, k, p, &other);
            if (bad >= 0) {
                a.status = RouteStatus::Clearance;
                a.pair_a = std::min(bad, other);
                a.pair_b = std::max(bad, other);
                return false;
            }
        }
        i = j + 1;
    }
    float bottom = -1e30f;
    for (int t = 0; t < n; ++t)
        bottom = std::max(bottom, w[t].dir == 0 ? w[t].sy : w[t].ye + p.lead_out);
    a.required_bottom = bottom;
    return true;
}

// Moves each run's jogs down by @p align of the room its tightest trace has.
void align_runs(Work* w, int n, const Params& p, float align) {
    int i = 0;
    while (i < n) {
        int dir = w[i].dir;
        int j = i;
        while (j + 1 < n && w[j + 1].dir == dir)
            ++j;
        if (dir != 0) {
            float room = 1e30f;
            for (int t = i; t <= j; ++t)
                room = std::min(room, w[t].ey - p.lead_out - w[t].ye);
            float shift = std::max(0.0f, room) * align;
            for (int t = i; t <= j; ++t)
                set_jog(w[t], w[t].yb + shift, p);
        }
        i = j + 1;
    }
}

Attempt try_slope(const Work* in, int n, const RouterConfig& cfg, float m, Work* w) {
    Params p = params_for(cfg, m);
    Attempt a;
    std::copy(in, in + n, w);
    if (!place_all(w, n, p, a))
        return a;
    for (int t = 0; t < n; ++t) {
        float need = w[t].dir == 0 ? w[t].sy : w[t].ye + p.lead_out;
        if (need > w[t].ey + kTol) {
            a.status = RouteStatus::NoRoom;
            return a;
        }
    }
    Work top[kMaxTraces];
    std::copy(w, w + n, top);
    float align = std::clamp(cfg.align, 0.0f, 1.0f);
    if (align > 0.0f)
        align_runs(w, n, p, align);
    a.min_clearance = verify_all(w, n, p, &a.pair_a, &a.pair_b);
    if (a.min_clearance < p.clearance - kTol && align > 0.0f) {
        std::copy(top, top + n, w);
        a.min_clearance = verify_all(w, n, p, &a.pair_a, &a.pair_b);
    }
    if (a.min_clearance < p.clearance - kTol) {
        a.status = RouteStatus::Clearance;
        return a;
    }
    a.pair_a = -1;
    a.pair_b = -1;
    a.status = RouteStatus::Ok;
    return a;
}

// Reads every bus into @p w; checks counts, order and that sinks sit below
// their sources.
RouteStatus load(const Bus* buses, int bus_count, Work* w, int* n_out) {
    int n = 0;
    if (!buses || bus_count <= 0)
        return RouteStatus::BadInput;
    for (int b = 0; b < bus_count; ++b) {
        const Bus& bus = buses[b];
        if (bus.count <= 0 || !bus.sources || !bus.sinks)
            return RouteStatus::BadInput;
        if (n + bus.count > kMaxTraces)
            return RouteStatus::BadInput;
        for (int l = 0; l < bus.count; ++l) {
            Work& t = w[n++];
            t = Work{};
            t.sx = bus.sources[l].x;
            t.sy = bus.sources[l].y;
            t.ex = bus.sinks[l].x;
            t.ey = bus.sinks[l].y;
            t.bus = b;
            t.lane = l;
            float dx = t.ex - t.sx;
            t.dir = std::fabs(dx) < kStraightEps ? 0 : (dx > 0 ? 1 : -1);
            if (!std::isfinite(dx) || !std::isfinite(t.sy) || !std::isfinite(t.ey))
                return RouteStatus::BadInput;
        }
    }
    *n_out = n;
    for (int t = 1; t < n; ++t)
        if (w[t].sx <= w[t - 1].sx || w[t].ex <= w[t - 1].ex)
            return RouteStatus::OutOfOrder;
    return RouteStatus::Ok;
}

void emit(const Work& w, float m, Trace& out) {
    out = Trace{};
    out.bus = w.bus;
    out.lane = w.lane;
    out.slope = w.dir == 0 ? 0.0f : m;
    out.count = work_points(w, w.ey, out.pts);
}

// load(), then the checks every plan makes on the config, the output buffer and
// each sink standing below its source.
RouteStatus load_checked(const Bus* buses, int bus_count, const RouterConfig& cfg, const Trace* out,
                         int out_cap, Work* in, int* n_out) {
    RouteStatus st = load(buses, bus_count, in, n_out);
    if (st == RouteStatus::Ok &&
        (!out || out_cap < *n_out || !(cfg.clearance > 0.0f) || cfg.fillet_r < 0.0f))
        st = RouteStatus::BadInput;
    for (int t = 0; st == RouteStatus::Ok && t < *n_out; ++t)
        if (in[t].ey <= in[t].sy)
            st = RouteStatus::BadInput;
    return st;
}

// Tries each slope of @p slopes in order (0 is a horizontal jog) and emits the
// first plan that fits. A failure reports the first slope's attempt.
RouteStatus route_ladder(const Work* in, int n, const RouterConfig& cfg, const float* slopes,
                         int slope_count, Trace* out, RouteReport& r) {
    Work w[kMaxTraces];
    Attempt first;
    for (int s = 0; s < slope_count; ++s) {
        Attempt a = try_slope(in, n, cfg, slopes[s], w);
        if (s == 0)
            first = a;
        if (a.status == RouteStatus::Ok) {
            for (int t = 0; t < n; ++t)
                emit(w[t], slopes[s], out[t]);
            r.status = RouteStatus::Ok;
            r.slope = slopes[s];
            r.required_bottom = a.required_bottom;
            r.min_clearance = a.min_clearance;
            return RouteStatus::Ok;
        }
    }
    r.status = first.status;
    r.slope = slopes[0];
    r.required_bottom = first.required_bottom;
    r.min_clearance = first.min_clearance;
    r.pair_a = first.pair_a;
    r.pair_b = first.pair_b;
    return first.status;
}

} // namespace

RouteStatus plan_routes(const Bus* buses, int bus_count, const RouterConfig& cfg, Trace* out,
                        int out_cap, RouteReport* report) {
    RouteReport local;
    RouteReport& r = report ? *report : local;
    r = RouteReport{};
    r.slope = cfg.slope;

    Work in[kMaxTraces];
    int n = 0;
    RouteStatus st = load_checked(buses, bus_count, cfg, out, out_cap, in, &n);
    if (st == RouteStatus::Ok && !(cfg.slope > 0.0f))
        st = RouteStatus::BadInput;
    if (st != RouteStatus::Ok) {
        r.status = st;
        return st;
    }

    float slopes[3];
    int slope_count = 0;
    slopes[slope_count++] = cfg.slope;
    if (cfg.alt_slope > 0.0f && cfg.alt_slope != cfg.slope)
        slopes[slope_count++] = cfg.alt_slope;
    if (cfg.allow_horizontal)
        slopes[slope_count++] = 0.0f;
    return route_ladder(in, n, cfg, slopes, slope_count, out, r);
}

RouteStatus plan_routes_ladder(const Bus* buses, int bus_count, const RouterConfig& cfg,
                               const float* slopes, int slope_count, bool level_last, Trace* out,
                               int out_cap, RouteReport* report) {
    RouteReport local;
    RouteReport& r = report ? *report : local;
    r = RouteReport{};
    r.slope = slopes && slope_count > 0 ? slopes[0] : 0.0f;

    Work in[kMaxTraces];
    int n = 0;
    RouteStatus st = load_checked(buses, bus_count, cfg, out, out_cap, in, &n);
    if (st == RouteStatus::Ok && (!slopes || slope_count <= 0 || slope_count > kMaxLadderSlopes))
        st = RouteStatus::BadInput;
    for (int s = 0; st == RouteStatus::Ok && s < slope_count; ++s)
        if (!(slopes[s] > 0.0f))
            st = RouteStatus::BadInput;
    if (st != RouteStatus::Ok) {
        r.status = st;
        return st;
    }
    float ladder[kMaxLadderSlopes + 1];
    std::copy(slopes, slopes + slope_count, ladder);
    int count = slope_count;
    if (level_last)
        ladder[count++] = 0.0f;
    return route_ladder(in, n, cfg, ladder, count, out, r);
}

// Every RouterConfig field: one left out would let a kept plan answer for another config.
static_assert(sizeof(RouterConfig) == 10 * sizeof(float),
              "RouterConfig changed: compare the new field in same_config()");
bool same_config(const RouterConfig& a, const RouterConfig& b) {
    return a.clearance == b.clearance && a.bus_pitch == b.bus_pitch && a.group_gap == b.group_gap &&
           a.fillet_r == b.fillet_r && a.lead_in == b.lead_in && a.lead_out == b.lead_out &&
           a.slope == b.slope && a.alt_slope == b.alt_slope &&
           a.allow_horizontal == b.allow_horizontal && a.align == b.align;
}

InputKey input_key(const Bus* buses, int bus_count, const RouterConfig& cfg) {
    InputKey k;
    k.cfg = cfg;
    for (int b = 0; buses && b < bus_count && b < kMaxTraces; ++b) {
        const Bus& bus = buses[b];
        if (bus.count <= 0 || !bus.sources || !bus.sinks || k.traces + bus.count > kMaxTraces)
            break;
        std::copy(bus.sources, bus.sources + bus.count, k.sources + k.traces);
        std::copy(bus.sinks, bus.sinks + bus.count, k.sinks + k.traces);
        k.bus_sizes[k.bus_count++] = bus.count;
        k.traces += bus.count;
    }
    return k;
}

bool same_input(const InputKey& a, const InputKey& b) {
    auto same_point = [](const PathPoint& p, const PathPoint& q) {
        return p.x == q.x && p.y == q.y;
    };
    return a.traces == b.traces && a.bus_count == b.bus_count &&
           std::equal(a.bus_sizes, a.bus_sizes + a.bus_count, b.bus_sizes) &&
           std::equal(a.sources, a.sources + a.traces, b.sources, same_point) &&
           std::equal(a.sinks, a.sinks + a.traces, b.sinks, same_point) &&
           same_config(a.cfg, b.cfg);
}

float segment_distance(PathPoint a0, PathPoint a1, PathPoint b0, PathPoint b1) {
    return dist_seg_seg({a0, a1}, {b0, b1});
}

float required_span(const Bus* buses, int bus_count, const RouterConfig& cfg) {
    Work w[kMaxTraces];
    int n = 0;
    if (load(buses, bus_count, w, &n) != RouteStatus::Ok || !(cfg.slope > 0.0f) ||
        !(cfg.clearance > 0.0f))
        return -1.0f;
    Params p = params_for(cfg, cfg.slope);
    Attempt a;
    if (!place_all(w, n, p, a))
        return -1.0f;
    float top = w[0].sy;
    for (int t = 1; t < n; ++t)
        top = std::min(top, w[t].sy);
    return std::max(0.0f, a.required_bottom - top);
}

void spread_entries(float cx, float y, int count, float pitch, PathPoint* out) {
    if (!out || count <= 0)
        return;
    float first = cx - 0.5f * pitch * (float)(count - 1);
    for (int i = 0; i < count; ++i)
        out[i] = {first + pitch * (float)i, y};
}

void trace_to_path(const Trace& t, float fillet_r, pathgeo::FilamentPath& out) {
    out.clear();
    pathgeo::route_polyline_filleted(out, t.pts, t.count, fillet_r);
}

} // namespace tuberoute
} // namespace ui
} // namespace helix
