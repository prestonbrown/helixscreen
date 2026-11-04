// SPDX-License-Identifier: GPL-3.0-or-later

#include "grid_edit_drop.h"

#include "grid_edit_cross_page.h"
#include "panel_widget_registry.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace helix {

DropResolution resolve_drop(const DropInput& in, const GridLayout& occupancy) {
    const bool on_next_page_slot = in.page_index >= in.page_count;
    DropResolution nothing;
    nothing.outcome =
        in.page_index != in.origin_page ? DropOutcome::ReturnToOrigin : DropOutcome::Cancel;

    if (cross_page_drop_creates_page(in.page_index, in.page_count, in.has_next_page_slot,
                                     in.past_right_border, in.past_left_border)) {
        DropResolution created;
        created.outcome = DropOutcome::CreatePage;
        if (on_next_page_slot) {
            created.col = in.target_col;
            created.row = in.target_row;
        } else if (in.past_left_border) {
            // The widget sits past the first page's left border, and the page
            // this creates lands before it. The border decides, not the page
            // index: a single page is both first and last.
            created.prepend_page = true;
            created.col = 0;
            created.row = std::max(in.target_row, 0);
        } else {
            // The widget sits past the last page's right border.
            created.col = occupancy.cols() - in.colspan;
            created.row = std::max(in.target_row, 0);
        }
        const GridLayout empty_page(occupancy.breakpoint(), occupancy.dimensions());
        return empty_page.can_place(created.col, created.row, in.colspan, in.rowspan) ? created
                                                                                      : nothing;
    }
    if (on_next_page_slot) {
        return nothing;
    }
    // A page change is itself a move, even onto the origin cell's coordinates.
    const bool moved = in.target_col != in.origin_col || in.target_row != in.origin_row ||
                       in.page_index != in.origin_page;
    if (in.target_col < 0 || in.target_row < 0 || !moved) {
        return nothing;
    }
    if (occupancy.can_place(in.target_col, in.target_row, in.colspan, in.rowspan)) {
        DropResolution move;
        move.outcome = DropOutcome::Move;
        move.col = in.target_col;
        move.row = in.target_row;
        return move;
    }
    // The occupant's new cell is the origin, which only the origin page has free.
    if (in.page_index != in.origin_page) {
        return nothing;
    }
    const GridPlacement* occupant = nullptr;
    for (const GridPlacement& p : occupancy.placements()) {
        const bool overlaps =
            p.col < in.target_col + in.colspan && in.target_col < p.col + p.colspan &&
            p.row < in.target_row + in.rowspan && in.target_row < p.row + p.rowspan;
        if (!overlaps) {
            continue;
        }
        if (occupant) {
            return nothing;
        }
        occupant = &p;
    }
    if (!occupant) {
        return nothing;
    }
    DropResolution swap;
    swap.outcome = DropOutcome::Swap;
    swap.col = occupant->col;
    swap.row = occupant->row;
    swap.swapped = {occupant->widget_id, in.origin_col, in.origin_row, occupant->colspan,
                    occupant->rowspan};
    GridLayout after = occupancy;
    after.remove(occupant->widget_id);
    if (!after.place({"", swap.col, swap.row, in.colspan, in.rowspan}) ||
        !after.can_place(swap.swapped.col, swap.swapped.row, swap.swapped.colspan,
                         swap.swapped.rowspan)) {
        return nothing;
    }
    return swap;
}

std::string placement_refusal(const PanelWidgetDef& def, const GridLayout& occupancy, int col,
                              int row, int colspan, int rowspan) {
    // Reported in cells, the unit a person places widgets in.
    const auto cells = [](int tracks) {
        return fmt::format("{:g}", static_cast<double>(tracks) / GridLayout::TRACKS_PER_CELL);
    };
    const auto span_refusal = [&](const char* axis, int span, int lo, int hi,
                                  bool half) -> std::string {
        if (span < lo || span > hi) {
            return fmt::format("{} {} is outside {}'s {}..{} cells", axis, cells(span), def.id,
                               cells(lo), cells(hi));
        }
        if (!half && span % GridLayout::TRACKS_PER_CELL != 0) {
            return fmt::format("{} {} is not whole cells, and {} has no half cells", axis,
                               cells(span), def.id);
        }
        return {};
    };
    std::string why = span_refusal("width", colspan, def.effective_min_colspan(),
                                   def.effective_max_colspan(), def.supports_half_col);
    if (why.empty()) {
        why = span_refusal("height", rowspan, def.effective_min_rowspan(),
                           def.effective_max_rowspan(), def.supports_half_row);
    }
    if (!why.empty()) {
        return why;
    }
    const auto where =
        fmt::format("{}x{} at ({}, {})", cells(colspan), cells(rowspan), cells(col), cells(row));
    if (col < 0 || row < 0 || col + colspan > occupancy.cols() ||
        row + rowspan > occupancy.rows()) {
        return fmt::format("{} leaves the {}x{} grid", where, cells(occupancy.cols()),
                           cells(occupancy.rows()));
    }
    if (!occupancy.can_place(col, row, colspan, rowspan)) {
        return where + " overlaps another widget";
    }
    return {};
}

} // namespace helix
