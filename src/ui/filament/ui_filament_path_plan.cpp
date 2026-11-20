// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LINEAR/HUB route plan: the pure frame → plan step and the layered painter.
// See ui_filament_path_plan.h for the model.

#include "ui_filament_path_plan.h"

#include "ui_ams_lane_spool.h"
#include "ui_bypass_spool_widget.h"
#include "ui_icon_codepoints.h"

#include "i18n_plural.h"
#include "lvgl.h"
#include "path_view_scale.h"
#include "theme_manager.h"
#include "tube_router.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace helix::ui::fpath {

// ============================================================================
// Frame
// ============================================================================

lv_color_t pulsed_error_color(const FilamentPathData& data) {
    const lv_color_t error = data.theme.color_error;
    if (!data.anim.error_pulse_active || data.anim.error_pulse_opa >= LV_OPA_COVER)
        return error;
    const float blend = (float)(LV_OPA_COVER - data.anim.error_pulse_opa) /
                        (float)(LV_OPA_COVER - kErrorPulseOpaMin);
    return ph_blend(error, ph_darken(error, 80), blend);
}

int32_t tube_gauge_for_spacing(int32_t space_xs) {
    return LV_MAX(3, space_xs - 3) + 2;
}

TubePalette tube_palette(const FilamentPathData& data) {
    const ThemeCache& t = data.theme;
    return {t.color_idle, t.color_accent, pulsed_error_color(data), t.color_bg, t.tube_gauge};
}

lv_area_t offpage_chip_rect(int side, const lv_area_t& hub, int32_t w, int32_t h, int32_t gap,
                            int32_t min_x, int32_t max_x) {
    const int32_t cy = (hub.y1 + hub.y2) / 2;
    int32_t x1 = side == 0 ? hub.x1 - gap - w : hub.x2 + gap;
    x1 = std::clamp(x1, min_x, std::max(min_x, max_x - w));
    return {x1, cy - h / 2, x1 + w, cy - h / 2 + h};
}

OffpageChipFit fit_offpage_chip(int side, const lv_area_t& hub, int32_t long_w, int32_t short_w,
                                int32_t glyph_extra, int32_t pad, int32_t h, int32_t gap,
                                int32_t min_x, int32_t max_x) {
    struct Form {
        bool long_text, glyph;
    };
    const Form forms[] = {{true, true}, {false, true}, {false, false}};
    for (const Form& form : forms) {
        if (!form.glyph && glyph_extra == 0)
            continue; // the same as the form before it
        const int32_t w =
            (form.long_text ? long_w : short_w) + 2 * pad + (form.glyph ? glyph_extra : 0);
        const lv_area_t r = offpage_chip_rect(side, hub, w, h, gap, min_x, max_x);
        const bool clear = side == 0 ? r.x2 <= hub.x1 - gap / 2 : r.x1 >= hub.x2 + gap / 2;
        if (clear)
            return {true, form.long_text, form.glyph && glyph_extra > 0, r};
    }
    return {};
}

std::string offpage_label_text(int count) {
    char buf[48];
    snprintf(buf, sizeof(buf), tr_plural("1 unit", "%d units", static_cast<uint64_t>(count)),
             count);
    return buf;
}

int32_t hub_box_width(const FilamentPathData& data, const BaseGeometry& g,
                      const LinearHubFrame& f) {
    if (data.topology == static_cast<int>(PathTopology::LINEAR) && data.slot_count > 1) {
        const int32_t slot_pad = LV_MAX(data.slot_width, f.sensor_r * 4);
        return (g.slot_x[data.slot_count - 1] - g.slot_x[0]) + slot_pad;
    }
    return data.topology == static_cast<int>(PathTopology::HUB) ? f.hub_box_w
                                                                : data.theme.hub_width;
}

int32_t hub_box_right_limit(const FilamentPathData& data, const BaseGeometry& g,
                            const LinearHubFrame& f) {
    if (data.topology != static_cast<int>(PathTopology::LINEAR) || !data.show_bypass ||
        !data.feeds_toolhead || data.hub_on_toolhead || data.slot_count < 1)
        return INT32_MAX;
    // The box the spool widget stands in, over the merge line at kBypassXRatio.
    const int32_t box = helix::ui::bypass_spool_box_size();
    const int32_t spool_bottom = f.bypass_merge_y - helix::ui::bypass_spool_bottom_above_merge();
    const int32_t spool_top = spool_bottom - box;
    // The bar's gear may stand a little taller than the bar; half a bar's height of margin
    // covers it.
    const int32_t reach = f.hub_h;
    if (spool_top > f.hub_y + reach || spool_bottom < f.hub_y - reach)
        return INT32_MAX;
    // The spool itself, inside its box's padding.
    const int32_t spool_left =
        g.x_off + (int32_t)(g.width * kBypassXRatio) - helix::ui::ams_slot_spool_size() / 2;
    // The bar always reaches the last lane's tube.
    return LV_MAX(spool_left, g.slot_x[data.slot_count - 1] + data.theme.tube_gauge);
}

lv_area_t hub_box_area(const FilamentPathData& data, const BaseGeometry& g,
                       const LinearHubFrame& f) {
    const int32_t w = hub_box_width(data, g, f);
    const int32_t x1 = f.center_x - w / 2;
    // An even width, so the box drawn about its middle is this one.
    const int32_t x2 = LV_MIN(f.center_x + w / 2, hub_box_right_limit(data, g, f));
    return {x1, f.hub_y - f.hub_h / 2, x1 + (x2 - x1) / 2 * 2, f.hub_y + f.hub_h / 2};
}

namespace {
// A slope no fan reaches: the diagonals take the whole height between the lanes and the hub.
constexpr float kSmoothFanSlope = 1.0e4f;
// Room above the rail art for the lanes to bend in from their slots, as a share of the height.
constexpr float kRailFanRatio = 0.08f;
constexpr float kRailMaxWidthRatio = 0.9f;
// Below this the art's detail is lost; the bar is drawn instead.
constexpr int32_t kRailArtMinH = 40;
// A pass-through unit's art is never taller than this share of the canvas.
constexpr float kThroughMaxHRatio = 0.3f;

// The short stub of shared tube between a hub on the print head and the head.
int32_t hub_stub_height(const BaseGeometry& g) {
    return LV_MAX(10, (int32_t)(g.height * 0.03f));
}

// The height-search ceiling under a view scale: well past a canvas the unit has squeezed, whose
// own height says nothing about what the path needs.
int32_t height_search_ceiling(int32_t own, const FilamentPathData& data) {
    return std::max(own * 4, data.view_hub_h * 24);
}

// A tube's round cap: how far below the edge it hangs from its top reaches.
int32_t tube_cap(const FilamentPathData& data) {
    return (data.theme.tube_gauge + 1) / 2;
}

// The size head-on art takes in a band @p band_h tall: as tall as fits, never wider than most of
// the canvas. Empty when the art would be too small to read.
RailArtRect fit_art(const FilamentPathData& data, const BaseGeometry& g, int32_t band_h) {
    int32_t h = band_h;
    int32_t w = (int32_t)std::lround(h * data.rail_art->aspect);
    const int32_t max_w = (int32_t)(g.width * kRailMaxWidthRatio);
    if (w > max_w) {
        w = max_w;
        h = (int32_t)std::lround(w / data.rail_art->aspect);
    }
    if (h < kRailArtMinH)
        return {};
    return {0, 0, w, h};
}
} // namespace

namespace {
/// How many tubes' and bands' room the lanes keep straight below the spool box's edge before
/// their row: two, a compact path one, so the unit's drawing keeps the height.
int32_t lane_stub_tubes(const FilamentPathData& data) {
    return compact_path(view_unit(data)) ? 1 : 2;
}
} // namespace

int32_t box_edge_above(const FilamentPathData& data, const BaseGeometry& g, int32_t cap,
                       int32_t row) {
    if (g.lane_entry_y == INT32_MIN)
        return INT32_MIN;
    const int32_t y = g.lane_entry_y + cap;
    return y <= row - lane_stub_tubes(data) * data.theme.tube_gauge - kBandThickness ? y
                                                                                     : INT32_MIN;
}

int32_t lanes_row_y(const FilamentPathData& data, const BaseGeometry& g) {
    const int32_t prep_y = g.y_off + (int32_t)(g.height * kPrepYRatio);
    if (g.lane_entry_y == INT32_MIN)
        return prep_y;
    // Lanes that drop their bands below a slanted edge keep a band's room under the lowest.
    int32_t drop = 0;
    for (int i = 0; i < FilamentPathData::kMaxSlots; ++i)
        drop = std::max(drop, data.lane_band_drop[i]);
    if (drop == 0)
        return prep_y;
    const int32_t stub = lane_stub_tubes(data);
    return std::max(prep_y,
                    g.lane_entry_y + drop + stub * (data.theme.tube_gauge + kBandThickness));
}

int32_t lanes_leave_y(const FilamentPathData& data, const BaseGeometry& g) {
    const int32_t prep_y = lanes_row_y(data, g);
    // The lanes leave the spool box at its front edge, through their prep sensors.
    const int32_t edge = box_edge_above(data, g, 0, prep_y);
    return edge != INT32_MIN ? edge : prep_y;
}

int32_t art_fan_room(const FilamentPathData& data, const BaseGeometry& g) {
    return LV_MAX(4 * data.theme.sensor_radius, (int32_t)(g.height * kRailFanRatio));
}

namespace {
// The selector's slot, between the lanes' sensors and the buffer, centred, as tall as fits: the
// art sits on the band's bottom. Empty when it is too short to show the art.
RailArtRect selector_slot_rect(const FilamentPathData& data, const BaseGeometry& g) {
    const int32_t sensor_r = data.theme.sensor_radius;
    const int32_t buffer_box_h = LV_MAX(16, (int32_t)(g.height * kHubHeightRatio));
    const int32_t top = lanes_leave_y(data, g) + art_fan_room(data, g);
    // Clear of the buffer box, or of where the exit's jog to the trunk ends above a buffer.
    const int32_t buffer_y = g.y_off + (int32_t)(g.height * kBufferYRatio);
    const int32_t bottom =
        (data.buffer_present ? buffer_y - buffer_box_h : buffer_y - buffer_box_h / 2) -
        2 * sensor_r;
    const RailArtRect size = fit_art(data, g, bottom - top);
    return size.w > 0 ? RailArtRect{g.center_x - size.w / 2, bottom - size.h, size.w, size.h}
                      : RailArtRect{};
}
} // namespace

BufferControlGeometry buffer_control_geometry(int32_t cap_h, int32_t max_w) {
    BufferControlGeometry b;
    b.half = LV_MAX(cap_h * 2 + cap_h / 2, 22);
    b.cell = cap_h + 2;
    // A capped track still leaves its thumb room to travel between the end marks.
    if (max_w > 0)
        b.half = LV_MAX(LV_MIN(b.half, (max_w - b.cell) / 2), cap_h + cap_h / 2 + 6);
    b.thumb_w = LV_MAX(6, cap_h * 2 / 5);
    b.thumb_h = cap_h + 6;
    return b;
}

lv_area_t buffer_control_hit(int32_t cx, int32_t cy, int32_t cap_h, int32_t max_w) {
    const BufferControlGeometry b = buffer_control_geometry(cap_h, max_w);
    // The thumb's glow reaches 3 px past it.
    const int32_t reach = LV_MAX(cap_h / 2, b.thumb_h / 2 + 3) + 4;
    return {cx - b.half - 4, cy - reach, cx + b.half + b.cell + 4, cy + reach};
}

RailArtRect rail_art_rect(const FilamentPathData& data, const BaseGeometry& g) {
    if (!data.rail_art || data.rail_art_failed || !data.rail_carriage ||
        data.topology != static_cast<int>(PathTopology::LINEAR) || data.hub_on_toolhead ||
        data.rail_art->aspect <= 0)
        return {};
    return selector_slot_rect(data, g);
}

namespace {
// The canvas has a pass-through unit's art to draw: HUB or MIXED, art that loads, no head hub.
bool has_through_art(const FilamentPathData& data) {
    const bool spread = data.topology == static_cast<int>(PathTopology::HUB) ||
                        data.topology == static_cast<int>(PathTopology::MIXED);
    return data.rail_art && !data.rail_art_failed && spread && !data.hub_on_toolhead &&
           data.rail_art->aspect > 0;
}
} // namespace

RailArtRect through_art_rect(const FilamentPathData& data, const BaseGeometry& g, int32_t top,
                             int32_t bottom, int32_t center_x) {
    if (!has_through_art(data))
        return {};
    // The lanes come in just above a pass-through unit: it sits on the band's top.
    const RailArtRect size = fit_art(data, g, bottom - top);
    return size.w > 0 ? RailArtRect{center_x - size.w / 2, top, size.w, size.h} : RailArtRect{};
}

int32_t linear_hub_nozzle_y(const FilamentPathData& data, const BaseGeometry& g) {
    return linear_hub_nozzle_y(data, g, data.theme.extruder_scale);
}

int32_t linear_hub_nozzle_y(const FilamentPathData& data, const BaseGeometry& g, int32_t scale) {
    if (compact_path(view_unit(data)))
        return toolhead_standing_on(g.y_off + g.height - kCompactHeadFoot, scale);
    return toolhead_standing_on(g.y_off + (int32_t)(g.height * kToolheadGlyphBottomRatio), scale);
}

int32_t lane_start_y(const FilamentPathData& data, const LinearHubFrame& f, int lane) {
    // On a box edge, a lane starts where the edge crosses its column.
    if (!f.prep_on_box_edge || f.rail.w != 0 || f.through.w != 0 || lane < 0 ||
        lane >= FilamentPathData::kMaxSlots)
        return f.entry_y;
    return f.entry_y - std::max<int32_t>(0, data.lane_entry_rise[lane]);
}

