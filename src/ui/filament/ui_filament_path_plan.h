// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_unit_shell.h"

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

#include <algorithm>
#include <cmath>
#include <string>

namespace helix::ui::fpath {

// One off-page stub: the hub's suggestion of the other units of its hub that sit on
// other pages. It is a lane that is only hinted at: a short run outward, a diagonal
// parallel to the outermost real lane on its side, and a drop into the hub's top.
struct OffpageStub {
    bool present = false;
    int count = 0;       ///< units it stands for
    bool drying = false; ///< one of them is drying
};

/// The label an off-page stub carries: "N units", "1 unit" for one.
std::string offpage_label_text(int count);

/// The text a chip standing for @p count units on other pages carries: "+N units" where
/// @p fits (called with that text) says it fits, else "+N".
template <typename Fits> std::string offpage_chip_text(int count, Fits&& fits) {
    std::string text = "+" + offpage_label_text(count);
    return fits(text) ? text : "+" + std::to_string(count);
}

/// How an off-page chip fits beside the hub box: which text and whether its glyph, and where.
struct OffpageChipFit {
    bool shown = false;     ///< false: no form of it stands clear of the hub
    bool long_text = false; ///< "+N units"; else "+N"
    bool glyph = false;     ///< the leading heat-wave glyph
    lv_area_t rect{};
};

/// Fits the chip on @p side of @p hub (offpage_chip_rect()): the first of "+N units" with its
/// glyph, "+N" with it, then "+N" alone, that stands @p gap / 2 clear of the hub within
/// [@p min_x, @p max_x]. Text widths @p long_w and @p short_w, the glyph and its spacing
/// @p glyph_extra (0 with none), @p pad each side. A chip no form of which clears the hub is
/// not shown: it never covers the hub.
OffpageChipFit fit_offpage_chip(int side, const lv_area_t& hub, int32_t long_w, int32_t short_w,
                                int32_t glyph_extra, int32_t pad, int32_t h, int32_t gap,
                                int32_t min_x, int32_t max_x);

/// What a count chip draws in: its fill and 1 px ring, and its text's color, font and line.
struct CountPillStyle {
    lv_color_t bg, border, text;
    const lv_font_t* font;
    int32_t line_h;
};

/// A count chip ("+2 units"), the detail page's off-page chip and the overview's parked one:
/// a pill filling @p chip, @p text on one line vertically centered in it between @p text_x1 and
/// @p text_x2, aligned @p align.
void draw_count_pill(lv_layer_t* layer, const lv_area_t& chip, const CountPillStyle& style,
                     const char* text, lv_text_align_t align, int32_t text_x1, int32_t text_x2);

/// Where an off-page chip stands beside the hub box @p hub: left of it for @p side 0, right
/// for 1, @p gap away, vertically centered on it, @p w by @p h, kept within [min_x, max_x].
lv_area_t offpage_chip_rect(int side, const lv_area_t& hub, int32_t w, int32_t h, int32_t gap,
                            int32_t min_x, int32_t max_x);

/// Where a selector rail's head-on art is drawn; w == 0 when the canvas draws the bar instead.
struct RailArtRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

/// The flattest a hub fan's diagonal leans under a view scale, as its drop over its run.
inline constexpr float kFanMinSlope = 0.35f;
/// Under a view scale a fan's fillets are this round, so its diagonals bend late and short.
inline constexpr float kScaledFanFillet = 4.0f;
/// The straight leg a scaled fan's fillet keeps at each end of a diagonal (build_merge_fan's
/// reserve: the fillet and its slack).
inline constexpr float kFanLegRoom = kScaledFanFillet + 4.0f;
/// Under a view scale a lane this close to its hub entry drops straight in: a sub-pixel offset
/// drawn through two fillets reads as a step.
inline constexpr float kStraightLaneSlack = 2.5f;

/// The drop a lane's diagonal needs to run @p run sideways no flatter than kFanMinSlope, its
/// fillets' straight legs included.
inline float fan_lean_drop(float run) {
    return run * kFanMinSlope + 2.0f * kFanLegRoom;
}

/// The height lanes converging into a hub @p hub_h tall take above it, the farthest running
/// @p run sideways: its lean, and never under three quarters of the hub.
inline float fan_drop_needed(float run, int32_t hub_h) {
    return std::max(0.75f * (float)hub_h, fan_lean_drop(run));
}

/// The buffer capsule's height beside a hub chip @p hub_h tall: three quarters of it, never
/// under 14 px so its end marks stay legible.
inline int32_t buffer_capsule_height(int32_t hub_h) {
    return LV_MAX(14, hub_h * 3 / 4);
}

/// The height the trunk keeps for the buffer's control: the capsule. Its thumb stands 3 px
/// proud of it at each end, into the gaps kept above and below it.
inline int32_t buffer_control_height(int32_t hub_h) {
    return buffer_capsule_height(hub_h);
}

/// The buffer control's parts for a capsule @p cap_h tall: the track either side of the tube,
/// the detail button's cell at the capsule's right end, and the thumb.
struct BufferControlGeometry {
    int32_t half = 0;    ///< the track's reach either side of the tube
    int32_t cell = 0;    ///< the detail button's cell, right of the track
    int32_t thumb_w = 0; ///< the thumb, cap_h + 6 tall
    int32_t thumb_h = 0;
};
/// A @p max_w > 0 caps the whole control's width, the track taking the cut.
BufferControlGeometry buffer_control_geometry(int32_t cap_h, int32_t max_w = 0);

/// What a tap on the control at (@p cx, @p cy) opens the detail from: the capsule, its
/// button and the thumb's glow, with 4 px to spare.
lv_area_t buffer_control_hit(int32_t cx, int32_t cy, int32_t cap_h, int32_t max_w = 0);

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
    // The toolhead glyph's scale: the view scale's, or with none the theme's, smaller on a
    // canvas too short to fit the hub between the lanes and a full-size head.
    int32_t tool_scale = 0;
    int32_t inlet_y = 0; // the head's filament inlet, where the tube ends
    int32_t head_x = 0;  // the toolhead glyph's column, its inlet on center_x
    int32_t bypass_merge_y = 0;
    int32_t center_x = 0;
    int32_t output_x = 0; // LINEAR: under the active slot (possibly animating)
    RailArtRect rail;     // LINEAR with rail art: where it is drawn
    RailArtRect through;  // HUB with a pass-through unit's art: where it is drawn

