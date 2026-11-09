// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "maintenance_meters.h"

// Pure layout decision for the maintenance home tile.
//
// Free of LVGL so it is unit-testable without a display. MaintenanceWidget measures the
// minimum box each rung draws in and feeds it here. The tile offers Glance (the ring of
// the meter closest to due with its hours left inside), Strip (that meter's name and
// hours on one line), Column and Cell (the ring over the name), Wide (the ring beside a
// row per meter) and Full (a row and a bar per meter, under a title).

#include "src/ui/panel_widgets/size_rung.h"

#include <algorithm>

namespace helix {

struct MaintenanceTileLayoutInput {
    int content_w = 0; ///< the box the content draws in, chrome already subtracted
    int content_h = 0;
    RungNeeds needs; ///< offers glance, strip, column, cell, wide and full
    /// What the total line adds under the Full rows: its width as it draws now, and its
    /// height with the gap above it. It never ellipsizes; it shows whole or not at all.
    int total_line_w = 0;
    int total_line_h = 0;
    /// What sits beside or under the ring, for sizing it: the name line under it at
    /// Column and Cell (its height with the gap above), and the rows beside it at Wide
    /// (their width with the gap before them).
    int caption_h = 0;
    int list_w = 0;
    /// A ranked row's height at Wide and at Full, the gap above a row, and the Full title.
    int list_row_h = 1;
    int full_row_h = 1;
    int row_gap = 0;
    int full_title_h = 0;
};

struct MaintenanceTileVerdict {
    SizeRung rung = SizeRung::Glance;
    bool fits = false;
    bool show_total = false;
    /// The ring's diameter: the largest square the rung leaves it; 0 where no ring shows.
    int ring_d = 0;
    /// How many ranked rows Wide or Full holds; 0 elsewhere. The rung does not depend on it.
    int rows = 0;
};

[[nodiscard]] constexpr MaintenanceTileVerdict
decide_maintenance_tile_layout(const MaintenanceTileLayoutInput& in) {
    MaintenanceTileVerdict v;
    v.rung = choose_size_rung(in.content_w, in.content_h, in.needs);
    v.fits = holds_any_rung(in.content_w, in.content_h, in.needs);
    v.show_total = v.rung == SizeRung::Full && in.total_line_w <= in.content_w &&
                   in.content_h >= in.needs.full.h + in.total_line_h;
    switch (v.rung) {
    case SizeRung::Glance:
        v.ring_d = std::min(in.content_w, in.content_h);
        break;
    case SizeRung::Column:
    case SizeRung::Cell:
        v.ring_d = std::min(in.content_w, in.content_h - in.caption_h);
        break;
    case SizeRung::Wide:
        v.ring_d = std::min(in.content_w - in.list_w, in.content_h);
        break;
    default:
        break;
    }
    v.ring_d = std::max(v.ring_d, 0);
    if (v.rung == SizeRung::Wide) {
        v.rows = (in.content_h + in.row_gap) / (in.list_row_h + in.row_gap);
    } else if (v.rung == SizeRung::Full) {
        const int avail = in.content_h - in.full_title_h - (v.show_total ? in.total_line_h : 0);
        v.rows = avail / (in.full_row_h + in.row_gap);
    }
    if (v.rung == SizeRung::Wide || v.rung == SizeRung::Full)
        v.rows = std::clamp(v.rows, 1, maintenance::kTileRows);
    return v;
}

} // namespace helix