int32_t lane_band_y(const FilamentPathData& data, const LinearHubFrame& f, int lane) {
    // Below its start, on a line parallel to the edge; a band's width above its row at most.
    if (f.rail.w != 0 || f.through.w != 0 || lane < 0 || lane >= FilamentPathData::kMaxSlots)
        return f.prep_band_y;
    const int32_t start = f.prep_on_box_edge ? lane_start_y(data, f, lane) : f.prep_band_y;
    return std::clamp(start + std::max<int32_t>(0, data.lane_band_drop[lane]), start,
                      std::max(start, f.prep_y - kBandThickness));
}

int32_t linear_hub_required_height(const FilamentPathData& data, BaseGeometry g) {
    // The short stub under a hub on the head holds no buffer at any height, so only the head's
    // size asks for room there.
    const bool wants_buffer = data.buffer_present && !data.hub_on_toolhead && !data.buffer_in_hub;
    const auto fits = [&](int32_t height) {
        g.height = height;
        const int32_t nozzle = linear_hub_nozzle_y(data, g, data.theme.extruder_scale);
        const LinearHubFrame f =
            compute_linear_hub_frame(data, g, toolhead_top_y(nozzle, data.theme.extruder_scale));
        if (f.tool_scale != data.theme.extruder_scale || (wants_buffer && !f.has_buffer))
            return false;
        if (data.view_hub_h <= 0)
            return true;
        // A pass-through unit's own drawing needs its room between the lanes and the hub.
        if (has_through_art(data) && f.through.w <= 0)
            return false;
        // Whatever stands between, the head stands clear below where the lanes leave the
        // unit: their bands, a run of tube and the room over the head.
        const int32_t head_top = toolhead_top_y(nozzle, data.theme.extruder_scale);
        if (head_top - f.prep_band_y < 6 * f.sensor_r + kBandThickness)
            return false;
        // Under a view scale the lanes keep room to converge smoothly into the hub (one and a
        // half hub heights), the hub keeps the room over the head, and a buffer on the trunk
        // its even gaps: as much room over it as the bypass merge and toolhead band take
        // under it.
        const int32_t r = f.sensor_r;
        if (f.hub_y - f.hub_h / 2 - f.prep_band_y < f.hub_h * 3 / 4)
            return false;
        // No lane's diagonal leans flatter than kFanMinSlope, its fillets' straight legs
        // aside: a flatter one reads as a step sideways just above the hub.
        const bool fan =
            data.topology == static_cast<int>(PathTopology::HUB) && !data.hub_on_toolhead;
        const bool compact = compact_path(view_unit(data));
        for (int i = 0; fan && i < LV_MIN(data.slot_count, FilamentPathData::kMaxSlots); ++i) {
            const pg::MergeLaneOut& lane = f.hub_fan[i];
            const float drop = lane.pts[3].y - lane.pts[0].y;
            if (fan_lean_drop(std::fabs(lane.pts[3].x - lane.pts[0].x), compact) > drop + 0.5f)
                return false;
        }
        if (f.output_y + 2 * r > toolhead_top_y(nozzle, data.theme.extruder_scale))
            return false;

        return !wants_buffer || f.buffer_y - f.cap_h / 2 - f.output_y >= 3 * r;
    };
    const int32_t own = g.height;
    // Under a view scale the answer is the least height that lays it out, so a unit above
    // can take what the path leaves; otherwise a canvas that fits keeps its own.
    const bool least = data.view_hub_h > 0;
    if (own <= 0 || (!least && fits(own)))
        return own;
    if (least) {
        // The fan's room is not monotonic in the height (the hub's width follows it), so the
        // least height is the first that fits, walked up. Above the head's top stand at least
        // the lanes' converging room and the hub (two and a half hub heights together) and the
        // room over the head, whatever the fan does; heights short of that lay out no frame.
        const auto head_room = [&](int32_t height) {
            BaseGeometry at = g;
            at.height = height;
            return toolhead_top_y(linear_hub_nozzle_y(data, at, data.theme.extruder_scale),
                                  data.theme.extruder_scale) -
                   g.y_off;
        };
        const int32_t ceiling = height_search_ceiling(own, data);
        int32_t h = data.view_hub_h;
        const int32_t stack = data.view_hub_h * 5 / 2 + 3 * data.theme.sensor_radius;
        while (h <= ceiling && head_room(h) < stack)
            h += 2;
        for (; h <= ceiling; h += 2)
            if (fits(h))
                return h;
        return own;
    }
    int32_t lo = own + 1, hi = own * 4;
    // Where even four times the height does not lay it out at full size, no height asked for
    // would: the canvas keeps its own.
    if (!fits(hi))
        return own;
    while (lo < hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        if (fits(mid))
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

namespace {
// The share of the canvas an on-head selector stands below the lanes' row, past half a hub.
constexpr float kOnHeadSelectorDropRatio = 0.02f;
// Without a view scale: a hub's entries stand this far apart, this far in from each end, and
// its lanes' diagonals keep to this slope near the hub.
constexpr int32_t kRatioEntrySpacing = 22;
constexpr int32_t kRatioEntryMargin = 8;
constexpr float kRatioFanFillet = 8.0f;
constexpr float kRatioFanMaxSlope = 1.2f;
// The least gap kept between the hub's sensor room and the bypass merge (or the buffer's room).
constexpr int32_t kHubMergeGap = 8;
// The run a lane keeps straight under its start before a hub that borrows the trunk's height.
constexpr int32_t kBorrowedFanRun = 24;

/// Sets the hub box's top at @p top, its center row and its outlet under it with it.
void set_hub_top(LinearHubFrame& f, int32_t top) {
    f.hub_y = top + f.hub_h / 2;
    f.output_y = f.hub_y + f.hub_h / 2;
}

int32_t hub_top_of(const LinearHubFrame& f) {
    return f.hub_y - f.hub_h / 2;
}

/// What compute_linear_hub_frame()'s steps share: the canvas, the frame being laid out, and
/// the head's top, which moves down when the head shrinks to fit.
struct HubFrameCtx {
    const FilamentPathData& data;
    const BaseGeometry& g;
    LinearHubFrame& f;
    int32_t glyph_top;
    bool linear = false;
    bool hub_unit = false;             ///< a hub under the unit's lanes, not on the head
    bool trunk_buffer = false;         ///< the buffer stands on the trunk, not in the hub box
    bool selector_buffer = false;      ///< LINEAR's selector with a buffer on the trunk below it
    int32_t r = 0;                     ///< the sensor radius
    int32_t buf_box_h = 0;             ///< the buffer capsule's height
    int32_t badges_bottom = INT32_MIN; ///< the lowest lane number's bottom, plus a radius
    int32_t lanes_top = 0;             ///< the highest a hub's top stands: under the lanes' starts
    bool buffer_placed = false;        ///< place_buffer_under_unit() decided the buffer's place
};

// The gap above and below a buffer on the trunk, and the room a selector's jog to the center
// takes above it.
int32_t buffer_gap(const HubFrameCtx& c) {
    return c.r;
}
int32_t jog_room(const HubFrameCtx& c) {
    return 2 * c.r;
}

/// The room the trunk keeps under a hub: with a buffer, a gap, the buffer and an equal gap, each
/// gap the buffer's least gap plus the band's clear room above the head where the bypass merges
/// and the toolhead band sit; with none, only that room, and a radius more where the bypass
/// merges below the hub (the output sensor stands inside the hub box).
int32_t room_under_hub(const HubFrameCtx& c, bool buffer) {
    if (buffer)
        return 2 * (buffer_gap(c) + 2 * c.r) + c.buf_box_h;
    return (c.data.show_bypass ? c.r : 0) + 2 * c.r;
}

/// The lowest a hub's top stands and still leaves its sensor room and a gap above the bypass
/// merge, or above the buffer's room when the trunk carries one.
int32_t lowest_free_hub_top(const HubFrameCtx& c) {
    const LinearHubFrame& f = c.f;
    const int32_t buffer_room = 2 * f.sensor_r + 2 * c.buf_box_h;
    const int32_t next_y = f.bypass_merge_y - (f.has_buffer ? buffer_room : 0);
    return next_y - f.hub_h - 2 * f.sensor_r - kHubMergeGap;
}

/// The ratio layout's rows, the lanes' row moved onto the spool box's edge where they leave it
/// there, and the sizes every later step reads.
void base_rows(const FilamentPathData& data, const BaseGeometry& g, LinearHubFrame& f) {
    f.entry_y = g.y_off + (int32_t)(g.height * kEntryYRatio);
    f.prep_y = lanes_row_y(data, g);
    f.prep_band_y = lanes_leave_y(data, g);
    if (f.prep_band_y != f.prep_y) {
        f.entry_y = f.prep_band_y;
        f.prep_on_box_edge = true;
    }
    f.hub_y = g.y_off + (int32_t)(g.height * kHubYRatio);
    f.hub_h = data.view_hub_h > 0 ? data.view_hub_h : (int32_t)(g.height * kHubHeightRatio);
    f.toolhead_y = g.y_off + (int32_t)(g.height * kToolheadYRatio);
    // The selector is part of the unit, butted against the prep sensors.
    if (data.hub_on_toolhead)
        f.selector_y =
            f.prep_y + (int32_t)(g.height * (kHubHeightRatio / 2 + kOnHeadSelectorDropRatio));
    f.sensor_r = data.theme.sensor_radius;
    f.cap_h = data.view_cap_h > 0 ? data.view_cap_h : buffer_capsule_height(f.hub_h);
    // A buffer riding in the hub box takes no room on the trunk. A selector drawn as its rail
    // art carries it on the art itself.
    f.rail = rail_art_rect(data, g);
    f.buffer_in_hub = data.buffer_present && data.buffer_in_hub && !data.hub_on_toolhead;
    f.tool_scale = data.theme.extruder_scale;
    f.nozzle_y = linear_hub_nozzle_y(data, g, f.tool_scale);
}

/// Under a drawn unit each lane's number stands centered on its band, the band dropped below
/// the lane's start by at least half a number (the least drop): the lowest number's bottom
/// plus a radius, INT32_MIN when no lane drops its band.
int32_t lane_numbers_bottom(const FilamentPathData& data, const LinearHubFrame& f) {
    const int n = LV_MIN(data.slot_count, FilamentPathData::kMaxSlots);
    int32_t least_drop = INT32_MAX;
    for (int i = 0; i < n; ++i)
        if (data.lane_band_drop[i] > 0)
            least_drop = std::min(least_drop, data.lane_band_drop[i]);
    int32_t bottom = INT32_MIN;
    for (int i = 0; i < n; ++i)
        if (data.lane_band_drop[i] > 0)
            bottom = std::max(bottom, lane_band_y(data, f, i) + least_drop + f.sensor_r);
    return bottom;
}

/// The selector's bottom edge: its rail art's outlet, or the bar's bottom.
int32_t selector_bottom(const HubFrameCtx& c) {
    const LinearHubFrame& f = c.f;
    return f.rail.w > 0 ? rail_outlet_y(*c.data.rail_art, f.rail) : f.prep_y + c.r + f.hub_h;
}

/// A selector (a bar, or its rail art) with a buffer: the filament leaves under the active lane
/// and jogs to the center, and the buffer stands on the final drop below that jog. The selector
/// rises toward the lanes to make the room, as a hub does.
void raise_selector_for_buffer(HubFrameCtx& c) {
    if (!c.selector_buffer)
        return;
    LinearHubFrame& f = c.f;
    const int32_t deficit =
        selector_bottom(c) - (c.glyph_top - room_under_hub(c, c.trunk_buffer) - jog_room(c));
    if (deficit > 0 && f.rail.w > 0) {
        const int32_t min_top = lanes_leave_y(c.data, c.g) + art_fan_room(c.data, c.g);
        f.rail.y -= LV_MIN(deficit, LV_MAX(0, f.rail.y - min_top));
    } else if (deficit > 0 && f.prep_on_box_edge) {
        // The bar stays butted against the prep sensors; they rise toward the box edge,
        // keeping a band's room under it.
        f.prep_y -= LV_MIN(deficit, LV_MAX(0, f.prep_y - (f.prep_band_y + 2 * c.r)));
    }
}

/// Without a view scale, a canvas too short for the risen unit and the room under it above a
/// full-size head draws a smaller head, down to a third of its size. A view scale keeps its
/// head at every height: the art above gives up the room instead.
void fit_toolhead_scale(HubFrameCtx& c) {
    if (!(c.hub_unit || c.selector_buffer) || c.data.view_hub_h > 0)
        return;
    LinearHubFrame& f = c.f;
    const int32_t min_scale = LV_MAX(3, c.data.theme.extruder_scale / 3);
    const auto need = [&] {
        const int32_t under = room_under_hub(c, c.trunk_buffer);
        return c.hub_unit ? c.lanes_top + f.hub_h + under
                          : selector_bottom(c) + under + jog_room(c);
    };
    while (f.tool_scale > min_scale && c.glyph_top < need()) {
        --f.tool_scale;
        f.nozzle_y = linear_hub_nozzle_y(c.data, c.g, f.tool_scale);
        c.glyph_top = toolhead_top_y(f.nozzle_y, f.tool_scale);
    }
}

/// The head, its inlet and the trunk's column; the rows just over the head (the toolhead band
/// and the bypass merge), and a hub on the head over its short stub.
void place_head(HubFrameCtx& c) {
    const FilamentPathData& data = c.data;
    const BaseGeometry& g = c.g;
    LinearHubFrame& f = c.f;
    const helix::ui::ToolheadInlet inlet = path_toolhead_inlet(f.nozzle_y, f.tool_scale);
    // The head is drawn first and the tube painted over it, down to the head's filament inlet.
    f.inlet_y = inlet.y;
    // Under a view scale a hub under its own lanes stands centered on them, the trunk and head
    // with it, so each lane drops straight into its entry.
    int32_t trunk_x = g.center_x;
    if (data.view_hub_h > 0 && c.hub_unit && data.slot_count > 1 &&
        data.slot_count <= FilamentPathData::kMaxSlots && data.offpage_before <= 0 &&
        data.offpage_after <= 0)
        trunk_x = (g.slot_x[0] + g.slot_x[data.slot_count - 1]) / 2;
    f.head_x = trunk_x + inlet.dx;
    f.center_x = trunk_x;
    // The toolhead sensor band stays in the clear room above the head, so the tube never
    // runs down over the head's face to reach it.
    f.toolhead_y = LV_MIN(f.toolhead_y, c.glyph_top - c.r);
    // The hub moves down to the print head with a short shared stub below.
    if (data.hub_on_toolhead)
        f.hub_y = f.toolhead_y - hub_stub_height(g) - f.hub_h / 2;
    f.bypass_merge_y = g.y_off + (int32_t)(g.height * kBypassMergeYRatio);
    if (c.hub_unit)
        f.bypass_merge_y = LV_MIN(f.bypass_merge_y, c.glyph_top - 2 * c.r);
}

/// The buffer hangs just under the outlet: on many units it is built onto the unit itself, so
/// it never stands down by the toolhead. Where it cannot fit above the bypass merge (or the
/// toolhead sensor), its reading moves to the outlet's band. A buffer
/// place_buffer_under_unit() placed stays where it is.
void hang_buffer_under_outlet(HubFrameCtx& c) {
    LinearHubFrame& f = c.f;
    if (!f.has_buffer || c.buffer_placed)
        return;
    f.buffer_y = f.output_y + 2 * f.sensor_r + c.buf_box_h;
    f.buf_fil_top = f.buffer_y - c.buf_box_h;
    const int32_t floor = LV_MIN(c.data.show_bypass ? f.bypass_merge_y : f.toolhead_y, c.glyph_top);
    if (f.buffer_y + c.buf_box_h / 2 + 2 * f.sensor_r > floor) {
        f.has_buffer = false;
        f.sync_on_output = true;
    }
}

/// Places the buffer evenly between the unit's outlet (f.output_y) and the head: the gap above
/// it equals the gap below, and the one below holds the bypass merge and the toolhead band in
/// the clear room over the head. @p top is the highest its gap may start (a selector's jog to
/// the center ends there), which only ever pushes it lower. A room too short for even gaps
/// keeps that room below and at least the buffer's gap above; shorter still, the buffer's
/// reading moves to the unit's outlet band.
void place_buffer_under_unit(HubFrameCtx& c, int32_t top) {
    LinearHubFrame& f = c.f;
    c.buffer_placed = true;
    f.toolhead_y = c.glyph_top - c.r;
    if (c.data.show_bypass)
        f.bypass_merge_y = c.glyph_top - 2 * c.r;
    if (!f.has_buffer)
        return;
    const int32_t half = c.buf_box_h / 2;
    const int32_t below = buffer_gap(c) + 2 * c.r;
    int32_t y = LV_MAX((f.output_y + c.glyph_top) / 2, top + below + half);
    y = LV_MIN(y, c.glyph_top - below - half);
    if (y - half < LV_MAX(f.output_y, top) + buffer_gap(c)) {
        f.has_buffer = false;
        f.sync_on_output = true;
        return;
    }
    f.buffer_y = y;
    f.buf_fil_top = LV_MAX(LV_MAX(f.output_y, top), y - half - below);
}

/// HUB with no bypass lane and no buffer: the hub hangs above the toolhead glyph, giving the
/// lane fan the height it would otherwise occupy. The stack only ever moves it down; a canvas
/// too short for it keeps the ratio layout.
void stack_hub_over_head(HubFrameCtx& c) {
    LinearHubFrame& f = c.f;
    if (c.linear || c.data.hub_on_toolhead || c.data.show_bypass || f.has_buffer)
        return;
    const int32_t gap = f.hub_h / 2 + 2 * f.sensor_r;
    const int32_t hub_bottom = c.glyph_top - 2 * gap;
    const int32_t stacked_hub_y = hub_bottom - f.hub_h / 2;
    if (stacked_hub_y > f.hub_y) {
        f.hub_stacked = true;
        f.hub_y = stacked_hub_y;
        // The toolhead sensor band sits in the clear gap above the glyph.
        f.toolhead_y = (hub_bottom + c.glyph_top) / 2;
    }
}

/// The frame's colors and the path's filament and error state.
void resolve_frame_state(const FilamentPathData& data, const BaseGeometry& g, LinearHubFrame& f) {
    const ThemeCache& theme = data.theme;
    f.idle_color = theme.color_idle;
    f.active_color = lv_color_hex(data.filament_color);
    f.hub_bg = theme.color_hub_bg;
    f.hub_border = theme.color_hub_border;
    f.error_color = pulsed_error_color(data);

    f.output_x = f.center_x;
    if (data.topology == static_cast<int>(PathTopology::LINEAR) && data.active_slot >= 0 &&
        data.active_slot < g.slot_count) {
        f.output_x = data.anim.output_x_active ? data.anim.output_x_current
                                               : linear_exit_x(data, g, data.active_slot);
    }

    f.has_error = data.error_segment > 0;
    f.error_seg = static_cast<PathSegment>(data.error_segment);
    f.fil_seg = static_cast<PathSegment>(data.filament_segment);
    f.states = compute_slot_render_states(&data);
}

/// Where a pass-through unit's art went: the highest the hub's top may stand under it, and
/// whether the art took the selector's slot (the hub then hangs right under it).
struct ThroughPlacement {
    int32_t floor = INT32_MIN;
    bool in_slot = false;
};

/// A pass-through unit stands between the lanes' sensors and the hub, as tall as the room
/// above the hub's lowest place allows; the lanes fan into the hub from its outlets. With no
/// room above the hub's place the art takes the selector's slot and the hub hangs below it,
/// clear of the bypass merge: the buffer keeps its place under the hub where the room is there,
/// otherwise its sync reading moves to the hub's outlet band.
ThroughPlacement place_through_art(HubFrameCtx& c) {
    const FilamentPathData& data = c.data;
    LinearHubFrame& f = c.f;
    ThroughPlacement out;
    const int32_t fan = art_fan_room(data, c.g);
    const int32_t lowest_hub_top =
        f.hub_stacked ? hub_top_of(f)
                      : LV_MIN(lowest_free_hub_top(c),
                               c.glyph_top - room_under_hub(c, c.trunk_buffer) - f.hub_h);
    // The lanes converge from the whole row of spools onto the unit: twice a rail's room.
    const int32_t top = f.prep_band_y + 2 * fan;
    const int32_t bottom =
        LV_MIN(lowest_hub_top - 2 * fan, top + (int32_t)(c.g.height * kThroughMaxHRatio));
    f.through = through_art_rect(data, c.g, top, bottom, f.center_x);
    if (f.through.w > 0) {
        // Clear of the art's outlets by the lanes' fan room.
        out.floor = f.through.y + f.through.h + 2 * fan;
        return out;
    }
    if (!has_through_art(data))
        return out;
    const RailArtRect slot = selector_slot_rect(data, c.g);
    const int32_t below = slot.y + slot.h + fan;
    const int32_t limit = (data.show_bypass ? f.bypass_merge_y : c.glyph_top) - 2 * f.sensor_r;
    if (slot.w <= 0 || below + f.hub_h > limit)
        return out;
    f.through = slot;
    out.floor = below;
    out.in_slot = true;
    if (below + f.hub_h + 2 * buffer_gap(c) + c.buf_box_h + 2 * c.r > c.glyph_top) {
        f.has_buffer = false;
        f.sync_on_output = c.trunk_buffer;
    }
    return out;
}

/// Where each lane starts its run to the hub: the on-toolhead selector's bottom edge, a
/// pass-through unit's outlet, else its prep sensor. Returns the lane count.
int gather_fan_starts(const HubFrameCtx& c, pg::MergeLaneIn* fan_in) {
    const FilamentPathData& data = c.data;
    const LinearHubFrame& f = c.f;
    const int n = LV_MIN(data.slot_count, FilamentPathData::kMaxSlots);
    for (int i = 0; i < n; i++) {
        int32_t start_y = data.slot_has_prep_sensor[i] ? (f.prep_y + f.sensor_r) : f.prep_y;
        int32_t start_x = c.g.slot_x[i];
        if (data.hub_on_toolhead)
            start_y = f.selector_y + f.hub_h / 2;
        if (f.through.w > 0) {
            start_x = rail_outlet_x(*data.rail_art, f.through, i);
            start_y = rail_outlet_y(*data.rail_art, f.through);
        }
        fan_in[i] = {(float)start_x, (float)start_y};
    }
    return n;
}

/// How a hub's lanes fan into it, and the widths its box may take.
struct HubFanShape {
    int32_t separation = 0;   ///< centerline distance between neighbouring tubes
    int32_t entry_margin = 0; ///< the outer entries' distance in from the box's ends
    float fillet = 0.0f;
    float max_slope = 0.0f;
    int32_t min_width = 0;
    int32_t max_width = 0;
};

/// Horizontal entry spacing alone does not keep shallow diagonals apart: the hub is widened
/// until neighbouring tubes clear the tube, its halo and a gap. Under a view scale each lane
/// runs one long diagonal from its outlet into the hub, all the height there is, so the fan
/// converges without a step above the hub, and the hub keeps to the lanes it gathers (entries a
/// tube's clearance apart, a sensor's room in from each end): it is only as wide as its
/// entries, and the path takes the height the diagonals need rather than a wider hub.
HubFanShape hub_fan_shape(const HubFrameCtx& c, const pg::MergeLaneIn* fan_in, int n) {
    const FilamentPathData& data = c.data;
    const int32_t hub_w = data.theme.hub_width;
    const bool scaled = data.view_hub_h > 0;
    HubFanShape s;
    s.separation = (int32_t)lane_clearance(data);
    s.max_slope = scaled ? kSmoothFanSlope : kRatioFanMaxSlope;
    const int32_t entry_spacing = scaled ? s.separation + 2 : kRatioEntrySpacing;
    s.entry_margin = scaled ? c.f.sensor_r + 2 : kRatioEntryMargin;
    s.fillet = scaled ? kScaledFanFillet : kRatioFanFillet;
    const int32_t want_w = n > 1 ? (n - 1) * entry_spacing + 2 * s.entry_margin : hub_w;
    const int32_t slot_span = n > 1 ? (int32_t)(fan_in[n - 1].slot_x - fan_in[0].slot_x) : hub_w;
    s.max_width = LV_MAX(hub_w, slot_span + 2 * s.entry_margin);
    s.min_width = LV_CLAMP(hub_w, want_w, s.max_width);
    if (scaled)
        s.max_width = s.min_width;
    return s;
}

/// Under a view scale the hub stands just below the lanes' fan: the highest top every lane
/// reaches, each from its own start, on a diagonal no flatter than kFanMinSlope into its entry
/// at the hub's least width.
int32_t scaled_fan_hub_top(const HubFrameCtx& c, const pg::MergeLaneIn* fan_in, int n,
                           const HubFanShape& s) {
    const LinearHubFrame& f = c.f;
    const float usable = (float)(s.min_width - 2 * s.entry_margin);
    const float step = usable / (float)(n - 1);
    const bool compact = compact_path(view_unit(c.data));
    float need = -1e9f;
    for (int i = 0; i < n; ++i) {
        const float offset = (float)i - (float)(n - 1) / 2.0f;
        const float run = std::fabs((float)f.center_x + step * offset - fan_in[i].slot_x);
        const float start = std::max(fan_in[i].start_y, (float)f.prep_band_y);
        need = std::max(need, start + fan_drop_needed(run, f.hub_h, compact));
    }
    return (int32_t)std::ceil(need + (float)f.sensor_r);
}

/// The hub's top, every rule in the order it applies, each later one winning:
/// 1. Under a pass-through unit's art, clear of its outlets (or right under it in the
///    selector's slot).
/// 2. Three or more lanes borrow unused trunk height before the hub widens, keeping the
///    buffer and bypass room clear (not on the head, not stacked over it).
/// 3. Never over the lanes' numbers.
/// 4. A hub whose place leaves too little room under it, and every hub under a view scale,
///    rises toward the lanes (under the art and the lanes' starts still): the room under it is
///    sized for the buffer the hub still carries. Under a view scale it stands just below the
///    lanes' fan, and whatever the canvas has past that goes to the trunk down to the head.
/// @p risen is set when rule 4 placed it.
int32_t choose_hub_top(const HubFrameCtx& c, const ThroughPlacement& art,
                       const pg::MergeLaneIn* fan_in, int n, const HubFanShape& s, bool& risen) {
    const FilamentPathData& data = c.data;
    const LinearHubFrame& f = c.f;
    risen = false;
    int32_t top = hub_top_of(f);
    if (art.in_slot)
        top = art.floor;
    else if (f.through.w > 0)
        top = LV_MAX(top, art.floor);
    if (n > 2 && !data.hub_on_toolhead && !f.hub_stacked) {
        float deepest_start = fan_in[0].start_y;
        for (int i = 1; i < n; ++i)
            deepest_start = std::max(deepest_start, fan_in[i].start_y);
        const int32_t wanted_top =
            (int32_t)deepest_start + kBorrowedFanRun + 2 * s.separation + f.sensor_r;
        top = LV_MAX(top, LV_MIN(wanted_top, lowest_free_hub_top(c)));
    }
    if (data.hub_on_toolhead)
        return top;
    if (top < c.badges_bottom)
        top = c.badges_bottom;
    const int32_t roomy_top = c.glyph_top - room_under_hub(c, f.has_buffer) - f.hub_h;
    const bool scaled = data.view_hub_h > 0;
    if (top <= roomy_top && !scaled)
        return top;
    int32_t rise_to = roomy_top;
    if (scaled && n > 1)
        rise_to = std::min(roomy_top, scaled_fan_hub_top(c, fan_in, n, s));
    risen = true;
    return LV_MAX(LV_MAX(rise_to, c.lanes_top), art.floor);
}

/// The lanes' fan into the hub at @p hub_top: the hub as wide as a fan ending one sensor radius
/// above it needs (so the final legs keep their clearance across the hub-entry bands), the
/// fan's lanes, laid by the tube router under a view scale.
void build_hub_fan(HubFrameCtx& c, const pg::MergeLaneIn* fan_in, int n, const HubFanShape& s,
                   int32_t hub_top) {
    LinearHubFrame& f = c.f;
    const int32_t tube_end_y = hub_top - f.sensor_r;
    f.hub_box_w = (int32_t)std::ceil(pg::merge_fan_width(
        fan_in, n, (float)f.center_x, (float)tube_end_y, (float)s.min_width, (float)s.max_width,
        (float)s.entry_margin, s.fillet, s.max_slope, (float)s.separation));
    pg::build_merge_fan(fan_in, n, (float)f.center_x, (float)hub_top, (float)f.hub_box_w,
                        (float)s.entry_margin, s.fillet, s.max_slope, f.hub_fan);
    if (c.data.view_hub_h <= 0)
        return;
    route_fan(fan_in, f.hub_fan, n, (float)s.separation);
    // A lane within a couple of pixels of its entry drops straight in: a sub-pixel offset drawn
    // through two fillets reads as a step.
    for (int i = 0; i < n; ++i) {
        pg::MergeLaneOut& lane = f.hub_fan[i];
        if (std::fabs(lane.pts[3].x - lane.pts[0].x) <= kStraightLaneSlack)
            for (int k = 1; k < 4; ++k)
                lane.pts[k].x = lane.pts[0].x;
    }
}
} // namespace

LinearHubFrame compute_linear_hub_frame(const FilamentPathData& data, const BaseGeometry& g) {
    return compute_linear_hub_frame(
        data, g, toolhead_top_y(linear_hub_nozzle_y(data, g), data.theme.extruder_scale));
}

LinearHubFrame compute_linear_hub_frame(const FilamentPathData& data, const BaseGeometry& g,
                                        int32_t glyph_top) {
    LinearHubFrame f;
    base_rows(data, g, f);
    HubFrameCtx c{data, g, f, glyph_top};
    c.linear = data.topology == static_cast<int>(PathTopology::LINEAR);
    c.hub_unit = !c.linear && !data.hub_on_toolhead;
    c.trunk_buffer = data.buffer_present && !f.buffer_in_hub;
    c.selector_buffer = c.linear && !data.hub_on_toolhead && c.trunk_buffer;
    c.r = f.sensor_r;
    c.buf_box_h = f.cap_h;
    // The lanes' fan needs a band's room between where they leave the spools and the hub top,
    // clear of the lanes' numbers.
    c.badges_bottom = lane_numbers_bottom(data, f);
    c.lanes_top =
        std::max((f.prep_on_box_edge ? f.prep_band_y : f.prep_y + c.r) + 2 * c.r, c.badges_bottom);

    raise_selector_for_buffer(c);
    fit_toolhead_scale(c);
    place_head(c);
    f.has_buffer = c.trunk_buffer;
    stack_hub_over_head(c);

    // LINEAR's selector is butted against the prep sensors; the output sensor against the hub's
    // bottom (mirroring the hub-top entries), or at the selected gate's outlet on rail art.
    set_hub_top(f, c.linear ? f.prep_y + f.sensor_r : hub_top_of(f));
    if (f.rail.w > 0)
        f.output_y = rail_outlet_y(*data.rail_art, f.rail);
    // The buffer stands on the drop below the jog from the outlet to the center.
    if (c.selector_buffer && f.has_buffer)
        place_buffer_under_unit(c, f.output_y + jog_room(c));
    resolve_frame_state(data, g, f);
    f.hub_box_w = data.theme.hub_width;
    if (c.linear) {
        hang_buffer_under_outlet(c);
        return f;
    }

    const ThroughPlacement art = place_through_art(c);
    pg::MergeLaneIn fan_in[FilamentPathData::kMaxSlots];
    const int fan_n = gather_fan_starts(c, fan_in);
    const HubFanShape shape = hub_fan_shape(c, fan_in, fan_n);
    bool risen = false;
    const int32_t hub_top = choose_hub_top(c, art, fan_in, fan_n, shape, risen);
    set_hub_top(f, hub_top);
    if (risen) {
        // Under the risen hub the buffer sits centered with even gaps, and below it the bypass
        // merges and the toolhead band sits in the clear room above the head. The lanes start
        // their fan above it.
        place_buffer_under_unit(c, f.output_y);
        const float fan_start = (float)LV_MAX(f.prep_band_y, hub_top - 2 * c.r);
        if (f.prep_on_box_edge)
            f.prep_y = LV_MAX(f.prep_band_y, LV_MIN(f.prep_y, hub_top - 2 * c.r));
        for (int i = 0; i < fan_n; ++i)
            fan_in[i].start_y = std::min(fan_in[i].start_y, fan_start);
    }
    build_hub_fan(c, fan_in, fan_n, shape, hub_top);
    // Units on the hub's other pages: a chip beside the hub, drawn with it.
    if (!data.hub_on_toolhead) {
        f.stubs[0] = {data.offpage_before > 0, data.offpage_before, data.offpage_before_drying};
        f.stubs[1] = {data.offpage_after > 0, data.offpage_after, data.offpage_after_drying};
    }
    // The buffer stands centered on the trunk between the hub and the head.
    if (f.has_buffer && !c.buffer_placed)
        place_buffer_under_unit(c, f.output_y);
    return f;
}

namespace {

// The Y @p fraction of the way from @p top to @p bottom.
int32_t row_between(int32_t top, int32_t bottom, float fraction) {
    return top + (int32_t)((bottom - top) * fraction);
}

} // namespace

namespace {
namespace tr = helix::ui::tuberoute;

/// The router's settings for lanes converging under a view scale: a tube's clearance between
/// any two, short fillets, 45-degree jogs where the height allows and the flattest the fan
/// takes (kFanMinSlope) where it does not, never a sideways run, the jogs low near the sinks.
tr::RouterConfig scaled_router(float clearance, float slope) {
    tr::RouterConfig cfg;
    cfg.clearance = clearance;
    cfg.fillet_r = kScaledFanFillet;
    cfg.slope = slope;
    cfg.alt_slope = slope > kFanMinSlope ? kFanMinSlope : 0.0f;
    cfg.allow_horizontal = false;
    cfg.align = 1.0f;
    return cfg;
}
} // namespace

float lane_clearance(const FilamentPathData& data) {
    return (float)LV_MAX(haloed_tube_width(data.theme.tube_gauge) + 2,
                         2 * data.theme.sensor_radius + 2);
}

float view_unit(const FilamentPathData& data) {
    return data.view_unit > 0.0f ? data.view_unit : (float)data.view_hub_h / kPathHubUnits;
}

bool route_lanes(const pg::PathPoint* sources, pg::MergeLaneOut* out, int n, float clearance) {
    if (n <= 0 || n > tr::kMaxTraces)
        return false;
    pg::PathPoint sinks[tr::kMaxTraces];
    for (int i = 0; i < n; ++i)
        sinks[i] = out[i].pts[3];
    const tr::Bus bus{sources, sinks, n};
    tr::Trace traces[tr::kMaxTraces];
    if (tr::plan_routes(&bus, 1, scaled_router(clearance, 1.0f), traces, tr::kMaxTraces) !=
        tr::RouteStatus::Ok)
        return false;
    for (int i = 0; i < n; ++i) {
        const tr::Trace& t = traces[i];
        if (t.count == 4) {
            for (int k = 0; k < 4; ++k)
                out[i].pts[k] = t.pts[k];
        } else {
            const pg::PathPoint a = t.pts[0], b = t.pts[t.count - 1];
            out[i].pts[0] = a;
            out[i].pts[1] = {a.x, a.y + (b.y - a.y) / 3.0f};
            out[i].pts[2] = {a.x, a.y + 2.0f * (b.y - a.y) / 3.0f};
            out[i].pts[3] = b;
        }
    }
    return true;
}

void route_fan(const pg::MergeLaneIn* fan_in, pg::MergeLaneOut* fan, int n, float clearance) {
    if (n <= 0)
        return;
    pg::PathPoint src[FilamentPathData::kMaxSlots];
    for (int i = 0; i < n; ++i)
        src[i] = {fan_in[i].slot_x, fan_in[i].start_y};
    route_lanes(src, fan, n, clearance);
}

int32_t head_scale_within_height(int32_t scale, float max_h) {
    while (scale > kMinHeadScale) {
        const GlyphBounds b = toolhead_glyph_bounds(0, 0, scale);
        if ((float)(b.bottom - b.top) <= max_h)
            break;
        --scale;
    }
    return scale;
}

namespace {
/// The share of the canvas width a row of per-lane heads may take under a view scale.
constexpr int32_t kHeadRowShareNum = 1, kHeadRowShareDen = 2;
/// The gap a scaled row of heads' tool tags leaves above the canvas bottom.
constexpr int32_t kHeadsFootMargin = 2;

/// The size of @p heads per-lane heads standing in a row: under a view scale the hub layout's
/// (no taller than kHeadUnits), shrunk where the row would take more than kHeadRowShare of the
/// canvas width, so several heads leave the drawing above its height.
int32_t per_lane_tool_scale(const FilamentPathData& data, const BaseGeometry& g, int heads) {
    const int32_t base = data.theme.extruder_scale;
    if (data.view_hub_h <= 0)
        return LV_MAX(6, base * 2 / 3);
    const int32_t budget = g.width * kHeadRowShareNum / kHeadRowShareDen;
    const auto row = [&](int32_t scale) {
        const GlyphBounds b = toolhead_glyph_bounds(0, 0, scale);
        return heads * ((b.right - b.left) + 4 * data.theme.sensor_radius);
    };
    const float unit = view_unit(data);
    int32_t scale = head_scale_within_height(base, kHeadUnits * unit);
    while (heads > 1 && scale > kMinHeadScale && row(scale) > budget)
        --scale;
    return scale;
}
/// Where a row of per-lane heads stands: @p ratio of the way down the canvas, or under a view
/// scale on its foot, with room for the tool tags hanging below them and no more.
int32_t per_lane_heads_bottom(const FilamentPathData& data, const BaseGeometry& g, float ratio) {
    if (data.view_hub_h > 0)
        return g.y_off + g.height - data.theme.tool_tag_h / 2 - kHeadsFootMargin;
    return g.y_off + (int32_t)(g.height * ratio);
}
} // namespace

ParallelRows parallel_rows(const FilamentPathData& data, const BaseGeometry& g) {
    ParallelRows r;
    r.tool_scale = per_lane_tool_scale(data, g, LV_MIN(g.slot_count, FilamentPathData::kMaxSlots));
    r.entry_y = g.y_off + (int32_t)(g.height * kEntryYRatio);
    r.toolhead_y = toolhead_standing_on(per_lane_heads_bottom(data, g, kParallelGlyphBottomRatio),
                                        r.tool_scale);
    r.sensor_y = row_between(r.entry_y, r.toolhead_y, kParallelSensorFraction);
    // The tubes hang from the spools' bottom edge, the round cap's top on it.
    if (const int32_t edge = box_edge_above(data, g, tube_cap(data), r.sensor_y);
        edge != INT32_MIN) {
        r.entry_y = edge;
        r.sensor_y = row_between(r.entry_y, r.toolhead_y, kParallelSensorFraction);
    }
    // A lane's entry sensor clamps the tube three gauges below its cap, clear
    // of the spool's glow, and keeps two gauges and a band of tube above the
    // toolhead sensor. On a run too short for both it stands on the cap.
    const int32_t gauge = data.theme.tube_gauge;
    r.prep_y =
        LV_MAX(r.entry_y, LV_MIN(r.entry_y + 3 * gauge, r.sensor_y - 2 * gauge - kBandThickness));
    return r;
}

int32_t parallel_head_x(const FilamentPathData& data, const BaseGeometry& g,
                        const ParallelRows& rows, int i) {
    const int n = LV_MIN(g.slot_count, FilamentPathData::kMaxSlots);
    if (n < 2 || i < 0 || i >= n)
        return i >= 0 && i < FilamentPathData::kMaxSlots ? g.slot_x[i] : g.center_x;
    const GlyphBounds b = toolhead_glyph_bounds(0, 0, rows.tool_scale);
    const int32_t step = (b.right - b.left) + 4 * data.theme.sensor_radius;
    const int32_t pitch = (g.slot_x[n - 1] - g.slot_x[0]) / (n - 1);
    if (pitch >= step)
        return g.slot_x[i];
    const int32_t half = (b.right - b.left) / 2;
    const int32_t span = std::min<int32_t>((n - 1) * step, g.width - 2 * half);
    const int32_t each = span / (n - 1);
    int32_t left = (g.slot_x[0] + g.slot_x[n - 1]) / 2 - span / 2;
    left =
        std::clamp(left, g.x_off + half, std::max(g.x_off + half, g.x_off + g.width - half - span));
    return left + i * each;
}

/// The lowest a MIXED hub's top stands: a short stub of shared tube over its head.
static int32_t mixed_hub_lowest_top(const BaseGeometry& g, const MixedFrame& f) {
    return toolhead_glyph_bounds(0, f.toolhead_y, f.tool_scale).top - hub_stub_height(g) - f.hub_h;
}

/// The highest a MIXED hub's top stands for the lanes from a pass-through unit's outlets to
/// reach it on diagonals no flatter than kFanMinSlope.
static int32_t mixed_hub_fan_top(const FilamentPathData& data, const MixedFrame& f) {
    // The hub's entries spread across its top, so a lane runs in only to its nearest one.
    const float reach = std::max(0.0f, (float)f.hub_w / 2 - 8.0f);
    float run = 0.0f;
    for (int i = 0; i < LV_MIN(data.slot_count, FilamentPathData::kMaxSlots); i++) {
        if (data.slot_is_hub_routed[i])
            run = std::max(run, std::fabs((float)(f.head_x[i] - f.hub_cx)) - reach);
    }
    return f.sensor_y + data.theme.sensor_radius +
           (int32_t)std::ceil(fan_drop_needed(run, f.hub_h));
}

namespace {
/// View units (path_view_scale.h) at and below which a per-lane path takes its compact runs,
/// and at and above which its full ones.
constexpr float kCompactViewUnit = 5.0f;
constexpr float kRoomyViewUnit = 9.0f;
} // namespace

int32_t per_lane_required_height(const FilamentPathData& data, BaseGeometry g) {
    if (data.view_hub_h <= 0 || g.height <= 0)
        return 0;
    const bool mixed = data.topology == static_cast<int>(PathTopology::MIXED);
    // A run from each lane's outlet down to its head long enough to read as a tube with its
    // sensors on it; a shared hub's run also holds the hub between them; and a pass-through
    // unit's own drawing its room.
    // The least runs in view units: a MIXED lane's holds its hub; a PARALLEL lane's only its
    // sensor. A compact view (few pixels to the unit) spends more of its column on text that
    // does not shrink with it, so its runs shorten toward the compact length.
    const float unit = view_unit(data);
    const float roomy =
        std::clamp((unit - kCompactViewUnit) / (kRoomyViewUnit - kCompactViewUnit), 0.0f, 1.0f);
    const auto run_units = [&](float compact, float full) {
        return (compact + (full - compact) * roomy) * unit;
    };
    const int32_t run_need = (int32_t)std::lround(run_units(9.0f, 11.5f));
    bool require_art = true;
    const auto fits = [&](int32_t height) {
        g.height = height;
        if (mixed) {
            const MixedFrame f = compute_mixed_frame(data, g);
            if (require_art && has_through_art(data) && f.through.w <= 0)
                return false;
            // The hub's lanes reach it on diagonals steep enough.
            if (!data.hub_on_toolhead && f.hub_count > 0 &&
                mixed_hub_fan_top(data, f) > mixed_hub_lowest_top(g, f))
                return false;
            const int32_t head_top = toolhead_glyph_bounds(0, f.toolhead_y, f.tool_scale).top;
            return head_top - f.entry_y >= run_need;
        }
        // Each lane runs to its head only as far as its sensor's room or its lean out to a head
        // spread from the others needs, no flatter than a hub's lanes.
        const ParallelRows r = parallel_rows(data, g);
        const int32_t head_top = toolhead_glyph_bounds(0, r.toolhead_y, r.tool_scale).top;
        float spread = 0.0f;
        for (int i = 0; i < LV_MIN(g.slot_count, FilamentPathData::kMaxSlots); i++)
            spread =
                std::max(spread, (float)std::abs(parallel_head_x(data, g, r, i) - g.slot_x[i]));
        const float lean = spread > 1.0f ? fan_lean_drop(spread) : 0.0f;
        return (float)(head_top - r.entry_y) >= std::max(run_units(6.0f, 9.2f), lean);
    };
    const int32_t ceiling = height_search_ceiling(g.height, data);
    const int32_t own = g.height;
    // A pass-through unit too wide for any height here goes undrawn: the path then asks only
    // for its lanes.
    for (int pass = 0; pass < 2; ++pass, require_art = false) {
        for (int32_t h = data.view_hub_h; h <= ceiling; h += 2)
            if (fits(h))
                return h;
    }
    return own;
}

// MIXED layout: the toolhead glyphs stand on a line near the bottom; the lane
// sensors and the hub sit fixed fractions of the way down to the toolheads.
namespace {
/// Whether a MIXED path's lanes leave a pass-through unit drawn at @p through from its own
/// outlets: only under a view scale. (A HUB's lanes leave the outlets at every scale.)
bool mixed_lanes_leave_outlets(const FilamentPathData& data, const RailArtRect& through) {
    return through.w > 0 && data.view_hub_h > 0;
}

/// The column lane @p i of a MIXED path leaves from: its outlet in @p through where
/// mixed_lanes_leave_outlets(), else its slot's column.
int32_t mixed_lane_column(const FilamentPathData& data, const BaseGeometry& g,
                          const RailArtRect& through, int i) {
    return mixed_lanes_leave_outlets(data, through) ? rail_outlet_x(*data.rail_art, through, i)
                                                    : g.slot_x[i];
}

/// compute_mixed_frame() with a pass-through unit's drawing ending @p art_cut higher under a
/// view scale.
MixedFrame mixed_frame_with(const FilamentPathData& data, const BaseGeometry& g, int32_t art_cut) {
    MixedFrame f;
    constexpr float kGlyphBottomY = 0.80f;
    constexpr float kSensorFraction = 0.36f;
    constexpr float kHubFraction = 0.59f;
    constexpr float kHubHFraction = 0.11f;
    // A pass-through unit's art ends no lower than this share of the run.
    constexpr float kMixedArtBottom = 0.5f;

    f.entry_y = g.y_off + (int32_t)(g.height * kEntryYRatio);
    int heads = 0;
    bool shared = false;
    for (int i = 0; i < LV_MIN(data.slot_count, FilamentPathData::kMaxSlots); i++) {
        if (data.slot_is_hub_routed[i])
            shared = true;
        else
            ++heads;
    }
    f.tool_scale = per_lane_tool_scale(data, g, heads + (shared ? 1 : 0));
    f.toolhead_y =
        toolhead_standing_on(per_lane_heads_bottom(data, g, kGlyphBottomY), f.tool_scale);
    const int32_t run = f.toolhead_y - f.entry_y;
    f.sensor_y = row_between(f.entry_y, f.toolhead_y, kSensorFraction);
    f.hub_h = LV_MAX(16, (int32_t)(run * kHubHFraction));
    if (data.hub_on_toolhead) {
        // Combiner on the print head: the box hugs the toolhead over a short
        // stub of shared tube, measured from the nozzle glyph's top so the two
        // never overlap.
        const int32_t nozzle_top = toolhead_glyph_bounds(0, f.toolhead_y, f.tool_scale).top;
        const int32_t stub = hub_stub_height(g);
        f.hub_bottom = nozzle_top - stub;
        f.hub_cy = f.hub_bottom - f.hub_h / 2;
    } else {
        f.hub_cy = row_between(f.entry_y, f.toolhead_y, kHubFraction);
        f.hub_bottom = f.hub_cy + f.hub_h / 2;
    }

    // The tubes hang from the spools' bottom edge, the round cap's top on it; the rows below
    // keep their places.
    if (const int32_t edge = box_edge_above(data, g, tube_cap(data), f.sensor_y); edge != INT32_MIN)
        f.entry_y = edge;

    f.states = compute_slot_render_states(&data);

    const int n = LV_MIN(data.slot_count, FilamentPathData::kMaxSlots);
    // A pass-through unit stands between the spools and the lane sensors. The sensors and the
    // hub move down below it, never so far that the hub reaches the shared head.
    if (n > 0) {
        const int32_t fan = art_fan_room(data, g);
        const int32_t stub = hub_stub_height(g);
        const int32_t nozzle_top = toolhead_glyph_bounds(0, f.toolhead_y, f.tool_scale).top;
        const int32_t sensor_to_hub = (f.hub_cy - f.hub_h / 2) - f.sensor_y;
        const int32_t lowest = nozzle_top - stub - f.hub_h - sensor_to_hub - fan;
        // The lanes converge from the whole row of spools onto the unit: twice a rail's room
        // (once under a view scale, whose rows are already tight to the unit).
        const int32_t top = f.entry_y + (data.view_hub_h > 0 ? fan : 2 * fan);
        // Under a view scale the drawing takes the room down to what the sensors, the hub and
        // the shared head need below it; the rows below it then follow it down.
        const int32_t scaled_lowest =
            nozzle_top - stub - f.hub_h - fan - 2 * data.theme.sensor_radius - art_cut;
        const int32_t bottom =
            data.view_hub_h > 0
                ? scaled_lowest
                : LV_MIN(lowest, row_between(f.entry_y, f.toolhead_y, kMixedArtBottom));
        f.through = through_art_rect(data, g, top, bottom, (g.slot_x[0] + g.slot_x[n - 1]) / 2);
        if (f.through.w > 0 && data.view_hub_h > 0 && !data.hub_on_toolhead) {
            f.sensor_y = f.through.y + f.through.h + fan / 2;
            // The hub stands just over its shared head, leaving the lanes from the outlets the
            // whole run above it to lean in.
            const int32_t hub_top =
                LV_MAX(f.sensor_y + fan / 2, nozzle_top - stub - f.hub_h - fan / 2);
            f.hub_cy = hub_top + f.hub_h / 2;
            f.hub_bottom = hub_top + f.hub_h;
        } else if (f.through.w > 0) {
            const int32_t shift = LV_MAX(0, f.through.y + f.through.h + fan - f.sensor_y);
            f.sensor_y += shift;
            f.hub_cy += shift;
            f.hub_bottom += shift;
        }
    }
    int32_t hub_x_sum = 0;
    for (int i = 0; i < n; i++) {
        if (data.slot_is_hub_routed[i]) {
            hub_x_sum += g.slot_x[i];
            f.hub_count++;
            if (f.first_hub_lane < 0)
                f.first_hub_lane = i;
        }
    }
    f.hub_cx = (f.hub_count > 0) ? (hub_x_sum / f.hub_count) : (g.x_off + 150);
    // The heads, each direct lane's and the shared one, stand in a row; where any two stand
    // closer than a head is wide they spread evenly a head's width apart, centered where they
    // were and kept inside the canvas.
    {
        struct Head {
            int32_t x;
            int lane; // -1: the shared head
        };
        Head heads[FilamentPathData::kMaxSlots + 1];
        int k = 0;
        const bool from_outlets = mixed_lanes_leave_outlets(data, f.through);
        for (int i = 0; i < n; i++) {
            f.head_x[i] = mixed_lane_column(data, g, f.through, i);
            if (!data.slot_is_hub_routed[i])
                heads[k++] = {f.head_x[i], i};
        }
        if (f.hub_count > 0) {
            // The shared head stands in the row where its lanes leave the unit, so the heads
            // keep the lanes' order and no tube crosses another.
            int32_t sum = 0;
            for (int i = 0; i < n; i++)
                sum += data.slot_is_hub_routed[i] ? f.head_x[i] : 0;
            f.hub_cx = from_outlets ? sum / f.hub_count : f.hub_cx;
            heads[k++] = {f.hub_cx, -1};
        }
        std::sort(heads, heads + k, [](const Head& l, const Head& r) { return l.x < r.x; });
        const GlyphBounds b = toolhead_glyph_bounds(0, 0, f.tool_scale);
        const int32_t w = b.right - b.left;
        const int32_t step = w + 4 * data.theme.sensor_radius;
        bool crowded = false;
        for (int j = 1; j < k; j++)
            crowded |= heads[j].x - heads[j - 1].x < step;
        if (crowded && k > 1) {
            const int32_t span = std::min<int32_t>((k - 1) * step, g.width - w);
            const int32_t each = span / (k - 1);
            int32_t left = (heads[0].x + heads[k - 1].x) / 2 - span / 2;
            left = std::clamp(left, g.x_off + w / 2,
                              std::max(g.x_off + w / 2, g.x_off + g.width - w / 2 - span));
            for (int j = 0; j < k; j++) {
                const int32_t x = left + j * each;
                if (heads[j].lane >= 0)
                    f.head_x[heads[j].lane] = x;
                else
                    f.hub_cx = x;
            }
        }
    }
    f.shared_head_x = f.hub_cx;
    // ~60% of the full hub topology width, enough for the hub lanes
    f.hub_w = LV_MAX(40, data.theme.hub_width * 3 / 5);
    // Under a view scale the hub's lanes converge on it as the hub layout's do: each diagonal no
    // flatter than kFanMinSlope (per_lane_required_height asks the height for it). Leaving a
    // pass-through unit's outlets they meet halfway over to the shared head, clear of the direct
    // lanes' outlets either side, and the trunk leans on down to the head; otherwise the hub drops
    // toward its head as far as the diagonals need.
    if (data.view_hub_h > 0 && !data.hub_on_toolhead && f.hub_count > 0) {
        const bool from_outlets = f.through.w > 0;
        if (from_outlets) {
            int32_t sum = 0, lo = INT32_MIN, hi = INT32_MAX;
            for (int i = 0; i < n; i++)
                sum += data.slot_is_hub_routed[i] ? f.head_x[i] : 0;
            const int32_t meet = sum / f.hub_count;
            const int32_t clear = 2 * data.theme.sensor_radius;
            for (int i = 0; i < n; i++) {
                if (data.slot_is_hub_routed[i])
                    continue;
                const int32_t out = rail_outlet_x(*data.rail_art, f.through, i);
                if (out < meet)
                    lo = std::max(lo, out + clear + f.hub_w / 2);
                else
                    hi = std::min(hi, out - clear - f.hub_w / 2);
            }
            // Halfway over to the shared head, so the lanes in and the trunk out share the lean.
            const int32_t mid = (meet + f.shared_head_x) / 2;
            f.hub_cx = lo <= hi ? std::clamp(mid, lo, hi) : mid;
        }
        const int32_t fan_top = std::min(mixed_hub_fan_top(data, f), mixed_hub_lowest_top(g, f));
        const int32_t hub_top = from_outlets ? fan_top : std::max(f.hub_cy - f.hub_h / 2, fan_top);
        f.hub_cy = hub_top + f.hub_h / 2;
        f.hub_bottom = hub_top + f.hub_h;
    }

    for (int i = 0; i < FilamentPathData::kMaxSlots; i++)
        f.slot_to_fan[i] = -1;
    const int32_t hub_top = f.hub_cy - f.hub_h / 2;
    pg::MergeLaneIn fan_in[FilamentPathData::kMaxSlots];
    int fan_n = 0;
    for (int i = 0; i < n; i++) {
        if (!data.slot_is_hub_routed[i])
            continue;
        const float col = (float)mixed_lane_column(data, g, f.through, i);
        fan_in[fan_n] = {col, (float)f.sensor_y};
        f.slot_to_fan[i] = fan_n++;
    }
    const float fillet = data.view_hub_h > 0 ? kScaledFanFillet : 8.0f;
    pg::build_merge_fan(fan_in, fan_n, (float)f.hub_cx, (float)hub_top, (float)f.hub_w,
                        /*entry_margin=*/8.0f, fillet, /*max_slope=*/1.2f, f.hub_fan);
    if (data.view_hub_h > 0)
        route_fan(fan_in, f.hub_fan, fan_n, lane_clearance(data));
    return f;
}
} // namespace

MixedFrame compute_mixed_frame(const FilamentPathData& data, const BaseGeometry& g) {
    MixedFrame f = mixed_frame_with(data, g, 0);
    // Under a view scale a pass-through unit's drawing gives up the height its lanes need to
    // reach the hub on steep enough diagonals, as far as it still draws; its outlets move as it
    // shrinks, so the need is measured again.
    int32_t cut = 0;
    for (int pass = 0; pass < 3; ++pass) {
        if (f.through.w <= 0 || data.view_hub_h <= 0 || data.hub_on_toolhead || f.hub_count <= 0)
            break;
        const int32_t over = mixed_hub_fan_top(data, f) - mixed_hub_lowest_top(g, f);
        if (over <= 0)
            break;
        cut += over;
        const MixedFrame shorter = mixed_frame_with(data, g, cut);
        if (shorter.through.w <= 0)
            break; // the drawing stays; the canvas asks for more height instead
        f = shorter;
    }
    return f;
}

// ============================================================================
// Routes, styles, bands
// ============================================================================

bool hub_is_bare(const FilamentPathData& data) {
    return data.topology == static_cast<int>(PathTopology::HUB) && !data.hub_on_toolhead &&
           data.slot_count == 1;
}

bool operator==(const SpanStyle& a, const SpanStyle& b) {
    return a.wall == b.wall && lv_color_eq(a.bore, b.bore) && a.filled == b.filled &&
           a.painted == b.painted;
}

bool route_append(Route& r, const pg::FilamentPath& piece, SpanStyle s) {
    for (int i = 0; i < piece.count; i++) {
        if (r.path.count == pg::FilamentPath::kMaxSegs) {
            r.dropped += piece.count - i;
            return false;
        }
        r.style[r.path.count] = s;
        r.path.segs[r.path.count++] = piece.segs[i];
    }
    return true;
}

int coalesce(const Route& r, Stroke* out, int max_out) {
    int n = 0;
    for (int i = 0; i < r.path.count;) {
        if (!r.style[i].painted) {
            i++;
            continue;
        }
        int j = i + 1;
        while (j < r.path.count && r.style[j] == r.style[i])
            j++;
        if (n < max_out)
            out[n++] = {i, j, r.style[i]};
        i = j;
    }
    return n;
}

pg::FilamentPath filled_prefix(const Route& r) {
    pg::FilamentPath p;
    int last = -1;
    for (int i = 0; i < r.path.count; i++)
        if (r.style[i].filled)
            last = i;
    for (int i = 0; i <= last; i++)
        p.segs[p.count++] = r.path.segs[i];
    return p;
}

SpanStyle span_style(PathSegment span, PathSegment reached, bool on_active_route,
                     PathSegment error_seg, lv_color_t filament, lv_color_t bg) {
    const bool error = span == error_seg;
    if (!is_segment_active(span, reached))
        return {error ? TubeWall::Error : TubeWall::Plain, bg, false, true};
    const TubeWall wall =
        error ? TubeWall::Error : (on_active_route ? TubeWall::Active : TubeWall::Plain);
    return {wall, filament, true, true};
}

BandState band_state(PathSegment sensor, PathSegment reached, bool on_active_route,
                     PathSegment error_seg) {
    if (sensor == error_seg)
        return BandState::Error;
    if (!is_segment_active(sensor, reached))
        return BandState::Empty;
    return on_active_route ? BandState::Active : BandState::Loaded;
}

void band_segment(const SensorBand& band, int32_t gauge, pg::PathPoint& p0, pg::PathPoint& p1) {
    const float h = (gauge + kBandExtra) / 2.0f - kBandThickness / 2.0f;
    const float nx = -band.tangent.y;
    const float ny = band.tangent.x;
    p0 = {band.at.x - nx * h, band.at.y - ny * h};
    p1 = {band.at.x + nx * h, band.at.y + ny * h};
}

lv_color_t band_color(const SensorBand& b, const TubePalette& pal) {
    switch (b.state) {
    case BandState::Active:
        return pal.accent;
    case BandState::Error:
        return pal.error;
    case BandState::Loaded:
    case BandState::Sync:
        return b.fill;
    case BandState::Empty:
        break;
    }
    return pal.idle_wall;
}

void draw_sensor_band(lv_layer_t* layer, const SensorBand& band, int32_t gauge, lv_color_t color) {
    pg::PathPoint p0, p1;
    band_segment(band, gauge, p0, p1);
    if (band.dot) {
        // A sensor drawn round, small enough to leave the art round it: a faint light halo keeps
        // it legible on a head of its own color.
        const int32_t r = LV_MAX(3, gauge / 2 + 1);
        const int32_t x = (int32_t)std::lround(band.at.x), y = (int32_t)std::lround(band.at.y);
        lv_draw_fill_dsc_t fill;
        lv_draw_fill_dsc_init(&fill);
        fill.radius = LV_RADIUS_CIRCLE;
        fill.color = lv_color_white();
        fill.opa = LV_OPA_30;
        const lv_area_t ring = {x - r - 1, y - r - 1, x + r + 1, y + r + 1};
        lv_draw_fill(layer, &fill, &ring);
        fill.color = color;
        fill.opa = LV_OPA_COVER;
        const lv_area_t dot = {x - r, y - r, x + r, y + r};
        lv_draw_fill(layer, &fill, &dot);
        return;
    }
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = band.thickness > 0 ? band.thickness : kBandThickness;
    dsc.opa = LV_OPA_COVER;
    dsc.p1.x = p0.x;
    dsc.p1.y = p0.y;
    dsc.p2.x = p1.x;
    dsc.p2.y = p1.y;
    dsc.round_start = true;
    dsc.round_end = true;
    lv_draw_line(layer, &dsc);
}

void append_line(Route& r, float x0, float y0, float x1, float y1, SpanStyle s) {
    pg::FilamentPath piece;
    piece.add_line(x0, y0, x1, y1);
    route_append(r, piece, s);
}

void append_bypass_feed(Route& r, float spool_x, float merge_y, float merge_x, SpanStyle s) {
    const float drop = (float)helix::ui::bypass_spool_bottom_above_merge();
    const pg::PathPoint pts[3] = {
        {spool_x, merge_y - drop}, {spool_x, merge_y}, {merge_x, merge_y}};
    pg::FilamentPath piece;
    pg::route_polyline_filleted(piece, pts, 3, drop);
    route_append(r, piece, s);
}

void add_band(PathPlan& plan, BandKind kind, pg::PathPoint at, pg::PathPoint tangent,
              BandState state, lv_color_t fill, bool on_box_edge) {
    const bool full = (kind == BandKind::Lane)
                          ? plan.band_count - plan.trunk_band_count >= kMaxBands - kTrunkBands
                          : plan.band_count >= kMaxBands;
    if (full) {
        plan.dropped++;
        return;
    }
    plan.trunk_band_count += kind == BandKind::Trunk;
    plan.bands[plan.band_count++] = {at, tangent, state, fill, on_box_edge};
}

// A band where the route currently ends, across its last segment.
void add_band_at_end(PathPlan& plan, BandKind kind, const Route& r, BandState state,
                     lv_color_t fill, bool on_box_edge) {
    if (r.path.count == 0)
        return;
    pg::PathPoint tangent;
    const pg::PathPoint at = pg::path_point_at(r.path, pg::path_length(r.path), &tangent);
    add_band(plan, kind, at, tangent, state, fill, on_box_edge);
}

Route& new_route(PathPlan& plan) {
    Route& r = plan.routes[plan.route_count++];
    r.path.clear();
    r.dropped = 0;
    return r;
}

SpanStyle unpainted(SpanStyle s) {
    s.painted = false;
    return s;
}

namespace {

// The hub's output sensor, on the hub box's bottom edge: inside a HUB's box, so the trunk below
// keeps no room for it; across a selector's outlet. With the buffer box given way, that outlet
// carries its sync sensor instead, banded in the buffer's colour while there is a reading to judge.

void add_hub_output_band(PathPlan& plan, const Lane& lane, const LinearHubFrame& f,
                         const FilamentPathData& data) {
    const bool sync = f.sync_on_output;
    if (!sync && (data.hub_on_toolhead || !data.has_hub_sensor))
        return;
    BandState state = lane.band(PathSegment::OUTPUT);
    lv_color_t fill = lane.color;
    if (sync && state != BandState::Error && data.buffer_fault_state >= 0) {
        state = BandState::Sync;
        fill = buffer_box_colors(data, false, fill).border;
    }
    const bool in_box = data.topology == static_cast<int>(PathTopology::HUB);
    // In the box: its bottom row is the box's last (f.output_y), the band's thickness above it.
    const float y =
        in_box ? (float)f.output_y - (kHubSensorThickness - 1) / 2.0f : (float)f.output_y;
    const int before = plan.band_count;
    add_band(plan, BandKind::Trunk, {(float)f.output_x, y}, {0, 1}, state, fill,
             /*on_box_edge=*/true);
    if (in_box && plan.band_count > before)
        plan.bands[plan.band_count - 1].thickness = kHubSensorThickness;
}

// Hub bottom → inlet, appended to the route that owns the trunk. Ends at the
// merge when an active bypass (@p bypass_owner) owns everything below it.
void append_trunk(PathPlan& plan, Route& r, const Lane& lane, const LinearHubFrame& f,
                  const FilamentPathData& data, bool bypass_on_trunk, const Lane* bypass_owner) {
    const float cx = (float)f.center_x;
    const float hub_bot = (float)f.output_y;
    add_hub_output_band(plan, lane, f, data);

    const float output_end = (float)(bypass_on_trunk ? f.bypass_merge_y : f.toolhead_y);
    // LINEAR: the exit jogs from under the active slot to the center above the buffer.
    const float jog_end = f.has_buffer ? (float)f.buf_fil_top : output_end;
    pg::FilamentPath piece;
    pg::route_orthogonal(piece, (float)f.output_x, hub_bot, cx, jog_end, kFilletRadius);
    if (jog_end < output_end)
        piece.add_line(cx, jog_end, cx, output_end);
    if (lane.on && lane.reached == PathSegment::OUTPUT && data.bowden_fill >= 0) {
        // Mid-load or mid-unload: filament fills the bowden only as far as the
        // backend's progress says.
        Lane short_of = lane;
        short_of.reached = PathSegment::HUB;
        pg::FilamentPath filled, empty;
        pg::split_path(piece, pg::path_length(piece) * (float)data.bowden_fill / 100.0f, filled,
                       empty);
        route_append(r, filled, lane.style(PathSegment::OUTPUT));
        route_append(r, empty, short_of.style(PathSegment::OUTPUT));
    } else {
        route_append(r, piece, lane.style(PathSegment::OUTPUT));
    }

    if (bypass_on_trunk) {
        if (bypass_owner) {
            add_band_at_end(plan, BandKind::Trunk, r, bypass_owner->band(PathSegment::OUTPUT),
                            bypass_owner->color);
            return;
        }
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::OUTPUT), lane.color);
        append_line(r, cx, output_end, cx, (float)f.toolhead_y, lane.style(PathSegment::TOOLHEAD));
    }
    // Under a view scale the toolhead sensor is part of the head: its mark stands at the head's
    // filament inlet, plain unless it reports a fault.
    const bool on_head = data.view_hub_h > 0;
    if (data.has_toolhead_sensor && !on_head)
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::TOOLHEAD), lane.color);
    append_line(r, cx, (float)f.toolhead_y, cx, (float)f.inlet_y, lane.style(PathSegment::NOZZLE));
    if (data.has_toolhead_sensor && on_head) {
        const int before = plan.band_count;
        add_band_at_end(plan, BandKind::Trunk, r, lane.band(PathSegment::TOOLHEAD), lane.color);
        if (plan.band_count > before)
            plan.bands[plan.band_count - 1].dot = true;
    }
}