    int32_t buffer_y = 0;
    int32_t buf_fil_top = 0;
    bool has_buffer = false;
    int32_t cap_h = 0; // the buffer capsule's height
    // The buffer's state is drawn inside the hub box: no capsule on the trunk.
    bool buffer_in_hub = false;
    // No room on the trunk for the buffer: the selector's or hub's outlet band shows its reading.
    bool sync_on_output = false;

    lv_color_t idle_color, active_color, hub_bg, hub_border;
    lv_color_t error_color; // error token blended with the pulse phase

    int32_t sensor_r = 0;

    bool has_error = false;
    PathSegment error_seg = PathSegment::NONE;
    PathSegment fil_seg = PathSegment::NONE;

    // HUB merge fan: parallel diagonals per side, one 4-point polyline per lane.
    pg::MergeLaneOut hub_fan[FilamentPathData::kMaxSlots];
    int32_t hub_box_w = 0; // widened entry-spread width (HUB box drawn at this)
    // HUB with the bypass hidden: hub and buffer stacked upward from the
    // toolhead glyph instead of at their ratio positions.
    bool hub_stacked = false;
    // On-toolhead mode: the passthrough selector keeps the unit's position
    // while the hub box moves down to hug the toolhead.
    int32_t selector_y = 0;
    // Units on other pages of this hub: [0] earlier (a chip left of the hub), [1] later.
    OffpageStub stubs[2];

    SlotRenderStates states;
};

/// The layout a unit view draws a unit's lanes with (docs/devel/UNIT_ART.md § "Combiner"):
/// lanes that reach one nozzle meet in a hub box below the unit, whether its combiner is
/// inside it or not, and whatever selector the backend reports. A sliding selector keeps its
/// rail, a one-lane unit has nothing to merge, and paths that do not end at one nozzle
/// (PARALLEL, MIXED) keep their own drawing.
inline helix::PathTopology combined_topology(helix::ui::tray::Combiner combiner, bool has_carriage,
                                             helix::PathTopology reported) {
    using helix::PathTopology;
    const bool one_nozzle = reported == PathTopology::LINEAR || reported == PathTopology::HUB;
    if (!one_nozzle || has_carriage || combiner == helix::ui::tray::Combiner::Absent)
        return reported;
    return PathTopology::HUB;
}

/// Whether a unit view draws a unit's head-on art over the path it lays out: a sliding
/// selector's in LINEAR's bar, the face of a unit without one above its hub (HUB, or MIXED's
/// hub and tools). Lanes on a PARALLEL path each run to their own tool, past no unit worth
/// drawing.
inline bool unit_art_drawn(helix::PathTopology topology, bool has_carriage) {
    using helix::PathTopology;
    return has_carriage ? topology == PathTopology::LINEAR
                        : topology == PathTopology::HUB || topology == PathTopology::MIXED;
}

