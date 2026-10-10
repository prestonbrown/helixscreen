// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_filament_path_plan.h
 * @brief Route plan for the LINEAR/HUB detail canvas: frame → plan → paint.
 *
 * The frame is the per-draw layout. The plan turns it into routes (one
 * contiguous centerline per lane, the trunk owned by exactly one route) with a
 * SpanStyle per segment, plus the sensor bands. paint_tubes then strokes every
 * route in layers (halo, walls, bores, bands), so tubes run unbroken under
 * every sensor and a junction never shows a neighbour's cap over the active
 * fill. Bands on a box edge wait for paint_box_bands, after the boxes, so they
 * clamp the tube where it enters. The active route's filled prefix is what the
 * animation pass replays.
 */

#include "ui_filament_path_internal.h"

#include <string>

namespace helix::ui::fpath {

// One off-page stub: the hub's suggestion of the other units of its hub that sit on
// other pages. It is a lane that is only hinted at: a short run outward, a diagonal
// parallel to the outermost real lane on its side, and a drop into the hub's top.
struct OffpageStub {
    bool present = false;
    int count = 0;        ///< units it stands for
    bool drying = false;  ///< one of them is drying
    bool labeled = false; ///< the label has room; without it the stub is drawn bare
    /// Outer end of the run, start of the diagonal, its end where the drop begins,
    /// and where the drop meets the hub's top edge.
    pg::PathPoint pts[4];
    int32_t label_x = 0;  ///< left edge of the label block (glyph, then text)
    int32_t label_cy = 0; ///< center line of the label block
    int32_t label_w = 0;  ///< block width, glyph included
    int32_t glyph_w = 0;  ///< the drying glyph's width, 0 when there is none
};

/// The label an off-page stub carries: "N units", "1 unit" for one.
std::string offpage_label_text(int count);

// Everything the LINEAR/HUB plan needs for one draw: layout Ys, resolved
// colors, filament/error state and the HUB merge-fan plan.
struct LinearHubFrame {
    // Vertical layout (absolute display coords)
    int32_t entry_y = 0;
    int32_t prep_y = 0;
    int32_t prep_band_y = 0;       // the prep sensor's band: prep_y, or the box edge
    bool prep_on_box_edge = false; // lanes enter at the spool box's front edge
    int32_t hub_y = 0;
    int32_t hub_h = 0;
    int32_t output_y = 0;
    int32_t toolhead_y = 0;
    int32_t nozzle_y = 0;
    int32_t inlet_y = 0; // the toolhead glyph's filament inlet
    int32_t bypass_merge_y = 0;
    int32_t center_x = 0;
    int32_t output_x = 0; // LINEAR: under the active slot (possibly animating)

    int32_t buffer_y = 0;
    int32_t buf_fil_top = 0;
    bool has_buffer = false;

    lv_color_t idle_color, active_color, hub_bg, hub_border;
    lv_color_t error_color; // error token blended with the pulse phase

    int32_t sensor_r = 0;

    bool has_error = false;
    PathSegment error_seg = PathSegment::NONE;
    PathSegment fil_seg = PathSegment::NONE;

    // HUB merge fan: parallel diagonals per side, one 4-point polyline per lane.
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int32_t hub_box_w = 0; // widened entry-spread width (HUB box drawn at this)
    // HUB with the bypass hidden: hub and buffer stacked upward from the
    // toolhead glyph instead of at their ratio positions.
    bool hub_stacked = false;
    // On-toolhead mode: the passthrough selector keeps the unit's position
    // while the hub box moves down to hug the toolhead.
    int32_t selector_y = 0;
    // Off-page stubs: [0] left of the hub, [1] right. The hub is widened to hold their
    // entries whenever either side has one.
    OffpageStub stubs[2];

