// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_breakpoint.h"

#include "core/subject.h"
#include "display_metrics.h"
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
/// tests/unit/test_registry_span_bands.cpp pins the result - one band per
/// authored span, identical across all eight shipping geometries.
///
/// The high-DPI UI scale factor is the second axis the same reasoning runs
/// along, and the bands carry it for the same reason - see detail::scaled_band
/// below.
///
/// Span and physical width are still not monotonic across tiers (a portrait
/// panel's single column is wider than a small landscape panel's two), so
/// these thresholds cannot reproduce span behavior exactly. Where the two
/// disagree, physical size decides.

/// font_body px per tier, the ladder the bands are scaled against. Mirrors the
/// font_body_<tier> tokens in ui_xml/globals.xml.
inline constexpr int kFontBodyPx[] = {12, 12, 14, 18, 20, 24, 32};
static_assert(std::size(kFontBodyPx) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "FONT_BODY_PX must carry one font size per UiBreakpoint tier");

/// The tier the four base values were measured on. Its ladder entry is the
/// divisor, so the Small rung of every band is the measured value unchanged.
inline constexpr UiBreakpoint kBaseTier = UiBreakpoint::Small;
inline constexpr int kBaseFontBodyPx = kFontBodyPx[static_cast<size_t>(to_int(kBaseTier))];

/// One base value spread across the tiers, rounded to whole pixels.
constexpr std::array<int, std::size(kFontBodyPx)> band_ladder(int base_px) {
    std::array<int, std::size(kFontBodyPx)> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = (base_px * kFontBodyPx[i] + kBaseFontBodyPx / 2) / kBaseFontBodyPx;
    }
    return out;
}

/// Smallest measured 2-column width (Small span2). -> 116 116 135 174 193 231 309
inline constexpr auto kWNormalPx = band_ladder(135);
/// Smallest measured 3-column width (Small span3). -> 176 176 205 264 293 351 469
inline constexpr auto kWWidePx = band_ladder(205);
/// Smallest measured 2-row height (Micro row2). -> 112 112 131 168 187 225 299
inline constexpr auto kHTallPx = band_ladder(131);
/// Smallest measured 3-row height (Micro row3, 197.5 truncated to 197 at
/// runtime - already the `>=` value). -> 169 169 197 253 281 338 450
inline constexpr auto kHTallerPx = band_ladder(197);

static_assert(std::size(kWNormalPx) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "W_NORMAL_PX must carry one width per UiBreakpoint tier");
static_assert(std::size(kWWidePx) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "W_WIDE_PX must carry one width per UiBreakpoint tier");
static_assert(std::size(kHTallPx) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "H_TALL_PX must carry one height per UiBreakpoint tier");
static_assert(std::size(kHTallerPx) == static_cast<size_t>(to_int(UiBreakpoint::XXLarge)) + 1,
              "H_TALLER_PX must carry one height per UiBreakpoint tier");

// The Small rung is the measured value itself, so a change to the rounding
// cannot silently move the tier the numbers were calibrated on.
static_assert(kWNormalPx[static_cast<size_t>(to_int(kBaseTier))] == 135);
static_assert(kWWidePx[static_cast<size_t>(to_int(kBaseTier))] == 205);
static_assert(kHTallPx[static_cast<size_t>(to_int(kBaseTier))] == 131);
static_assert(kHTallerPx[static_cast<size_t>(to_int(kBaseTier))] == 197);

namespace detail {
/// Clamped table read. A tier out of range would otherwise index off the end
/// of a seven-entry table with whatever int a caller happened to hold.
constexpr int band_at(const std::array<int, std::size(kFontBodyPx)>& table, UiBreakpoint bp) {
    int32_t i = to_int(bp);
    if (i < to_int(UiBreakpoint::Micro)) {
        i = to_int(UiBreakpoint::Micro);
    }
    if (i > to_int(UiBreakpoint::XXLarge)) {
        i = to_int(UiBreakpoint::XXLarge);
    }
    return table[static_cast<size_t>(i)];
}

/// A band at the UI scale now in force.
///
/// The scale grows both sides of the question a band asks.
/// GridLayout::get_dimensions() puts the authored track edge through
/// DisplayMetrics::scaled_px(), so a widget's cell gets wider; theme_manager
/// scales the type ladder, so the text that has to fit inside it gets taller.
/// A band left on its authored pixels would be answering about a panel that is
/// no longer on screen: on 1080x2400 at 405 DPI the home grid quantises to 6
/// columns instead of 12, handing the two-cell fan stack 337px where the
/// unscaled panel handed it 168px, and 337 clears an unscaled 309px XXLarge
/// band that was calibrated against type 1.578x shorter than what now renders.
///
/// Running the band through the same scaled_px() the cell went through
/// preserves the cell-to-band ratio, so an authored span lands in the same band
/// at every scale — the scale-axis counterpart of what the font_body ladder
/// does across tiers. active_scale() is 1.0 on every shipping printer (all sit
/// inside the DPI deadband), which makes this an exact no-op there.
inline int scaled_band(const std::array<int, std::size(kFontBodyPx)>& table, UiBreakpoint bp) {
    return DisplayMetrics::scaled_px(band_at(table, bp), DisplayMetrics::active_scale());
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
    if (helix::subject_is_live(subject)) {
        return as_breakpoint(lv_subject_get_int(subject));
    }
    return breakpoint_for(responsive_dimension(nullptr));
}

/// Band for an explicit tier. Used by anything reasoning about a panel other
/// than the one it is running on - the shipping-geometry sweeps in the tests -
/// and by the widgets whose size predicate takes the tier as a parameter
/// (TempGraphWidget, PrintStatsWidget). The UI scale is process-wide rather
/// than per-tier, so these carry it too: a caller naming a tier is choosing
/// which type ladder to measure against, not opting out of the panel's scale.
inline int w_normal(UiBreakpoint bp) {
    return detail::scaled_band(kWNormalPx, bp);
}
inline int w_wide(UiBreakpoint bp) {
    return detail::scaled_band(kWWidePx, bp);
}
inline int h_tall(UiBreakpoint bp) {
    return detail::scaled_band(kHTallPx, bp);
}
inline int h_taller(UiBreakpoint bp) {
    return detail::scaled_band(kHTallerPx, bp);
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