/// The spool box's front edge plus @p cap, when that leaves two tube gauges and a band of tube
/// above @p row; INT32_MIN when the canvas has no box edge or it stands too low.
int32_t box_edge_above(const FilamentPathData& data, const BaseGeometry& g, int32_t cap,
                       int32_t row);

/// Where the lanes leave the unit: the spool box's front edge when it stands clear above the
/// lane run, else the prep sensors' row.
int32_t lanes_leave_y(const FilamentPathData& data, const BaseGeometry& g);

/// The lanes' row (their prep sensors' straight run's end): its ratio row, or lower when lanes
/// drop their bands below a box edge, by a band's room under the lowest band.
int32_t lanes_row_y(const FilamentPathData& data, const BaseGeometry& g);

/// A sliding selector's art in the LINEAR bar's place: between the lanes' sensors and the
/// buffer, centred, as tall as fits. Empty for a canvas without such art, and for one too small
/// to show it.
RailArtRect rail_art_rect(const FilamentPathData& data, const BaseGeometry& g);

/// Where a pass-through unit's head-on art goes (HUB and MIXED): a unit whose lanes each leave
/// through their own outlet, so nothing slides. Between @p top and @p bottom, centred on
/// @p center_x, as tall as fits. Empty for a canvas without such art, a LINEAR or on-head-hub
/// layout, and a band too short to show it.
RailArtRect through_art_rect(const FilamentPathData& data, const BaseGeometry& g, int32_t top,
                             int32_t bottom, int32_t center_x);

/// The room left above a unit's head-on art for the lanes to bend in to it.
int32_t art_fan_room(const FilamentPathData& data, const BaseGeometry& g);

/// Where @p gate's lane goes into the art drawn at @p r.
inline int32_t rail_gate_x(const helix::ui::tray::RailFaceArt& art, const RailArtRect& r,
                           int gate) {
    return r.x + static_cast<int32_t>(std::lround((art.gate0_x + gate * art.gate_step_x) * r.w));
}

/// The height every lane goes into the art drawn at @p r.
inline int32_t rail_inlet_y(const helix::ui::tray::RailFaceArt& art, const RailArtRect& r) {
    return r.y + static_cast<int32_t>(std::lround(art.inlet_y * r.h));
}

/// The height the filament leaves the art drawn at @p r.
inline int32_t rail_outlet_y(const helix::ui::tray::RailFaceArt& art, const RailArtRect& r) {
    return r.y + static_cast<int32_t>(std::lround(art.outlet_y * r.h));
}

/// Where the filament leaves the carriage when it stands at @p gate.
inline int32_t rail_outlet_x(const helix::ui::tray::RailFaceArt& art, const RailArtRect& r,
                             int gate) {
    return r.x + static_cast<int32_t>(std::lround((art.outlet_x + gate * art.gate_step_x) * r.w));
}

/// Where a LINEAR selector's output leaves with @p slot selected: under the slot, or at the rail
/// art's carriage coupler.
inline int32_t linear_exit_x(const FilamentPathData& data, const BaseGeometry& g, int slot) {
    const RailArtRect r = rail_art_rect(data, g);
    return r.w > 0 ? rail_outlet_x(*data.rail_art, r, slot) : g.slot_x[slot];
}

/// Where a selector rail's carriage is centred. It is under the active gate, on its way
/// there while sliding, and stays at its last gate when nothing is selected.
inline int32_t rail_carriage_x(const int32_t* slot_x, int slot_count, int active_slot,
                               int32_t output_x_current, bool sliding) {
    if (sliding)
        return output_x_current;
    if (active_slot >= 0 && active_slot < slot_count)
        return slot_x[active_slot];
    if (slot_count > 0 && output_x_current >= slot_x[0] &&
        output_x_current <= slot_x[slot_count - 1])
        return output_x_current;
    return slot_count > 0 ? slot_x[0] : output_x_current;
}

/// Width the hub or selector box is drawn at: the frame's fitted width for a HUB, the
/// slot span for a LINEAR selector. Its center is (center_x, hub_y).
int32_t hub_box_width(const FilamentPathData& data, const BaseGeometry& g, const LinearHubFrame& f);

/// The x a LINEAR selector bar and its gear stay left of: the bypass spool's left edge, where
/// that spool stands at the bar's level; never short of the last lane's
/// tube. INT32_MAX where nothing stands in the bar's way.
int32_t hub_box_right_limit(const FilamentPathData& data, const BaseGeometry& g,
                            const LinearHubFrame& f);