// The load sensor sits midway down the lane's straight run below the prep row.
float load_band_y(const LinearHubFrame& f, float run_end) {
    return ((float)f.prep_y + run_end) / 2;
}

} // namespace

// ============================================================================
// Plan
// ============================================================================

void reset_plan(PathPlan& out) {
    out.route_count = 0;
    out.band_count = 0;
    out.trunk_band_count = 0;
    out.dropped = 0;
    out.active_route = -1;
    out.trunk_route = -1;
    out.bypass_route = -1;
    out.buffer_has_filament = false;
}

void total_dropped(PathPlan& out) {
    for (int i = 0; i < out.route_count; i++)
        out.dropped += out.routes[i].dropped;
}

PathSegment lane_error(const SlotRenderState& s, bool on_active_route, PathSegment system_error) {
    if (on_active_route)
        return system_error;
    if (!s.has_error)
        return PathSegment::NONE;
    return s.has_filament ? s.segment : PathSegment::SPOOL;
}

namespace {

// A lane's own state: how far its filament reached, mounted or not.
Lane lane_of(const SlotRenderState& s, PathSegment error, lv_color_t bg) {
    return {s.has_filament ? s.segment : PathSegment::NONE, s.is_mounted,
            lane_error(s, s.is_mounted, error), s.color, bg};
}

// Spool entry down to the lane's sensor, with its band where the unit has
// one. The sensor reads filament once it has reached the toolhead segment.
Route& start_lane(PathPlan& out, const Lane& lane, float x, int32_t entry_y, int32_t sensor_y,
                  bool has_sensor = true) {
    Route& r = new_route(out);
    append_line(r, x, (float)entry_y, x, (float)sensor_y, lane.style(PathSegment::SPOOL));
    if (has_sensor)
        add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::TOOLHEAD), lane.color);
    return r;
}

