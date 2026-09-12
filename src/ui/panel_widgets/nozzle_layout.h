// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout-decision logic for the Nozzle Temps dashboard widget.
//
// Deliberately free of LVGL / widget state so it can be unit-tested without a
// display, font subsystem, or UpdateQueue. The widget measures pixel widths
// (NozzleTempsWidget::on_size_changed) and feeds them here; this header decides
// how many columns to use, which label rung each row renders, and which font it
// is drawn in.
//
// The two spellings are the nozzle's name ("Nozzle 1") and its physical
// position ("Tool 1"). Which of them is narrower is a locale fact, not a
// property of either role: de spells them "Düse 1" and "Werkzeug 1", so the
// position label is the wider one there. ru spells them "Сопло 1" and
// "Инструмент 1", both long enough that a one-grid-square column holds
// neither, which is why the number and icon rungs exist rather than a tuning
// constant.

namespace helix {

/// Measured pixel width of one complete nozzle row, in one of the two fonts a
/// row can be drawn in. A row is its leading nozzle icon, optionally a text
/// label, the label-value gap, the widest value the row will ever show, and a
/// comfort margin — so each field here is a whole row, not a fragment.
struct NozzleRowWidths {
    int long_px = 0;   ///< icon + the nozzle name ("Nozzle 1")
    int short_px = 0;  ///< icon + the position label ("Tool 1")
    int number_px = 0; ///< icon + the bare 1-based display number ("1")
    int icon_px = 0;   ///< the icon alone, no label at all
};

/// Which label a row draws beside its icon. The values are the widget's
/// published `nozzle_row_label_mode` subject ints, so XML ref_values and this
/// enum stay in lockstep by construction.
enum class NozzleLabelMode {
    None = 0,   ///< icon only: no spelling, nor even a number, fits
    Number = 1, ///< icon + the bare 1-based display number
    Short = 2,  ///< the position label ("Tool 1")
    Long = 3,   ///< the nozzle name ("Nozzle 1")
};

struct NozzleLayoutDecision {
    int columns = 1; ///< 1 or 2 side-by-side columns of rows
    NozzleLabelMode label_mode = NozzleLabelMode::Long;
    /// Whether the row's text needs the smaller font. A separate question
    /// from label_mode: which rung won says nothing about whether the row it
    /// produced fits, and in a locale where the position label is the wider
    /// one the long form wins at every width.
    bool use_compact_font = false;
};

/// Total rendered height of everything the tile stacks — title, every nozzle
/// row plus the bed row, inter-row padding — at each font. The font tier is
/// gated on VERTICAL fit first: five rows that cannot fit the tile's height at
/// the normal font must drop to the compact one however wide the column is,
/// because no horizontal rung can buy back height. Width still co-decides:
/// the scan skips a tier whose rows the column cannot hold. Heights are
/// per-line, not per-tile: two columns wrap the rows, so the stack is the
/// base plus the wrapped line count, and a wide short tile keeps the normal
/// font where the single-column height would have vetoed it.
struct NozzleStackHeights {
    int base_px = 0;        ///< title + tile padding + render rounding
    int row_normal_px = 0;  ///< one row's height at the normal font, inter-row pad included
    int row_compact_px = 0; ///< the same at the compact font
};

/// Decide column count, label rung and font from measured pixel widths.
///
/// @param avail_px  usable inner width of the tile content area (after padding)
/// @param avail_h   usable inner height of the tile; <= 0 means unknown
///                  (pre-layout) and imposes no vertical constraint
/// @param gap_px    horizontal gap between two side-by-side rows
/// @param normal    row widths with the text drawn in the row's normal font
/// @param compact   the same rows with the text drawn in the compact font
/// @param row_count number of nozzle rows (columns clamped to this)
/// @param stack     per-font line heights (see NozzleStackHeights)
///
/// Pure arithmetic; no LVGL calls. A degenerate avail_px <= 0 (pre-layout)
/// returns the safe single-column long-label default.
[[nodiscard]] inline NozzleLayoutDecision
decide_nozzle_layout(int avail_px, int avail_h, int gap_px, const NozzleRowWidths& normal,
                     const NozzleRowWidths& compact, int row_count,
                     const NozzleStackHeights& stack) {
    if (avail_px <= 0)
        return {1, NozzleLabelMode::Long, false};

    // Two columns only when there are at least two rows AND both rows plus the
    // inter-row gap fit at the normal font. The narrower of the two spellings is
    // what a tight column actually renders, so that is the width two columns
    // have to hold; measuring the compact form where it is the wider one
    // measures a string the widget will never draw there.
    const int narrow_row_px = (normal.short_px < normal.long_px) ? normal.short_px : normal.long_px;
    int columns = (row_count >= 2 && avail_px >= 2 * narrow_row_px + gap_px) ? 2 : 1;

    // Never split a single row into two columns.
    if (columns > row_count)
        columns = row_count;
    if (columns < 1)
        columns = 1;

    const int col_w = (columns == 2) ? (avail_px - gap_px) / 2 : avail_px;

    // The long form when the column fits it, and also when the compact form is
    // the wider of the two: falling back to a spelling the column fits even
    // less would defeat the fallback.
    const bool use_long_label = (col_w >= normal.long_px) || (normal.long_px <= normal.short_px);
    const NozzleLabelMode text_mode =
        use_long_label ? NozzleLabelMode::Long : NozzleLabelMode::Short;

    // The ladder, in two independent decisions:
    //
    //   the FONT TIER is chosen by HEIGHT — the normal font only where the
    //   stack the tile will actually stack (base + wrapped lines at that
    //   font) fits, because no horizontal rung can buy back vertical room —
    //   with width able to fall through to the compact tier when the column
    //   holds none of the normal-font rungs;
    //
    //   the LABEL RUNG is chosen by WIDTH within the tier — the chosen
    //   spelling, then the bare number, then the icon alone — because a row
    //   is one text line tall whatever it says, so the label never costs
    //   height and richness is free until the column runs out.
    const int items = row_count + 1; // + the bed row
    const int lines = (columns == 2) ? (items + 1) / 2 : items;
    for (const bool tier_compact : {false, true}) {
        if (!tier_compact) {
            // Unknown height (pre-layout) allows the normal tier, matching
            // the degenerate default.
            if (avail_h > 0 && stack.base_px + lines * stack.row_normal_px > avail_h)
                continue;
        }
        const NozzleRowWidths& w = tier_compact ? compact : normal;
        const int rung_widths[3] = {
            use_long_label ? w.long_px : w.short_px,
            w.number_px,
            w.icon_px,
        };
        for (int rung = 0; rung < 3; rung++) {
            if (col_w >= rung_widths[rung]) {
                const NozzleLabelMode modes[3] = {text_mode, NozzleLabelMode::Number,
                                                  NozzleLabelMode::None};
                return {columns, modes[rung], tier_compact};
            }
        }
        // A tier the column cannot hold at all falls through to the compact
        // one, which is both shorter and narrower.
    }
    // Below the compact tier's icon row there is nothing left to drop.
    return {columns, NozzleLabelMode::None, true};
}

} // namespace helix