/// The hub or selector box before its label and gear widen it: hub_box_width() about
/// center_x, its right end held to hub_box_right_limit().
lv_area_t hub_box_area(const FilamentPathData& data, const BaseGeometry& g,
                       const LinearHubFrame& f);

/// What the LINEAR/HUB hub box carries: an outside gear under a view scale, kept left of
/// hub_box_right_limit(), and the buffer's slot where the hub carries one.
HubBoxExtras hub_section_extras(const FilamentPathData& data, const BaseGeometry& g,
                                const LinearHubFrame& f);

/// The LINEAR/HUB hub or selector box as draw_hub_section() draws it, without an outside gear:
/// hub_box_area() grown by hub_box_shape() for its label and gear.
lv_area_t hub_box_drawn_area(const FilamentPathData& data, const BaseGeometry& g,
                             const LinearHubFrame& f);

/// The LINEAR/HUB nozzle row: the configured toolhead's glyph stands on
/// kToolheadGlyphBottomRatio of the canvas height.
int32_t linear_hub_nozzle_y(const FilamentPathData& data, const BaseGeometry& g);
int32_t linear_hub_nozzle_y(const BaseGeometry& g, int32_t scale);

/// The least height @p g (its top where it is) needs for compute_linear_hub_frame() to lay the
/// path out at full size: a full-size head, and the buffer kept on the trunk when the unit
/// reports one and its hub is not on the head. @p g's own height when that already fits, or
/// when no height up to four times it would.
int32_t linear_hub_required_height(const FilamentPathData& data, BaseGeometry g);

/// The LINEAR/HUB frame for canvas @p g, its head standing on the canvas's nozzle row.
LinearHubFrame compute_linear_hub_frame(const FilamentPathData& data, const BaseGeometry& g);
/// The frame with the full-size toolhead glyph's topmost drawn Y at @p glyph_top
/// (toolhead_top_y()): a HUB with the bypass hidden stacks its hub and buffer above it. A HUB
/// canvas too short for its hub above that glyph draws a smaller head
/// (LinearHubFrame::tool_scale).
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
    pg::MergeLaneOut hub_fan[FilamentPathData::kMaxSlots];
    int slot_to_fan[FilamentPathData::kMaxSlots]; // slot index -> hub-lane order (-1 none)
    RailArtRect through; // a pass-through unit's art between the spools and the lane sensors
    // Each direct lane's own head; spread a head's width apart from the shared one where the
    // lanes stand closer than that. A hub lane's entry is its column (its outlet under a view
    // scale).
    int32_t head_x[FilamentPathData::kMaxSlots] = {};
    // The shared head's center X: under the hub, or apart from it where the hub stands where
    // its lanes meet and the trunk leans to the head.
    int32_t shared_head_x = 0;

    SlotRenderStates states;
};

/**
 * @brief Routes @p n lanes from @p sources down to the sinks already in each @p out[i].pts[3]
 * with the tube router (tube_router.h), keeping their order and @p clearance apart, into
 * @p out's four waypoints. False, @p out untouched, where the router finds no room: the caller
 * keeps its own routes.
 */
bool route_lanes(const pathgeo::PathPoint* sources, pathgeo::MergeLaneOut* out, int n,
                 float clearance);
/// route_lanes() for a merge fan already built into @p fan from @p fan_in: each lane from its
/// start to the entry the fan chose for it. Where the router finds no room the fan's own
/// routes stand.
void route_fan(const pathgeo::MergeLaneIn* fan_in, pathgeo::MergeLaneOut* fan, int n,
               float clearance);
/// The least centerline distance two routed tubes keep: a tube, its halo and a gap, or a
/// sensor band's width where that is more.
float lane_clearance(const FilamentPathData& data);

/// Pixels per view unit (path_view_scale.h): the scale's own, or the hub's height in units.
float view_unit(const FilamentPathData& data);

/// The smallest a shrunk head draws and still reads as a head.
inline constexpr int32_t kMinHeadScale = 4;
/// The tallest a head stands under a view scale, in view units, whatever its style's
/// proportions: a tall head would take the unit's height on a compact view.
inline constexpr float kHeadUnits = 6.5f;
/// @p scale, shrunk (to kMinHeadScale at the least) until the head stands no taller than
/// @p max_h.
int32_t head_scale_within_height(int32_t scale, float max_h);

/// A hub with one lane entering on this page gathers nothing: no hub box is drawn, only the
/// buffer's meter where the hub carries one, and the lane's tube runs on through.
bool hub_is_bare(const FilamentPathData& data);

MixedFrame compute_mixed_frame(const FilamentPathData& data, const BaseGeometry& g);