// The fillet of an S-bend between two columns @p dx apart: as round as half the offset allows.
float bend_radius(float dx) {
    return std::clamp(std::fabs(dx) / 2.0f, 0.5f, 18.0f);
}

// From (@p x, @p from_y) the lane bends in above head-on art to its gate's inlet. Returns the
// inlet; the lane goes on behind the art's face from there.
pg::PathPoint route_to_inlet(Route& r, const FilamentPathData& data, const RailArtRect& art,
                             int lane, float x, float from_y, SpanStyle s) {
    const float in_x = (float)rail_gate_x(*data.rail_art, art, lane);
    const float in_y = (float)rail_inlet_y(*data.rail_art, art);
    const float top = (float)art.y;
    const pg::PathPoint pts[4] = {
        {x, from_y}, {x, from_y + (top - from_y) / 3}, {in_x, top}, {in_x, in_y}};
    pg::FilamentPath fan;
    pg::route_polyline_filleted(fan, pts, 4, bend_radius(x - in_x));
    route_append(r, fan, s);
    return pts[3];
}

// Into a pass-through unit's art and hidden behind its face to the lane's own outlet. Returns
// the outlet.
pg::PathPoint route_into_art(Route& r, const FilamentPathData& data, const RailArtRect& art,
                             int lane, float x, float from_y, SpanStyle s) {
    const pg::PathPoint in = route_to_inlet(r, data, art, lane, x, from_y, s);
    const pg::PathPoint outlet{(float)rail_outlet_x(*data.rail_art, art, lane),
                               (float)rail_outlet_y(*data.rail_art, art)};
    append_line(r, in.x, in.y, outlet.x, outlet.y, unpainted(s));
    return outlet;
}

