// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How the print status panel arranges its job tile, printer tile and strip in
// the content box it was given. Pure: measured pixels in, arrangement out.

#include <cstdint>

namespace helix {

enum class PrintStatusArrangement { SideBySide = 0, Stacked = 1 };

/// Where the panel shows the filament system's lane tray, if anywhere: in
/// the ultrawide readouts' column in their place, or as a strip under the
/// stacked tiles' strip. The int values are what `ps_lane_tray` publishes.
enum class PrintStatusLaneTray { None = 0, Column = 1, Strip = 2 };

/// Column widths of the ultrawide arrangement, left to right: the square
/// preview, the info column, the printer picture, the readouts, the inline
/// Tune section and the button stack. The info column takes what the others
/// leave. `tune_w` 0 means no Tune column: the Tune button shows instead.
struct PrintStatusColumns {
    int32_t preview_w = 0;
    int32_t info_w = 0;
    int32_t printer_w = 0;
    int32_t readouts_w = 0;
    int32_t actions_w = 0;
    int32_t tune_w = 0;
};

struct PrintStatusTileLayout {
    PrintStatusArrangement arrangement = PrintStatusArrangement::SideBySide;
    bool strip_stacked = false;     ///< progress above the buttons instead of beside them
    bool checklist_compact = false; ///< current step + dots instead of the list
    int32_t job_w = 0, job_h = 0;   ///< the job tile's size, for the chip and font decisions
    int32_t printer_w = 0, printer_h = 0;
    int32_t strip_h = 0;
    int32_t tune_h = 0; ///< the stacked inline Tune section's height; 0: the Tune button shows
    PrintStatusColumns columns; ///< set by the ultrawide decision only
    PrintStatusLaneTray lane_tray = PrintStatusLaneTray::None;
    int32_t lane_tray_h = 0; ///< the Strip's height; 0 otherwise
};

/// `content_w` x `content_h` is the panel's content box, `gap` the token gap,
/// `strip_row_h` / `strip_stacked_h` the strip heights the current tier's fonts need.
/// Carve order: the strip comes off `content_h` first, then the tiles split
/// the rest (236:196 stacked, halves side by side), then any extra comes out
/// of the job tile alone. With `lane_tray`, stacked tiles give the lane tray
/// strip, up to its natural height, what the job tile can spare above the
/// height its checklist goes compact at, or above `job_content_h` (what the job
/// tile shows now needs, the pre-print checklist in full) when that is more.
/// The printer tile keeps the height it
/// has without the tray: too little room shrinks the tray or shows none,
/// never the picture. Side by side never shows the tray.
/// `tune_natural_h` is the inline Tune section's laid-out height (0 when not
/// yet measured). After the lane tray, stacked tiles give it that height out
/// of the job tile alone, down to the same floor; too little room shows none,
/// leaving the Tune button. Side by side never shows it.
[[nodiscard]] PrintStatusTileLayout decide_print_status_tile_layout(
    int32_t content_w, int32_t content_h, int32_t gap, int32_t strip_row_h, int32_t strip_stacked_h,
    bool lane_tray = false, int32_t job_content_h = 0, int32_t tune_natural_h = 0);

/// The ultrawide arrangement of a `content_w` x `content_h` box whose columns sit
/// `gap` apart: the preview square at the full height, the printer picture at
/// 1.4 x the height, the readouts, Tune and buttons proportional to it. When the
/// info column would fall below the height, the printer, then the readouts, then
/// the preview give up width toward their floors. The Tune column shows only
/// where the readouts, the preview and the info column can make room for it
/// and still keep most of the height's width; the printer picture keeps its
/// full width beside it. Without `tune` (no print running) there is no Tune
/// column to make room for.
[[nodiscard]] PrintStatusColumns decide_print_status_columns(int32_t content_w, int32_t content_h,
                                                             int32_t gap, bool tune = false);

/// The ultrawide tile layout: the columns, with the job tile the preview
/// square and the printer tile its column, both at the full height. With
/// `lane_tray` the readouts' column shows the lane tray instead, since the
/// printer picture's callouts already carry the readings. `tune` as for
/// decide_print_status_columns().
[[nodiscard]] PrintStatusTileLayout
decide_print_status_ultrawide_layout(int32_t content_w, int32_t content_h, int32_t gap,
                                     bool lane_tray = false, bool tune = false);

} // namespace helix