// PARALLEL rows: the lane entries, each tool's entry sensor, and its toolhead.
struct ParallelRows {
    int32_t entry_y, prep_y, sensor_y, toolhead_y, tool_scale;
};
ParallelRows parallel_rows(const FilamentPathData& data, const BaseGeometry& g);
/// Under a view scale, the least height a PARALLEL or MIXED path lays out in: each lane's run
/// to its full-size head, a shared hub and a pass-through unit's drawing; 0 without a scale.
int32_t per_lane_required_height(const FilamentPathData& data, BaseGeometry g);
/// Where lane @p i's own head stands in a PARALLEL view: under its lane, or, where the lanes
/// stand closer than a head's width, spread evenly at a head's width apart, centered under the
/// lanes and kept inside the canvas; its tube bends out to it.
int32_t parallel_head_x(const FilamentPathData& data, const BaseGeometry& g,
                        const ParallelRows& rows, int i);

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
    SpanStyle style[pg::FilamentPath::kMaxSegs];
    int dropped = 0; // segments that did not fit in kMaxSegs
};

/// Append @p piece's segments with style @p s. Segments past kMaxSegs are
/// dropped and counted in Route::dropped; returns false when any were.
bool route_append(Route& r, const pg::FilamentPath& piece, SpanStyle s);

/// Sync: a rail's sync sensor at its outlet, banded in the buffer's colour (fill).
enum class BandState : uint8_t { Empty, Loaded, Active, Error, Sync };

struct SensorBand {
    pg::PathPoint at;
    pg::PathPoint tangent; // unit direction of the tube under the band
    BandState state;
    lv_color_t fill;          // the lane's filament color (Loaded)
    bool on_box_edge = false; // straddles a hub/selector edge: painted over the box
    bool dot = false;         // a round dot, ringed dark, instead of a band across the tube
};

inline constexpr int kMaxRoutes = FilamentPathData::kMaxSlots + 2; // lanes + trunk + bypass
inline constexpr int kMaxBands = 2 * FilamentPathData::kMaxSlots + 4;

struct PathPlan {
    Route routes[kMaxRoutes];
    int route_count = 0;
    int active_route = -1;
    int trunk_route = -1;  // idle trunk, when no lane owns it
    int bypass_route = -1; // bypass horizontal (and the trunk below it when active)
    SensorBand bands[kMaxBands];
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
inline constexpr int kTrunkBands = 3;
enum class BandKind : uint8_t { Lane, Trunk };

void reset_plan(PathPlan& out);
/// Adds every route's dropped segments to PathPlan::dropped; call once, last.
void total_dropped(PathPlan& out);
Route& new_route(PathPlan& plan);
void append_line(Route& r, float x0, float y0, float x1, float y1, SpanStyle s);

/// The bypass feed: out of the bottom of the spool standing over (@p spool_x, @p merge_y),
/// down into the gap above its label, then along the merge line to @p merge_x.
void append_bypass_feed(Route& r, float spool_x, float merge_y, float merge_x, SpanStyle s);
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

/// The row @p lane's tube starts on in frame @p f: on a box edge, the edge where it crosses
/// the lane's column (the edge's lowest point less the lane's rise).
int32_t lane_start_y(const FilamentPathData& data, const LinearHubFrame& f, int lane);

/// The row of @p lane's prep band: its band drop below its start, held a band's width above
/// the lane's row.
int32_t lane_band_y(const FilamentPathData& data, const LinearHubFrame& f, int lane);

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
/// The colour a band paints in: accent, error, its fill when loaded or a sync band, else idle.
lv_color_t band_color(const SensorBand& band, const TubePalette& pal);
/// The bands on a hub/selector edge; called after the boxes are drawn.
void paint_box_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal);

/// The frame's off-page chips beside the hub box drawn this pass ("+1 unit"), each a tap
/// target recorded in the widget's hits.
void draw_offpage_chips(const RenderCtx& ctx, const LinearHubFrame& f);

// Clamp band: a short rounded bar across the tube.
inline constexpr int32_t kBandExtra = 10;    // band length = gauge + kBandExtra
inline constexpr int32_t kBandThickness = 4; // stroke width, round caps

/// Centerline of the band bar: at ± normal * ((gauge + kBandExtra)/2 - kBandThickness/2).
void band_segment(const SensorBand& band, int32_t gauge, pg::PathPoint& p0, pg::PathPoint& p1);
void draw_sensor_band(lv_layer_t* layer, const SensorBand& band, int32_t gauge, lv_color_t color);

} // namespace helix::ui::fpath