// Spool entry, through a pass-through unit's art, back out to the lane's own column at its
// sensor, with the sensor's band.
Route& start_lane_through_art(PathPlan& out, const Lane& lane, const FilamentPathData& data,
                              const RailArtRect& art, int i, float x, int32_t entry_y,
                              int32_t sensor_y) {
    Route& r = new_route(out);
    const SpanStyle s = lane.style(PathSegment::SPOOL);
    const pg::PathPoint at = route_into_art(r, data, art, i, x, (float)entry_y, s);
    const float run = ((float)sensor_y - at.y) / 3;
    const pg::PathPoint pts[4] = {
        at, {at.x, at.y + run}, {x, at.y + 2 * run}, {x, (float)sensor_y}};
    pg::FilamentPath back;
    pg::route_polyline_filleted(back, pts, 4, bend_radius(x - at.x));
    route_append(r, back, s);
    add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::TOOLHEAD), lane.color);
    return r;
}

} // namespace

void plan_parallel(const FilamentPathData& data, const BaseGeometry& g, PathPlan& out) {
    reset_plan(out);
    const ParallelRows rows = parallel_rows(data, g);
    const int32_t entry_y = rows.entry_y;
    const int32_t sensor_y = rows.sensor_y;
    const float nozzle_top = (float)path_toolhead_inlet(rows.toolhead_y, rows.tool_scale).y;
    const PathSegment error = static_cast<PathSegment>(data.error_segment);
    const SlotRenderStates states = compute_slot_render_states(&data);

    const int n = LV_MIN(data.slot_count, FilamentPathData::kMaxSlots);
    // Under a view scale the tube router lays the lanes out to their heads as one ribbon; where
    // it finds no room each lane takes its own down-diagonal-down.
    pg::MergeLaneOut routed[FilamentPathData::kMaxSlots];
    bool use_router = false;
    if (data.view_hub_h > 0 && n > 0) {
        pg::PathPoint src[FilamentPathData::kMaxSlots];
        for (int i = 0; i < n; i++) {
            const bool prep_row = !data.art_sensors && data.slot_has_prep_sensor[i];
            src[i] = {(float)g.slot_x[i], (float)(prep_row ? rows.prep_y : entry_y)};
            routed[i].pts[3] = {(float)parallel_head_x(data, g, rows, i), (float)sensor_y};
        }
        use_router = route_lanes(src, routed, n, lane_clearance(data));
    }
    for (int i = 0; i < n; i++) {
        const Lane lane = lane_of(states[i], error, data.theme.color_bg);
        const float x = (float)g.slot_x[i];
        const float hx = (float)parallel_head_x(data, g, rows, i);
        Route* r = &new_route(out);
        float top = (float)entry_y;
        if (data.art_sensors) {
            // Leaving a drawn unit, the tube starts where the unit's slanted edge crosses its
            // column, and the lane's one sensor mark lies along that edge, as on a hub's lanes.
            const float start = (float)(entry_y - std::max<int32_t>(0, data.lane_entry_rise[i]));
            append_line(*r, x, start, x, top, lane.style(PathSegment::SPOOL));
            const bool load = data.slot_has_load_sensor[i];
            if (load || (data.slot_has_prep_sensor[i] && !data.art_draws_prep)) {
                const float k = data.art_edge_slope;
                const float norm = std::sqrt(1.0f + k * k);
                const BandState state = data.lane_load_fault[i] ? BandState::Error
                                        : load                  ? lane.band(PathSegment::LANE)
                                                                : lane.band(PathSegment::PREP);
                add_band(out, BandKind::Lane, {x, start}, {-k / norm, 1.0f / norm}, state,
                         lane.color, false);
            }
        } else if (data.slot_has_prep_sensor[i]) {
            // Filament the entry sensor alone sees fills the tube to its band.
            append_line(*r, x, (float)entry_y, x, (float)rows.prep_y,
                        lane.style(PathSegment::SPOOL));
            add_band_at_end(out, BandKind::Lane, *r, lane.band(PathSegment::PREP), lane.color);
            top = (float)rows.prep_y;
        }
        const SpanStyle run_style =
            lane.style(data.slot_has_prep_sensor[i] && !data.art_sensors ? PathSegment::LANE
                                                                         : PathSegment::SPOOL);
        if (std::fabs(hx - x) < 0.5f) {
            append_line(*r, x, top, x, (float)sensor_y, run_style);
        } else if (use_router) {
            pg::FilamentPath bend;
            pg::route_polyline_filleted(bend, routed[i].pts, 4, kScaledFanFillet);
            route_append(*r, bend, run_style);
        } else {
            // Out to its own head, spread wider than the lanes: down, one diagonal, down, all
            // above the heads.
            const float run = (float)sensor_y - top;
            const pg::PathPoint pts[4] = {
                {x, top}, {x, top + run * 0.15f}, {hx, top + run * 0.85f}, {hx, (float)sensor_y}};
            pg::FilamentPath bend;
            pg::route_polyline_filleted(bend, pts, 4, bend_radius(hx - x));
            route_append(*r, bend, run_style);
        }
        append_line(*r, hx, (float)sensor_y, hx, nozzle_top, lane.style(PathSegment::NOZZLE));
        // Each head's own sensor is the round dot at its filament inlet, as on a hub's head:
        // the lane's color while it sees filament, red on a fault.
        if (data.has_toolhead_sensor) {
            const int before = out.band_count;
            add_band_at_end(out, BandKind::Lane, *r, lane.band(PathSegment::TOOLHEAD), lane.color);
            if (out.band_count > before)
                out.bands[out.band_count - 1].dot = true;
        }
        if (lane.on)
            out.active_route = i;
    }
    total_dropped(out);
}