    SlotRenderStates states;
};

/// Width the hub or selector box is drawn at: the frame's fitted width for a HUB, the
/// slot span for a LINEAR selector. Its center is (center_x, hub_y).
int32_t hub_box_width(const FilamentPathData& data, const BaseGeometry& g, const LinearHubFrame& f);

/// @p glyph_top is the toolhead glyph's topmost drawn Y (toolhead_top_y()):
/// a HUB with the bypass hidden stacks its hub and buffer above it.
LinearHubFrame compute_linear_hub_frame(const FilamentPathData& data, const BaseGeometry& g,
                                        int32_t glyph_top);

/// The hub/selector box's fill and border: the error color for an error at
/// the hub, else a filament tint when it holds filament, else the theme's.
BoxColors resolve_hub_tint(const FilamentPathData& data, const LinearHubFrame& f,
                           bool has_filament);

// MIXED (HTLF) layout: some lanes run direct to their own nozzle, the rest fan
// into a shared hub feeding one nozzle.
struct MixedFrame {
    int32_t entry_y = 0;
    int32_t sensor_y = 0;
    int32_t hub_cy = 0;
    int32_t hub_h = 0;
    int32_t hub_bottom = 0;
    int32_t toolhead_y = 0;
    int32_t tool_scale = 0;

    int hub_count = 0;       // lanes routed through the hub
    int first_hub_lane = -1; // first hub-routed slot index
    int32_t hub_cx = 0;      // hub box center X (mean of hub lane Xs)
    int32_t hub_w = 0;

    // Merge fan for the hub lanes: parallel diagonals per side spread across
    // distinct hub-top entries.
    pg::MergeLaneOut hub_fan[FilamentPathData::MAX_SLOTS];
    int slot_to_fan[FilamentPathData::MAX_SLOTS]; // slot index -> hub-lane order (-1 none)

