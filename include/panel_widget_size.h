// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_breakpoint.h"

#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <array>
#include <cstddef>
#include <iterator>

namespace helix::widget_size {

/// Physical size bands for home widget layout decisions.
///
/// A widget picks its layout from the pixels it occupies rather than from a
/// grid span, so one authored span reads correctly on every panel and
/// orientation. Every predicate that uses these bands compares with `>=`, so
/// each band must equal the smallest extent at which the span predicate it
/// replaces fires on any shipping panel, not one pixel below it - measured
/// live at the Small tier. (`grid_track_extent()` truncates its float result
/// via `static_cast<int>`, so "smallest measured" here is already the
/// truncated integer pixel value a widget actually receives, not the raw float
/// cell arithmetic.)
///
/// The bands scale with the type ladder rather than staying flat. A band is
/// really asking "is there room for the roomy layout", and what has to fit in
/// that room is text: font_body runs 12/12/14/18/20/24/32px across
/// micro..xxlarge, so a box that reads as roomy at Small is cramped at XXLarge
/// where every glyph is 2.3x taller. A flat band promoted a one-cell XXLarge
/// widget (about 182px wide) to the layout calibrated for 32px-shorter text,
/// and the text overflowed - FanStackWidget clipped "Part Cooling Fan" where
/// it should have drawn "P", ams_mini_status wrapped "PLA" to a glyph per
/// line.
///
/// Scaling by font_body(tier) / font_body(Small) is what makes an authored
/// span mean one thing everywhere. GridLayout::GRID_CELL grows on roughly the
/// same curve as font_body, so a widget of a given cell span occupies about
/// the same 11-14 em of width on every shipping panel; measuring that against
/// a band that also moves in em puts every panel in the same band. Flat bands
/// did not: the same two-cell widget read "wide" on medium/large/xlarge and
/// "normal" on tiny/small, because those tiers' pixels buy fewer glyphs.
///
/// Span and physical width are still not monotonic across tiers (a portrait
/// panel's single column is wider than a small landscape panel's two), so
/// these thresholds cannot reproduce span behavior exactly. Where the two
/// disagree, physical size decides.

/// font_body px per tier, the ladder the bands are scaled against. Mirrors the
/// font_body_<tier> tokens in ui_xml/globals.xml.
inline constexpr int FONT_BODY_PX[] = {12, 12, 14, 18, 20, 24, 32};
static_assert(std::size(FONT_BODY_PX) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "FONT_BODY_PX must carry one font size per UiBreakpoint tier");

/// The tier the four base values were measured on. Its ladder entry is the
/// divisor, so the Small rung of every band is the measured value unchanged.
inline constexpr UiBreakpoint BASE_TIER = UiBreakpoint::Small;
inline constexpr int BASE_FONT_BODY_PX = FONT_BODY_PX[static_cast<size_t>(to_int(BASE_TIER))];

/// One base value spread across the tiers, rounded to whole pixels.
constexpr std::array<int, std::size(FONT_BODY_PX)> band_ladder(int base_px) {
    std::array<int, std::size(FONT_BODY_PX)> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = (base_px * FONT_BODY_PX[i] + BASE_FONT_BODY_PX / 2) / BASE_FONT_BODY_PX;
    }
    return out;
}

/// Smallest measured 2-column width (Small span2). -> 116 116 135 174 193 231 309
inline constexpr auto W_NORMAL_PX = band_ladder(135);
/// Smallest measured 3-column width (Small span3). -> 176 176 205 264 293 351 469
inline constexpr auto W_WIDE_PX = band_ladder(205);
/// Smallest measured 2-row height (Micro row2). -> 112 112 131 168 187 225 299
inline constexpr auto H_TALL_PX = band_ladder(131);
/// Smallest measured 3-row height (Micro row3, 197.5 truncated to 197 at
/// runtime - already the `>=` value). -> 169 169 197 253 281 338 450
inline constexpr auto H_TALLER_PX = band_ladder(197);

static_assert(std::size(W_NORMAL_PX) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "W_NORMAL_PX must carry one width per UiBreakpoint tier");
static_assert(std::size(W_WIDE_PX) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "W_WIDE_PX must carry one width per UiBreakpoint tier");
static_assert(std::size(H_TALL_PX) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "H_TALL_PX must carry one height per UiBreakpoint tier");
static_assert(std::size(H_TALLER_PX) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "H_TALLER_PX must carry one height per UiBreakpoint tier");