void plan_mixed(const MixedFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                PathPlan& out) {
    reset_plan(out);
    const lv_color_t bg = data.theme.color_bg;
    const float nozzle_top = (float)path_toolhead_inlet(f.toolhead_y, f.tool_scale).y;
    const PathSegment error = static_cast<PathSegment>(data.error_segment);

    const int n = LV_MIN(data.slot_count, FilamentPathData::kMaxSlots);
    // Under a view scale the tube router lays the direct lanes out to their heads as one
    // ribbon; where it finds no room each takes its own down-diagonal-down.
    pg::MergeLaneOut routed[FilamentPathData::kMaxSlots];
    int route_of[FilamentPathData::kMaxSlots];
    bool use_router = false;
    if (data.view_hub_h > 0) {
        pg::PathPoint src[FilamentPathData::kMaxSlots];
        int m = 0;
        for (int i = 0; i < n; i++) {
            route_of[i] = -1;
            if (f.slot_to_fan[i] >= 0)
                continue;
            const float x = (float)mixed_lane_column(data, g, f.through, i);
            src[m] = {x, (float)f.sensor_y};
            routed[m].pts[3] = {(float)f.head_x[i], nozzle_top};
            route_of[i] = m++;
        }
        use_router = m > 0 && route_lanes(src, routed, m, lane_clearance(data));
    }
    for (int i = 0; i < n; i++) {
        const Lane lane = lane_of(f.states[i], error, bg);
        float x = (float)g.slot_x[i];
        Route* rp;
        if (mixed_lanes_leave_outlets(data, f.through)) {
            // Under a view scale the tube leaves the unit's drawing straight down from its own
            // outlet, with no jog back to its lane's column.
            rp = &new_route(out);
            const pg::PathPoint at = route_into_art(*rp, data, f.through, i, x, (float)f.entry_y,
                                                    lane.style(PathSegment::SPOOL));
            x = at.x;
            append_line(*rp, x, at.y, x, (float)f.sensor_y, lane.style(PathSegment::SPOOL));
            add_band_at_end(out, BandKind::Lane, *rp, lane.band(PathSegment::TOOLHEAD), lane.color);
        } else {
            rp = f.through.w > 0 ? &start_lane_through_art(out, lane, data, f.through, i, x,
                                                           f.entry_y, f.sensor_y)
                                 : &start_lane(out, lane, x, f.entry_y, f.sensor_y);
        }
        Route& r = *rp;
        const int fi = f.slot_to_fan[i];
        if (fi >= 0) {
            pg::FilamentPath fan;
            pg::route_polyline_filleted(fan, f.hub_fan[fi].pts, 4,
                                        data.view_hub_h > 0 ? kScaledFanFillet : 8.0f);
            route_append(r, fan, lane.style(PathSegment::SPOOL));
        } else if (const float hx = (float)f.head_x[i]; std::fabs(hx - x) < 0.5f) {
            append_line(r, x, (float)f.sensor_y, x, nozzle_top, lane.style(PathSegment::NOZZLE));
        } else if (use_router && route_of[i] >= 0) {
            pg::FilamentPath bend;
            pg::route_polyline_filleted(bend, routed[route_of[i]].pts, 4, kScaledFanFillet);
            route_append(r, bend, lane.style(PathSegment::NOZZLE));
        } else {
            // Out to its own head, spread from the others: down, one diagonal, down.
            const float run = nozzle_top - (float)f.sensor_y;
            const pg::PathPoint pts[4] = {{x, (float)f.sensor_y},
                                          {x, (float)f.sensor_y + run * 0.15f},
                                          {hx, (float)f.sensor_y + run * 0.7f},
                                          {hx, nozzle_top}};
            pg::FilamentPath bend;
            pg::route_polyline_filleted(bend, pts, 4, bend_radius(hx - x));
            route_append(r, bend, lane.style(PathSegment::NOZZLE));
        }
        if (lane.on)
            out.active_route = i;
    }

    if (f.hub_count > 0) {
        // The shared trunk shows the first hub lane that reached the nozzle.
        // With none there it is idle, and still shows an error unless a direct
        // lane is the mounted one.
        const bool direct_mounted = data.active_slot >= 0 && data.active_slot < n &&
                                    !data.slot_is_hub_routed[data.active_slot];
        Lane trunk{PathSegment::NONE, !direct_mounted, direct_mounted ? PathSegment::NONE : error,
                   f.states[f.first_hub_lane].color, bg};
        for (int j = 0; j < n; j++) {
            if (data.slot_is_hub_routed[j] && f.states[j].segment >= PathSegment::NOZZLE) {
                trunk = lane_of(f.states[j], error, bg);
                break;
            }
        }
        out.trunk_route = out.route_count;
        Route& r = new_route(out);
        const float hx = (float)f.hub_cx, tx = (float)f.shared_head_x;
        if (std::fabs(tx - hx) < 0.5f) {
            append_line(r, hx, (float)f.hub_bottom, hx, nozzle_top,
                        trunk.style(PathSegment::NOZZLE));
        } else {
            // From where the lanes meet, down and over to the shared head.
            const float run = nozzle_top - (float)f.hub_bottom;
            const pg::PathPoint pts[4] = {{hx, (float)f.hub_bottom},
                                          {hx, (float)f.hub_bottom + run * 0.15f},
                                          {tx, (float)f.hub_bottom + run * 0.7f},
                                          {tx, nozzle_top}};
            pg::FilamentPath bend;
            pg::route_polyline_filleted(bend, pts, 4, bend_radius(tx - hx));
            route_append(r, bend, trunk.style(PathSegment::NOZZLE));
        }
    }
    total_dropped(out);
}

