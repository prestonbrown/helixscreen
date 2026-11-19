// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_path_geometry.h"

/**
 * @file tube_router.h
 * @brief Circuit-board style routing for filament tubes: ordered buses of traces
 *        from unit outlets down to hub entries or toolhead inlets.
 *
 * Pure geometry, no LVGL, no allocation: the caller owns every buffer.
 *
 * Coordinates are screen coordinates (+y down) in pixels. Every trace runs DOWN
 * from its source to its sink as at most three octilinear legs:
 *
 *     source -> vertical drop -> one jog -> vertical drop -> sink
 *
 * The jog is a diagonal at the configured slope (45 degrees by default), the
 * optional second slope, or a horizontal run, tried in that order until the
 * routes fit their height. A trace whose sink is directly below its source is
 * one straight drop. Callers round the corners with route_polyline_filleted at
 * the configured fillet radius (trace_to_path does it); the router checks
 * clearance on that filleted shape, not on the raw polyline.
 *
 * Traces keep their order (the i-th source from the left ends at the i-th sink
 * from the left), so routes never cross. Within one run of traces jogging the
 * same way the diagonals form a ribbon: parallel, a constant pitch apart, the
 * longest jog turning first. Neighboring buses in a ribbon keep a wider gap.
 * Jogs sit in the vertical slack as the alignment asks: just under the sources,
 * just above the sinks, or between.
 */

namespace helix {
namespace ui {
namespace tuberoute {

using pathgeo::PathPoint;

/// The most traces one plan routes, across all its buses.
constexpr int kMaxTraces = 32;
/// The most vertices one trace has: source, two bends, sink.
constexpr int kMaxTracePts = 4;

struct RouterConfig {
    /// Least distance between the centerlines of any two traces anywhere: the
    /// tube width plus its halo.
    float clearance = 10.0f;
    /// Centerline spacing of neighboring parallel jogs in one bus; raised to
    /// @ref clearance when smaller.
    float bus_pitch = 0.0f;
    /// Centerline spacing between the jogs of two neighboring buses; raised to
    /// the bus pitch when smaller.
    float group_gap = 0.0f;
    /// Corner radius callers round the bends with.
    float fillet_r = 6.0f;
    /// Least straight drop out of a source before the first bend. Raised to
    /// what the fillet needs to keep its full radius.
    float lead_in = 0.0f;
    /// Least straight drop into a sink after the last bend, raised the same way.
    float lead_out = 0.0f;
    /// The jog's slope, dy per dx: 1 is 45 degrees. Must be positive.
    float slope = 1.0f;
    /// A second slope to try when @ref slope does not fit the height; 0 for none.
    float alt_slope = 0.0f;
    /// Whether horizontal jogs are the last resort when no diagonal fits.
    bool allow_horizontal = true;
    /// Where the jogs sit in the vertical slack: 0 just under the sources, 1
    /// just above the sinks. Each run of same-way jogs moves as one block.
    float align = 1.0f;
};

/// One group of traces: @p count sources, each routed to the sink at the same
/// index. Sources and sinks run left to right.
struct Bus {
    const PathPoint* sources = nullptr;
    const PathPoint* sinks = nullptr;
    int count = 0;
};

/// One routed trace: an octilinear polyline from its source to its sink.
struct Trace {
    PathPoint pts[kMaxTracePts];
    int count = 0;     ///< vertices in @ref pts: 2 for a straight drop, else 4
    int bus = 0;       ///< index of the bus it belongs to
    int lane = 0;      ///< index within that bus
    float slope = 0.f; ///< slope of its jog: dy/dx, 0 for horizontal

    /// Bends in the polyline (each one rounded at the fillet radius).
    int bends() const {
        return count > 2 ? count - 2 : 0;
    }
};

enum class RouteStatus {
    Ok,
    BadInput,   ///< no traces, too many, a null buffer, or a sink not below its source
    OutOfOrder, ///< sources or sinks do not run strictly left to right
    NoRoom,     ///< the sinks sit too high for the jogs at every allowed slope
    Clearance,  ///< two traces come closer than the clearance at every allowed slope
};

struct RouteReport {
    RouteStatus status = RouteStatus::BadInput;
    /// Slope the plan used (Ok), or the primary slope's attempt (a failure).
    float slope = 0.0f;
    /// Deepest y the routes need their sinks at, at that slope.
    float required_bottom = 0.0f;
    /// Least centerline distance between any two routed traces (Ok).
    float min_clearance = 0.0f;
    /// The two traces (plan order) that failed clearance, or -1.
    int pair_a = -1;
    int pair_b = -1;
};

/**
 * @brief Route every bus in @p buses, in order, into @p out.
 *
 * Traces are written in plan order: bus 0's lanes left to right, then bus 1's,
 * and so on; all sources together, and all sinks together, must run strictly
 * left to right. On anything but Ok, @p out holds nothing useful and the caller
 * falls back to its own layout.
 *
 * @param out_cap capacity of @p out; must hold every trace.
 * @param report  optional: the status, the slope used and the numbers behind it.
 */
RouteStatus plan_routes(const Bus* buses, int bus_count, const RouterConfig& cfg, Trace* out,
                        int out_cap, RouteReport* report = nullptr);

/**
 * @brief Least vertical span, from the highest source down to a sink row, that
 *        routes these buses at @p cfg's primary slope.
 *
 * Only the sinks' x is read: the sinks are taken to share one row. Grows with
 * the clearance, the lane count and the horizontal distance each trace jogs.
 * Returns a negative value when no height can meet the clearance.
 */
float required_span(const Bus* buses, int bus_count, const RouterConfig& cfg);

/// @p count entries a @p pitch apart, centered on @p cx, all at @p y: the even
/// spread a narrowing bus lands on across a hub's top.
void spread_entries(float cx, float y, int count, float pitch, PathPoint* out);

/// The filleted path a trace is drawn along: its polyline rounded at @p fillet_r.
void trace_to_path(const Trace& t, float fillet_r, pathgeo::FilamentPath& out);

} // namespace tuberoute
} // namespace ui
} // namespace helix