    SlotRenderStates states;
};

MixedFrame compute_mixed_frame(const FilamentPathData& data, const BaseGeometry& g);

// PARALLEL rows: the lane entries, each tool's entry sensor, and its toolhead.
struct ParallelRows {
    int32_t entry_y, sensor_y, toolhead_y, tool_scale;
};
ParallelRows parallel_rows(const FilamentPathData& data, const BaseGeometry& g);

enum class TubeWall : uint8_t { Plain, Active, Error };

struct SpanStyle {
    TubeWall wall = TubeWall::Plain;
    lv_color_t bore;     // filament color, or the background when empty
    bool filled = false; // filament is in this span
    bool painted = true; // false inside an opaque box: recorded, never stroked
};
bool operator==(const SpanStyle& a, const SpanStyle& b);

struct Route {
    pg::FilamentPath path;
    SpanStyle style[pg::FilamentPath::MAX_SEGS];
    int dropped = 0; // segments that did not fit in MAX_SEGS
};

/// Append @p piece's segments with style @p s. Segments past MAX_SEGS are
/// dropped and counted in Route::dropped; returns false when any were.
bool route_append(Route& r, const pg::FilamentPath& piece, SpanStyle s);

enum class BandState : uint8_t { Empty, Loaded, Active, Error };

struct SensorBand {
    pg::PathPoint at;
    pg::PathPoint tangent; // unit direction of the tube under the band
    BandState state;
    lv_color_t fill;          // the lane's filament color (Loaded)
    bool on_box_edge = false; // straddles a hub/selector edge: painted over the box
};

inline constexpr int MAX_ROUTES = FilamentPathData::MAX_SLOTS + 2; // lanes + trunk + bypass
inline constexpr int MAX_BANDS = 2 * FilamentPathData::MAX_SLOTS + 4;

struct PathPlan {
    Route routes[MAX_ROUTES];
    int route_count = 0;
    int active_route = -1;
    int trunk_route = -1;  // idle trunk, when no lane owns it
    int bypass_route = -1; // bypass horizontal (and the trunk below it when active)
    SensorBand bands[MAX_BANDS];
    int band_count = 0;
    int trunk_band_count = 0; // of band_count: output, merge and toolhead bands
    int dropped = 0; // segments and bands that did not fit; the plan is incomplete when > 0
    bool buffer_has_filament = false;
    lv_color_t buffer_fill;
};

struct Stroke {
    int first = 0;
    int end = 0;
    SpanStyle style;
};

/// Maximal runs of adjacent painted segments with equal styles; an unpainted
/// segment ends a run and yields nothing. Returns the stroke count.
int coalesce(const Route& r, Stroke* out, int max_out);

/// segs[0..k], k = the last segment with filament, painted or not.
pg::FilamentPath filled_prefix(const Route& r);

SpanStyle span_style(PathSegment span, PathSegment reached, bool on_active_route,
                     PathSegment error_seg, lv_color_t filament, lv_color_t bg);
BandState band_state(PathSegment sensor, PathSegment reached, bool on_active_route,
                     PathSegment error_seg);
/// The error a lane shows: the system's on the active route, else the lane's
/// own at the point its filament reached (its spool when none did), else none.
/// span_style()/band_state() mark the span and band at the error they are given.
PathSegment lane_error(const SlotRenderState& s, bool on_active_route, PathSegment system_error);

// What one route carries: how far its filament reached, whether it is the
// active route (or the idle trunk, which shows errors like one), its color.
struct Lane {
    PathSegment reached;
    bool on;
    PathSegment error;
    lv_color_t color;
    lv_color_t bg;
    SpanStyle style(PathSegment tag) const {
        return span_style(tag, reached, on, error, color, bg);
    }
    BandState band(PathSegment tag) const {
        return band_state(tag, reached, on, error);
    }
};

// Plan building blocks shared by every planner, the detail views' and the
// system overview's.

/// Lane bands (prep, hub entry) stop short of the last few slots, so a full
/// band table drops a lane band and never the trunk's output, merge or
/// toolhead band.
inline constexpr int TRUNK_BANDS = 3;
enum class BandKind : uint8_t { Lane, Trunk };

void reset_plan(PathPlan& out);
/// Adds every route's dropped segments to PathPlan::dropped; call once, last.
void total_dropped(PathPlan& out);
Route& new_route(PathPlan& plan);
void append_line(Route& r, float x0, float y0, float x1, float y1, SpanStyle s);
SpanStyle unpainted(SpanStyle s);
void add_band(PathPlan& plan, BandKind kind, pg::PathPoint at, pg::PathPoint tangent,
              BandState state, lv_color_t fill, bool on_box_edge = false);
/// A band where the route currently ends, across its last segment.
void add_band_at_end(PathPlan& plan, BandKind kind, const Route& r, BandState state,
                     lv_color_t fill, bool on_box_edge = false);

/// The one plan every canvas renders through. About 14 KB: kept off the stack,
/// which on the ESP32 is the LVGL task's, and out of internal DRAM, which the
/// WiFi driver needs for its RX buffers. Rendering is single-threaded and not
/// re-entrant, and nothing DMA- or ISR-side touches it.
PathPlan& plan_scratch();

/// Outer tube width, walls included, for a theme's space_xs.
int32_t tube_gauge_for_spacing(int32_t space_xs);

void plan_linear_hub(const LinearHubFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                     PathPlan& out);
/// One route per tool: entry → sensor band → nozzle top; the mounted tool is active.
void plan_parallel(const FilamentPathData& data, const BaseGeometry& g, PathPlan& out);
/// Hub lanes: entry → sensor band → fan → hub top. One shared trunk: hub bottom
/// → nozzle top. Direct lanes: entry → sensor band → their own nozzle top.
void plan_mixed(const MixedFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                PathPlan& out);

struct TubePalette {
    lv_color_t idle_wall, accent, error, bg;
    int32_t gauge;
};
/// The error token, blended toward its darker shade with the pulse phase.
lv_color_t pulsed_error_color(const FilamentPathData& data);
/// Theme walls, accent, pulsed error, background and gauge.
TubePalette tube_palette(const FilamentPathData& data);
/// Halo, walls, bores, then every band not on a box edge.
void paint_tubes(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal,
                 bool simple = reduced_effects());
/// The bands on a hub/selector edge; called after the boxes are drawn.
void paint_box_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal);

/// The frame's off-page stubs: a dashed polyline in the idle tube color, then the label
/// (a drying glyph before it when one of the units is drying). Called before the hub box
/// so the box covers the end of each drop.
void draw_offpage_stubs(const RenderCtx& ctx, const LinearHubFrame& f);

// Clamp band: a short rounded bar across the tube.
inline constexpr int32_t BAND_EXTRA = 10;    // band length = gauge + BAND_EXTRA
inline constexpr int32_t BAND_THICKNESS = 4; // stroke width, round caps

/// Centerline of the band bar: at ± normal * ((gauge + BAND_EXTRA)/2 - BAND_THICKNESS/2).
void band_segment(const SensorBand& band, int32_t gauge, pg::PathPoint& p0, pg::PathPoint& p1);
void draw_sensor_band(lv_layer_t* layer, const SensorBand& band, int32_t gauge, lv_color_t color);

} // namespace helix::ui::fpath
