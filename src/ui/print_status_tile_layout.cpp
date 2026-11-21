// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_tile_layout.h"

#include <algorithm>

namespace helix {

namespace {
constexpr int32_t kStackedMinHeight = 500;
constexpr int32_t kStackedStripMaxWidth = 400;
constexpr int32_t kJobTileWeight = 236;
constexpr int32_t kPrinterTileWeight = 196;
constexpr int32_t kCompactMinJobWidth = 180;
constexpr int32_t kCompactMinJobHeight = 170;

// Ultrawide column widths as percent of the content height. The printer
// picture's tagged images are about 1.4:1 with their callouts beside them;
// the readouts fit a label and a "220 / 220°C" value; the buttons a label.
constexpr int32_t kPrinterPct = 140;
constexpr int32_t kReadoutsPct = 93;
constexpr int32_t kActionsPct = 48;
// The floors the printer, readouts and preview give width down to, in that
// order, before the info column narrows below the height.
constexpr int32_t kPrinterFloorPct = 100;
constexpr int32_t kReadoutsFloorPct = 75;
constexpr int32_t kPreviewFloorPct = 75;

// The stacked lane tray strip: its natural height, and the smallest that
// still reads as lanes.
constexpr int32_t kLaneTrayMaxHeight = 120;
constexpr int32_t kLaneTrayMinHeight = 48;

bool is_compact_job(int32_t job_w, int32_t job_h) {
    return job_w < kCompactMinJobWidth || job_h < kCompactMinJobHeight;
}
} // namespace

PrintStatusTileLayout decide_print_status_tile_layout(int32_t content_w, int32_t content_h,
                                                      int32_t gap, int32_t strip_row_h,
                                                      int32_t strip_stacked_h, bool lane_tray) {
    PrintStatusTileLayout out;
    const bool stacked = content_h > content_w && content_h >= kStackedMinHeight;
    out.arrangement =
        stacked ? PrintStatusArrangement::Stacked : PrintStatusArrangement::SideBySide;
    out.strip_stacked = stacked || content_w < kStackedStripMaxWidth;
    out.strip_h = out.strip_stacked ? strip_stacked_h : strip_row_h;

    // Carve order, top to bottom of the decision:
    //  1. the strip and its gap come off the content height;
    //  2. the tiles split what is left, 236:196 stacked or halves side by side;
    //  3. extras come out of the job tile only, down to the height its
    //     checklist goes compact at. The printer tile keeps its step-2
    //     height: the picture is never squeezed to make room.
    const int32_t area_h = std::max<int32_t>(0, content_h - out.strip_h - gap);
    if (stacked) {
        const int32_t usable = std::max<int32_t>(0, area_h - gap);
        out.job_w = out.printer_w = content_w;
        out.job_h = usable * kJobTileWeight / (kJobTileWeight + kPrinterTileWeight);
        out.printer_h = usable - out.job_h;
        if (lane_tray) {
            // The tray strip and its gap, up to its natural height; less room
            // than its smallest shows none.
            const int32_t job_spare = std::max<int32_t>(0, out.job_h - kCompactMinJobHeight);
            const int32_t tray_h = std::min(kLaneTrayMaxHeight, job_spare - gap);
            if (tray_h >= kLaneTrayMinHeight) {
                out.job_h -= tray_h + gap;
                out.lane_tray = PrintStatusLaneTray::Strip;
                out.lane_tray_h = tray_h;
            }
        }
    } else {
        const int32_t usable = std::max<int32_t>(0, content_w - gap);
        out.job_w = usable / 2;
        out.printer_w = usable - out.job_w;
        out.job_h = out.printer_h = area_h;
    }
    out.checklist_compact = is_compact_job(out.job_w, out.job_h);
    return out;
}

PrintStatusColumns decide_print_status_columns(int32_t content_w, int32_t content_h, int32_t gap) {
    const int32_t h = std::max<int32_t>(0, content_h);
    PrintStatusColumns c;
    c.preview_w = h;
    c.printer_w = h * kPrinterPct / 100;
    c.readouts_w = h * kReadoutsPct / 100;
    c.actions_w = h * kActionsPct / 100;
    const auto rest = [&]() {
        return content_w - 4 * gap - c.preview_w - c.printer_w - c.readouts_w - c.actions_w;
    };
    int32_t deficit = h - rest();
    const auto give = [&](int32_t& col, int32_t floor_pct) {
        const int32_t spare = std::max<int32_t>(0, col - h * floor_pct / 100);
        const int32_t taken = std::clamp<int32_t>(deficit, 0, spare);
        col -= taken;
        deficit -= taken;
    };
    give(c.printer_w, kPrinterFloorPct);
    give(c.readouts_w, kReadoutsFloorPct);
    give(c.preview_w, kPreviewFloorPct);
    c.info_w = std::max<int32_t>(0, rest());
    return c;
}

PrintStatusTileLayout decide_print_status_ultrawide_layout(int32_t content_w, int32_t content_h,
                                                           int32_t gap, bool lane_tray) {
    PrintStatusTileLayout out;
    out.lane_tray = lane_tray ? PrintStatusLaneTray::Column : PrintStatusLaneTray::None;
    out.columns = decide_print_status_columns(content_w, content_h, gap);
    out.job_w = out.columns.preview_w;
    out.job_h = content_h;
    out.printer_w = out.columns.printer_w;
    out.printer_h = content_h;
    out.checklist_compact = is_compact_job(out.job_w, out.job_h);
    return out;
}

} // namespace helix