// The Small rung is the measured value itself, so a change to the rounding
// cannot silently move the tier the numbers were calibrated on.
static_assert(W_NORMAL_PX[static_cast<size_t>(to_int(BASE_TIER))] == 135);
static_assert(W_WIDE_PX[static_cast<size_t>(to_int(BASE_TIER))] == 205);
static_assert(H_TALL_PX[static_cast<size_t>(to_int(BASE_TIER))] == 131);
static_assert(H_TALLER_PX[static_cast<size_t>(to_int(BASE_TIER))] == 197);

/// Legacy flat bands - the Small rung of each ladder above, frozen.
///
/// These are what every call site used before the ladder existed. Ported call
/// sites read the tier-aware `w_normal()`-style accessors below; the rest
/// (camera, humidity, tips, print_stats, job_queue, tool_switcher,
/// width_sensor, favorite_macro, print_status, temp_graph, temp_stack, clock,
/// and their tests) are still pending and keep pointing here. Do not add new
/// uses.
inline constexpr int W_NORMAL = 135; ///< was colspan >= 2 (smallest measured: 135, Small span2)
inline constexpr int W_WIDE = 205;   ///< was colspan >= 3 (smallest measured: 205, Small span3)
inline constexpr int H_TALL = 131;   ///< was rowspan >= 2 (smallest measured: 131, Micro row2)
inline constexpr int H_TALLER = 197; ///< was rowspan >= 3 (smallest measured: 197.5, truncates to
                                     ///< 197 at runtime, Micro row3 — already the `>=` value)

namespace detail {
/// Clamped table read. A tier out of range would otherwise index off the end
/// of a seven-entry table with whatever int a caller happened to hold.
constexpr int band_at(const std::array<int, std::size(FONT_BODY_PX)>& table, UiBreakpoint bp) {
    int32_t i = to_int(bp);
    if (i < to_int(UiBreakpoint::Micro)) {
        i = to_int(UiBreakpoint::Micro);
    }
    if (i > to_int(UiBreakpoint::XXLarge)) {
        i = to_int(UiBreakpoint::XXLarge);
    }
    return table[static_cast<size_t>(i)];
}
} // namespace detail

/// The tier a widget is being laid out on.
///
/// The ui_breakpoint subject is the runtime authority - it is what the XML
/// bindings and every other tier-aware lookup read, and theme_manager keeps it
/// in step with the display. Before the theme is initialized the subject holds
/// no int, so the display is classified directly, the same way theme_manager
/// itself computes the subject's value.
inline UiBreakpoint current_breakpoint() {
    lv_subject_t* subject = theme_manager_get_breakpoint_subject();
    if (subject != nullptr && subject->type == LV_SUBJECT_TYPE_INT) {
        return as_breakpoint(lv_subject_get_int(subject));
    }
    return breakpoint_for(responsive_dimension(nullptr));
}

/// Band for an explicit tier. Used by anything reasoning about a panel other
/// than the one it is running on - the shipping-geometry sweeps in the tests -
/// and by the widgets whose size predicate takes the tier as a parameter
/// (TempGraphWidget, PrintStatsWidget).
inline int w_normal(UiBreakpoint bp) {
    return detail::band_at(W_NORMAL_PX, bp);
}
inline int w_wide(UiBreakpoint bp) {
    return detail::band_at(W_WIDE_PX, bp);
}
inline int h_tall(UiBreakpoint bp) {
    return detail::band_at(H_TALL_PX, bp);
}
inline int h_taller(UiBreakpoint bp) {
    return detail::band_at(H_TALLER_PX, bp);
}

/// Band for the tier now on screen. What a widget's on_size_changed() wants:
/// the measured pixel extent it was handed is already in this tier's terms.
inline int w_normal() {
    return w_normal(current_breakpoint());
}
inline int w_wide() {
    return w_wide(current_breakpoint());
}
inline int h_tall() {
    return h_tall(current_breakpoint());
}
inline int h_taller() {
    return h_taller(current_breakpoint());
}

/// Height is still not a proxy for width. A predicate that reads "tall" from
/// height_px alone is fine for a layout that only scales type or icon size in
/// place, and wrong for one that would also lay out new content across the
/// width on the strength of height_px - a legend beside a chart, a second
/// column, a longer resolved label next to an icon. A widget with that shape
/// needs its own width-bearing predicate (`width_px >= w_normal()`, plain and
/// untransposable), not one that infers width readiness from height. What the
/// per-tier ladder does fix is the older, cruder version of the same trap: a
/// flat 131px band made Large's and XLarge's 141px and 169px single grid row
/// read as two rows, so a plain 1x1 widget was promoted on exactly those two
/// tiers and nowhere else.

} // namespace helix::widget_size