void plan_linear_hub(const LinearHubFrame& f, const FilamentPathData& data, const BaseGeometry& g,
                     PathPlan& out) {
    reset_plan(out);

    const lv_color_t bg = data.theme.color_bg;
    const bool linear = data.topology == static_cast<int>(PathTopology::LINEAR);
    const bool on_head = data.hub_on_toolhead && !linear;
    const int n = LV_MIN(data.slot_count, FilamentPathData::kMaxSlots);
    const int active = (data.active_slot >= 0 && data.active_slot < n) ? data.active_slot : -1;
    // A hub that feeds no toolhead ends there: no trunk, buffer or bypass below it.
    const bool feeds = data.feeds_toolhead;
    const bool bypass = data.show_bypass && feeds;
    // On-toolhead the merge point sits above the head hub, off the trunk.
    const bool bypass_on_trunk = bypass && !on_head;
    const bool bare = hub_is_bare(data);
    const bool bypass_owns = bypass_on_trunk && data.bypass_active;
    // The bypass filament reached the nozzle; on its own route it takes the
    // same error rules as an AMS lane.
    const Lane bypass_lane{data.bypass_active ? PathSegment::NOZZLE : PathSegment::NONE,
                           data.bypass_active, data.bypass_active ? f.error_seg : PathSegment::NONE,
                           lv_color_hex(data.bypass_color), data.theme.color_bg};
    const Lane* bypass_owner = bypass_owns ? &bypass_lane : nullptr;
    // Box edges in whole pixels, as the boxes are drawn.
    const int32_t hub_top_px = f.hub_y - f.hub_h / 2;
    const float hub_top = (float)hub_top_px;
    const float hub_bot = (float)f.output_y;
    const float cx = (float)f.center_x;

    out.buffer_has_filament =
        active >= 0 && !data.bypass_active && is_segment_active(PathSegment::OUTPUT, f.fil_seg);
    out.buffer_fill = f.active_color;

    for (int i = 0; i < n; i++) {
        const SlotRenderState& s = f.states[i];
        const Lane lane{s.has_filament ? s.segment : PathSegment::NONE, i == active,
                        lane_error(s, i == active, f.error_seg), s.color, bg};
        const float x = (float)g.slot_x[i];
        Route& r = new_route(out);

        const bool straight = f.rail.w == 0 && f.through.w == 0;
        const float start_y = (float)lane_start_y(data, f, i);
        const float band_y = (float)lane_band_y(data, f, i);
        append_line(r, x, start_y, x, band_y, lane.style(PathSegment::SPOOL));
        // Leaving a drawn unit, a lane shows one sensor mark: a short marker where its tube
        // leaves the unit, lying along the unit's bottom edge as drawn. It stands for the load
        // sensor, or for the prep sensor where the lane has no load sensor and the art draws
        // no prep region; the lane's color while it sees filament, red on a fault, plain
        // while empty. A lane with neither sensor has no mark. Without a drawing the prep
        // sensor is a band across the tube.
        const BandState prep = lane.band(PathSegment::PREP);
        if (data.art_sensors) {
            const bool load = data.slot_has_load_sensor[i];
            const bool prep_mark = !load && data.slot_has_prep_sensor[i] && !data.art_draws_prep;
            if (load || prep_mark) {
                const float k = data.art_edge_slope;
                const float norm = std::sqrt(1.0f + k * k);
                // A prep fault the art cannot show reddens the one mark too.
                const bool prep_fault = prep == BandState::Error && !data.art_draws_prep;
                const BandState state = data.lane_load_fault[i] || prep_fault ? BandState::Error
                                        : load ? lane.band(PathSegment::LANE)
                                               : prep;
                add_band(out, BandKind::Lane, {x, start_y}, {-k / norm, 1.0f / norm}, state,
                         s.color, false);
            }
        } else if (data.slot_has_prep_sensor[i]) {
            add_band(out, BandKind::Lane, {x, band_y}, {0, 1}, prep, s.color,
                     f.prep_on_box_edge && band_y == start_y);
        }
        if (straight)
            append_line(r, x, band_y, x, (float)f.prep_y, lane.style(PathSegment::LANE));

        if (linear && f.rail.w > 0) {
            // Each lane bends in above the art to its gate and goes in behind the art's face.
            const pg::PathPoint in = route_to_inlet(r, data, f.rail, i, x, (float)f.prep_band_y,
                                                    lane.style(PathSegment::LANE));
            if (!lane.on)
                continue;
            append_line(r, in.x, in.y, (float)f.output_x, (float)f.output_y,
                        unpainted(lane.style(PathSegment::HUB)));
        } else if (f.through.w > 0) {
            // Into the unit's art and out of its own outlet, then down the fan to the hub.
            route_into_art(r, data, f.through, i, x, (float)f.prep_band_y,
                           lane.style(PathSegment::LANE));
            pg::FilamentPath fan;
            pg::route_polyline_filleted(fan, f.hub_fan[i].pts, 4,
                                        data.view_hub_h > 0 ? kScaledFanFillet : 8.0f);
            route_append(r, fan, lane.style(PathSegment::LANE));
            if (bare)
                append_line(r, f.hub_fan[i].pts[3].x, hub_top, cx, hub_bot,
                            lane.style(PathSegment::HUB));
            if (!lane.on)
                continue;
            if (!bare)
                append_line(r, f.hub_fan[i].pts[3].x, hub_top, cx, hub_bot,
                            unpainted(lane.style(PathSegment::HUB)));
        } else if (linear) {
            append_line(r, x, (float)f.prep_y, x, hub_top, lane.style(PathSegment::LANE));
            if (!lane.on)
                continue;
            // Hidden through the selector, as through a hub: drawn over it, the tube would cross
            // the bar's label.
            append_line(r, x, hub_top, (float)f.output_x, hub_bot,
                        unpainted(lane.style(PathSegment::HUB)));
        } else {
            pg::PathPoint pts[4] = {f.hub_fan[i].pts[0], f.hub_fan[i].pts[1], f.hub_fan[i].pts[2],
                                    f.hub_fan[i].pts[3]};
            // The lane's load sensor sits midway down its straight run below
            // the prep sensor (to the selector, or to the fan's first bend).
            const bool load_band = data.slot_has_load_sensor[i] && !data.art_sensors;
            if (on_head && data.art_sensors) {
                // The unit's drawing shows its own selector: each tube runs unbroken from its
                // outlet down to the combiner on the head.
                append_line(r, x, (float)f.prep_y, x, pts[0].y, lane.style(PathSegment::LANE));
            } else if (on_head) {
                const int32_t sel_top_px = f.selector_y - f.hub_h / 2;
                const float sel_top = (float)sel_top_px;
                if (load_band) {
                    const float load_y = load_band_y(f, sel_top);
                    append_line(r, x, (float)f.prep_y, x, load_y, lane.style(PathSegment::LANE));
                    add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::LANE), s.color);
                    append_line(r, x, load_y, x, sel_top, lane.style(PathSegment::LANE));
                } else {
                    append_line(r, x, (float)f.prep_y, x, sel_top, lane.style(PathSegment::LANE));
                }
                append_line(r, x, sel_top, x, pts[0].y, unpainted(lane.style(PathSegment::LANE)));
            } else {
                pts[0].y = (float)f.prep_y;
                if (load_band) {
                    const float load_y = load_band_y(f, pts[1].y);
                    append_line(r, x, pts[0].y, x, load_y, lane.style(PathSegment::LANE));
                    add_band_at_end(out, BandKind::Lane, r, lane.band(PathSegment::LANE), s.color);
                    pts[0].y = load_y;
                }
            }
            pg::FilamentPath fan;
            pg::route_polyline_filleted(fan, pts, 4, data.view_hub_h > 0 ? kScaledFanFillet : 8.0f);
            route_append(r, fan, lane.style(PathSegment::LANE));
            // A lone lane's tube runs on, drawn, where a hub box would stand.
            const auto through = lane.style(PathSegment::HUB);
            if (bare)
                append_line(r, pts[3].x, hub_top, cx, hub_bot, through);
            if (!lane.on)
                continue;
            if (!bare)
                append_line(r, pts[3].x, hub_top, cx, hub_bot, unpainted(through));
        }

        out.active_route = i;
        if (feeds)
            append_trunk(out, r, lane, f, data, bypass_on_trunk, bypass_owner);
        else
            add_hub_output_band(out, lane, f, data);
    }

    if (active < 0) {
        // Nothing loaded owns the trunk. It still shows an error at its
        // sensors, and filament the hub sensor reads with no lane to own it
        // (stuck after a failed unload) fills it as far as the hub output.
        const bool parked = data.has_hub_sensor && data.hub_sensor_triggered;
        const Lane idle{parked ? PathSegment::OUTPUT : PathSegment::NONE, false, f.error_seg,
                        f.idle_color, bg};
        if (feeds) {
            out.trunk_route = out.route_count;
            Route& r = new_route(out);
            append_trunk(out, r, idle, f, data, bypass_on_trunk, bypass_owner);
        } else {
            add_hub_output_band(out, idle, f, data);
        }
    }

    if (bypass) {
        const float merge_y = (float)f.bypass_merge_y;
        // The spool widget stands over the merge line at kBypassXRatio.
        const float spool_x = (float)(g.x_off + (int32_t)(g.width * kBypassXRatio));
        out.bypass_route = out.route_count;
        Route& b = new_route(out);
        // Spool → merge carries the SPOOL tag: no AMS error lands on it.
        append_bypass_feed(b, spool_x, merge_y, cx, bypass_lane.style(PathSegment::SPOOL));
        if (bypass_owns) {
            append_line(b, cx, merge_y, cx, (float)f.toolhead_y,
                        bypass_lane.style(PathSegment::TOOLHEAD));
            if (data.has_toolhead_sensor)
                add_band_at_end(out, BandKind::Trunk, b, bypass_lane.band(PathSegment::TOOLHEAD),
                                bypass_lane.color);
            append_line(b, cx, (float)f.toolhead_y, cx, (float)f.inlet_y,
                        bypass_lane.style(PathSegment::NOZZLE));
        }
    }

    total_dropped(out);
}

