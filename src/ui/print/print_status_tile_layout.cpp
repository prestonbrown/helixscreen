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
} // namespace

PrintStatusTileLayout decide_print_status_tile_layout(int32_t content_w, int32_t content_h,
                                                      int32_t gap, int32_t strip_row_h,
                                                      int32_t strip_stacked_h) {
    PrintStatusTileLayout out;
    const bool stacked = content_h > content_w && content_h >= kStackedMinHeight;
    out.arrangement =
        stacked ? PrintStatusArrangement::Stacked : PrintStatusArrangement::SideBySide;
    out.strip_stacked = stacked || content_w < kStackedStripMaxWidth;
    out.strip_h = out.strip_stacked ? strip_stacked_h : strip_row_h;

    const int32_t area_h = std::max<int32_t>(0, content_h - out.strip_h - gap);
    if (stacked) {
        const int32_t usable = std::max<int32_t>(0, area_h - gap);
        out.job_w = out.printer_w = content_w;
        out.job_h = usable * kJobTileWeight / (kJobTileWeight + kPrinterTileWeight);
        out.printer_h = usable - out.job_h;
    } else {
        const int32_t usable = std::max<int32_t>(0, content_w - gap);
        out.job_w = usable / 2;
        out.printer_w = usable - out.job_w;
        out.job_h = out.printer_h = area_h;
    }
    out.checklist_compact = out.job_w < kCompactMinJobWidth || out.job_h < kCompactMinJobHeight;
    return out;
}

} // namespace helix
