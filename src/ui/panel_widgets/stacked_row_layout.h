// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

// Pure layout-decision logic shared by the home widgets that stack labelled
// rows: Temperatures (temp_stack) and Fan Speeds (fan_stack).
//
// Deliberately free of LVGL / widget state so it can be unit-tested without a
// display or font subsystem, the same shape as nozzle_layout.h. A widget
// measures its fonts and pixel height in on_size_changed() and feeds them
// here; this header decides whether each row draws as [icon | value] on one
// line or as an icon centred above its value.

namespace helix {

/// The rendered height of one row in the stacked (icon-above) layout.
struct StackedRowMetrics {
    int icon_px = 0;     ///< icon glyph line height
    int text_px = 0;     ///< value text line height
    int icon_gap_px = 0; ///< gap between the icon and its own value
    int label_px = 0;    ///< caption line height under the value; 0 = the row has no caption
};

/// Smallest authored rowspan, in half-cell grid tracks, that may stack.
///
/// The home grid lays out GridLayout::TRACKS_PER_CELL (2) tracks per cell and
/// both widgets set supports_half_row, so an authored height runs 2, 3 or 4
/// tracks — one cell, one and a half, or two. Stacking starts above one whole
/// cell. A static_assert at each call site ties this to the real
/// TRACKS_PER_CELL so a change to the grid cannot leave it stale.
inline constexpr int kIconAboveMinRowspan = 3;

/// Decide whether the icon-above-temperature layout fits.
///
/// @param rowspan     authored grid tracks (half-cells) of height
/// @param avail_px    measured pixel height the widget was allocated
/// @param row_count   rows actually visible (chamber / aux fan may be hidden)
/// @param row         per-heater metrics in the stacked layout
/// @param row_gap_px  gap between two heaters
///
/// Both halves of the rule are load-bearing. The rowspan gate is what the
/// authored size means — a widget the user left at one cell keeps the compact
/// side-by-side rows however many pixels that cell is worth on a large panel.
/// The pixel test is the guard on the other side: a track is only ~34px on the
/// smallest panels, so three tracks there is not room for two lines per row
/// even though the span says "taller than a cell". Where the two disagree the
/// compact layout wins, which is also what panel_widget_size.h asks for.
///
/// A degenerate call — no pixels yet, nothing visible, or fonts that have not
/// resolved — returns false, the layout that fits in strictly less room.
[[nodiscard]] inline bool wants_icon_above(int rowspan, int avail_px, int row_count,
                                           const StackedRowMetrics& row, int row_gap_px) {
    if (rowspan < kIconAboveMinRowspan)
        return false;
    if (avail_px <= 0 || row_count <= 0)
        return false;
    if (row.icon_px <= 0 || row.text_px <= 0)
        return false;

    const int per_row = row.icon_px + row.icon_gap_px + row.text_px;
    const int needed = row_count * per_row + (row_count - 1) * row_gap_px;
    return avail_px >= needed;
}

/// Smallest authored colspan, in half-cell tracks, that may lay the rows out
/// side by side. Mirrors kIconAboveMinRowspan on the other axis: above one
/// whole cell, so a 1.5-cell and wider box qualifies and a one-cell box never
/// does however many pixels a cell is worth on a big panel.
inline constexpr int kColumnsMinColspan = 3;

/// How the visible rows of a stacked widget are arranged in its box.
enum class StackShape {
    Beside,    ///< rows stacked vertically, each [icon | value]
    IconAbove, ///< rows stacked vertically, each icon over its value
    Columns,   ///< rows side by side, each icon over its value (and caption)
    Rows,      ///< rows stacked vertically, each a large icon with its value beside it
};

struct StackLayout {
    StackShape shape = StackShape::Beside;
    bool captions = false; ///< Columns only: the caption line under each value fits
};

/// Measured width one column needs, with and without its caption.
struct ColumnWidths {
    int plain_px = 0;        ///< widest column content with no caption
    int captioned_px = 0;    ///< widest column content with its caption
    int min_box_px = 0;      ///< narrowest box the layout may take at all (widget_size::w_normal)
    int beside_plain_px = 0; ///< Rows: one row's icon and value, side by side (0 = not offered)
    int beside_captioned_px = 0; ///< Rows: the same with the caption after the value
    int margin_px = 0; ///< space kept above the first and below the last row, in Columns and Rows
};

/// Pick the arrangement for a stacked widget: one decision for Temperatures
/// and Fan Speeds, which then draw it their own way.
///
/// Columns needs the authored width (kColumnsMinColspan), the physical width
/// band (ColumnWidths::min_box_px: where pixels and span disagree the compact
/// layout wins, as in wants_icon_above), measured pixels for
/// every visible row's column across the width, and the height for the icon,
/// value and (when it fits) the caption. The caption is dropped before the
/// layout is, so a short box keeps its columns. When Columns does not fit the
/// answer is the vertical rule, wants_icon_above(), unchanged.
[[nodiscard]] inline StackLayout classify_stack_layout(int colspan, int rowspan, int width_px,
                                                       int height_px, int row_count,
                                                       const StackedRowMetrics& row, int row_gap_px,
                                                       const ColumnWidths& cols) {
    StackLayout out;
    out.shape = wants_icon_above(rowspan, height_px, row_count, row, row_gap_px)
                    ? StackShape::IconAbove
                    : StackShape::Beside;

    if (colspan < kColumnsMinColspan || row_count < 2)
        return out;
    if (width_px <= 0 || height_px <= 0 || row.icon_px <= 0 || row.text_px <= 0)
        return out;
    if (width_px < cols.min_box_px)
        return out;
    // A box that is tall as well as wide already uses its height well stacked
    // vertically; columns take over once it is clearly wider than it is tall.
    const bool wide_enough_for_columns =
        out.shape != StackShape::IconAbove || width_px * 2 >= height_px * 3;

    auto width_fits = [&](int column_px) {
        return column_px > 0 && width_px >= row_count * column_px + (row_count - 1) * row_gap_px;
    };
    const int plain_h = row.icon_px + row.icon_gap_px + row.text_px;

    if (wide_enough_for_columns) {
        if (row.label_px > 0 && width_fits(cols.captioned_px) &&
            height_px >= plain_h + row.icon_gap_px + row.label_px + 2 * cols.margin_px) {
            return {StackShape::Columns, true};
        }
        if (width_fits(cols.plain_px) && height_px >= plain_h + 2 * cols.margin_px)
            return {StackShape::Columns, false};
    }

    // A box taller than a cell and wide enough: one row per heater down the
    // box, each a big glyph with its value (and caption) to the right. Only a
    // widget that measured a beside width for it offers this.
    if (rowspan >= kIconAboveMinRowspan && cols.beside_plain_px > 0) {
        const int rows_h =
            row_count * std::max(row.icon_px, row.text_px) + (row_count - 1) * row_gap_px;
        if (height_px >= rows_h + 2 * cols.margin_px) {
            if (row.label_px > 0 && width_px >= cols.beside_captioned_px)
                return {StackShape::Rows, true};
            if (width_px >= cols.beside_plain_px)
                return {StackShape::Rows, false};
        }
    }
    return out;
}

} // namespace helix