// ============================================================================
// Paint
// ============================================================================

namespace {

LaneStyle lane_style_for(const SpanStyle& s, const TubePalette& pal) {
    const lv_color_t wall = s.wall == TubeWall::Active  ? pal.accent
                            : s.wall == TubeWall::Error ? pal.error
                                                        : pal.idle_wall;
    return {wall, s.bore, pal.bg, pal.gauge, s.wall == TubeWall::Active};
}

void stroke_layer(lv_layer_t* layer, const Route& r, const Stroke& st, const TubePalette& pal,
                  TubeLayer which, bool simple) {
    TubePass passes[2];
    const int n = build_passes(lane_style_for(st.style, pal), which, passes, simple);
    if (n == 0)
        return;
    pg::FilamentPath sub;
    for (int i = st.first; i < st.end; i++)
        sub.segs[sub.count++] = r.path.segs[i];
    stroke_path(layer, sub, passes, n);
}

void paint_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal,
                 bool on_box_edge) {
    for (int i = 0; i < plan.band_count; i++) {
        const SensorBand& b = plan.bands[i];
        if (b.on_box_edge == on_box_edge)
            draw_sensor_band(layer, b, pal.gauge, band_color(b, pal));
    }
}

} // namespace

void paint_tubes(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal, bool simple) {
    auto each = [&](TubeLayer which, auto&& want) {
        Stroke strokes[pg::FilamentPath::kMaxSegs];
        for (int ri = 0; ri < plan.route_count; ri++) {
            const int n = coalesce(plan.routes[ri], strokes, pg::FilamentPath::kMaxSegs);
            for (int k = 0; k < n; k++)
                if (want(ri, strokes[k]))
                    stroke_layer(layer, plan.routes[ri], strokes[k], pal, which, simple);
        }
    };

    each(TubeLayer::Halo, [](int, const Stroke& s) { return s.style.wall == TubeWall::Active; });
    each(TubeLayer::Wall, [](int, const Stroke&) { return true; });
    // The active route's bore goes last so at a T its fill wins over a neighbour's cap.
    each(TubeLayer::Bore, [](int, const Stroke& s) { return !s.style.filled; });
    each(TubeLayer::Bore,
         [&](int ri, const Stroke& s) { return s.style.filled && ri != plan.active_route; });
    each(TubeLayer::Bore,
         [&](int ri, const Stroke& s) { return s.style.filled && ri == plan.active_route; });

    paint_bands(layer, plan, pal, /*on_box_edge=*/false);
}

void paint_box_bands(lv_layer_t* layer, const PathPlan& plan, const TubePalette& pal) {
    paint_bands(layer, plan, pal, /*on_box_edge=*/true);
}

} // namespace helix::ui::fpath
