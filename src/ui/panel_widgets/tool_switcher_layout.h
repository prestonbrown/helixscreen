// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure layout-decision logic for the home tool switcher.
//
// Free of LVGL so it can be unit-tested without a display. The widget measures
// its content box and fonts and feeds them here; this header decides whether
// the tile draws a rail of head segments (with or without its action cells,
// Park and the feeder), a mounted-head face beside Park, or the face alone, and
// how the rail's cells are arranged. It also sizes the picker's grid and the
// filament-colour band every cell carries. Every cell it hands back is at least min_target on both
// axes, so no mode ever draws a touch target smaller than the floor.

#include <algorithm>
#include <vector>

namespace helix {

/// The values are the widget's published mode subject ints. The XML compares
/// that subject against literal ref_values, which must match these values.
/// RailWithActions draws the action cells (Park, the feeder) after the heads.
enum class ToolSwitcherMode : int { RailWithActions = 0, Rail = 1, Pair = 2, Single = 3 };

/// The action cells the backend offers beside the heads, in rail order.
struct ToolSwitcherActions {
    bool park = false;   ///< it can dock the mounted head
    bool feeder = false; ///< it has a frame-side feeder the user opens and closes
    [[nodiscard]] int cells() const {
        return (park ? 1 : 0) + (feeder ? 1 : 0);
    }
};

struct ToolSwitcherLayout {
    ToolSwitcherMode mode = ToolSwitcherMode::Single;
    int cols = 1; ///< rail modes: grid columns; Pair: 2 along the long axis, else 1
    int rows = 1;
    int cell_w = 0; ///< px; >= min_target in every mode
    int cell_h = 0;
    int rail_h = 0; ///< px of the content box the rail or face occupies
    /// px the rail's columns span: whole cells and the gaps between them, so
    /// the rail's rounded corners land on its outer cells, not on the
    /// remainder equal cells leave of the box. The box's width outside the
    /// rail modes.
    int rail_w = 0;
    bool detail = false;         ///< a detail block fits under the rail
    bool pair_horizontal = true; ///< Pair: face and Park side by side (else stacked)
};

/// A grid of equal cells laid into a box: the first row count whose cells
/// clear the touch floor on both axes.
struct ToolSwitcherGrid {
    bool fits = false;
    int cols = 0;
    int rows = 0;
    int cell_w = 0;
    int cell_h = 0;
};

/// Lay `cells` equal cells into a w x h box. Rows are tried from one upward,
/// each with as many columns as it needs; more rows only ever shorten the
/// cells, so the search stops at the first row count whose cells are too
/// short.
[[nodiscard]] inline ToolSwitcherGrid fit_tool_switcher_cells(int cells, int w, int h,
                                                              int min_target, int gap) {
    for (int rows = 1; rows <= cells; rows++) {
        const int cols = (cells + rows - 1) / rows;
        const int cell_w = (w - gap * (cols - 1)) / cols;
        const int cell_h = (h - gap * (rows - 1)) / rows;
        if (cell_h < min_target)
            break;
        if (cell_w >= min_target)
            return {true, cols, rows, cell_w, cell_h};
    }
    return {};
}

/// Decide the tool switcher's mode and geometry from its content box.
///
/// @param tool_count  heads on the carriage's tool changer
/// @param content_w   usable inner width of the tile
/// @param content_h   usable inner height of the tile
/// @param min_target  touch floor: no cell is drawn smaller on either axis
/// @param gap         px between adjacent cells
/// @param body_px     line height of the breakpoint's body font
/// @param small_px    line height of the breakpoint's small font
/// @param actions     the action cells the backend offers; an absent one is
///                    never reserved
///
/// The first mode that fits wins: a segment per head plus every action cell, a
/// segment per head, the mounted head's face beside Park along the long axis,
/// and the face alone, which takes whatever box there is. RailWithActions needs
/// at least one action and Pair needs Park; the feeder is reached from the
/// picker in every mode that has no cell for it.
[[nodiscard]] inline ToolSwitcherLayout
decide_tool_switcher_layout(int tool_count, int content_w, int content_h, int min_target, int gap,
                            int body_px, int small_px, ToolSwitcherActions actions) {
    ToolSwitcherLayout out;
    if (content_w <= 0 || content_h <= 0)
        return out;
    out.cell_w = content_w;
    out.cell_h = content_h;
    out.rail_h = content_h;
    out.rail_w = content_w;
    if (tool_count < 1)
        return out;

    const ToolSwitcherMode rail_modes[2] = {ToolSwitcherMode::RailWithActions,
                                            ToolSwitcherMode::Rail};
    const int rail_cells[2] = {tool_count + actions.cells(), tool_count};
    for (int i = actions.cells() > 0 ? 0 : 1; i < 2; i++) {
        const ToolSwitcherGrid g =
            fit_tool_switcher_cells(rail_cells[i], content_w, content_h, min_target, gap);
        if (!g.fits)
            continue;
        out.mode = rail_modes[i];
        out.cols = g.cols;
        out.rows = g.rows;
        out.cell_w = g.cell_w;
        out.cell_h = g.cell_h;
        out.rail_w = g.cols * g.cell_w + gap * (g.cols - 1);

        // A tall box (two touch rows or more) sizes the rail's rows to their
        // content, up to half the box, and gives what is left to a detail
        // block when that holds two and a half lines of small text. A
        // one-row-tall tile keeps its full-height segments.
        const int gaps_h = gap * (g.rows - 1);
        int row_h = (content_h / 2 - gaps_h) / g.rows;
        if (row_h > body_px * 34 / 10)
            row_h = body_px * 34 / 10;
        if (row_h < min_target)
            row_h = min_target;
        const int rail_h = row_h * g.rows + gaps_h;
        if (content_h >= 2 * min_target && (content_h - rail_h) * 2 >= small_px * 5) {
            out.detail = true;
            out.cell_h = row_h;
            out.rail_h = rail_h;
        }
        return out;
    }

    // Pair: the face and a Park cell of 30% of the long axis, never under the
    // floor, with the face taking the rest.
    const bool horizontal = content_w >= content_h;
    const int long_px = horizontal ? content_w : content_h;
    const int short_px = horizontal ? content_h : content_w;
    int park_px = long_px * 3 / 10;
    if (park_px < min_target)
        park_px = min_target;
    const int face_px = long_px - gap - park_px;
    if (actions.park && short_px >= min_target && face_px >= min_target) {
        out.mode = ToolSwitcherMode::Pair;
        out.pair_horizontal = horizontal;
        out.cols = horizontal ? 2 : 1;
        out.rows = horizontal ? 1 : 2;
        out.cell_w = horizontal ? face_px : content_w;
        out.cell_h = horizontal ? content_h : face_px;
    }
    return out;
}

/// The filament-colour strip along the foot of a cell @p cell_h tall: about 9%
/// of it, rounded, and never under 3 px so it reads on the smallest cells.
[[nodiscard]] inline int tool_switcher_strip_px(int cell_h) {
    const int px = (cell_h * 9 + 50) / 100;
    return px < 3 ? 3 : px;
}

/// How a cell shows its head's filament colour along its foot.
struct ToolSwitcherBand {
    bool labelled = false; ///< the band carries the material name
    int px = 0;            ///< band height
};

/// The band for a cell @p cell_h tall. The lower 36% of the cell, carrying the
/// material name, where that holds one @p small_px line plus @p pad above and
/// below it; else a plain bar about one small line tall, capped at 30% of the
/// cell so the label above keeps its room, and never thinner than
/// tool_switcher_strip_px().
[[nodiscard]] inline ToolSwitcherBand tool_switcher_band(int cell_h, int small_px, int pad) {
    const int labelled_px = cell_h * 36 / 100;
    if (labelled_px >= small_px + 2 * pad)
        return {true, labelled_px};
    int px = std::min(small_px, cell_h * 3 / 10);
    return {false, std::max(px, tool_switcher_strip_px(cell_h))};
}

/// The picker's preferred cell: half again the touch floor, or the smallest
/// cell whose band carries the material name, whichever is larger.
[[nodiscard]] inline int tool_picker_preferred_cell(int min_target, int small_px, int pad) {
    int cell = min_target * 3 / 2;
    while (!tool_switcher_band(cell, small_px, pad).labelled)
        cell++;
    return cell;
}

/// How much label the feeder cell shows under its lock. The values are the
/// published subject's ints, which the cell XML compares against literal
/// ref_values.
enum class FeederLabelForm : int { Icon = 0, Short = 1, Long = 2 };

/// The fullest feeder label that fits @p room px: the action a tap takes
/// ("Open feeder" / "Close feeder", the wider measuring @p long_px), else
/// "Feeder" (@p short_px), else none, the lock alone carrying the state.
[[nodiscard]] inline FeederLabelForm feeder_label_form(int room, int long_px, int short_px) {
    if (long_px <= room)
        return FeederLabelForm::Long;
    if (short_px <= room)
        return FeederLabelForm::Short;
    return FeederLabelForm::Icon;
}

/// Pixels between adjacent cells of a rail or the picker grid.
inline constexpr int kToolRailGap = 2;

/// The look a head's cell takes. The values are the per-head state subject's
/// ints, which the cell XML compares against literal ref_values.
enum class ToolSwitcherCellState : int { Idle = 0, Mounted = 1, Fault = 2 };

/// Head @p index's look: the mounted head reads mounted whatever else is
/// wrong, and while the changer reports a fault every other head reads
/// faulted, since none of them can be trusted to be where the changer thinks.
[[nodiscard]] inline ToolSwitcherCellState tool_switcher_cell_state(int index, int active_tool,
                                                                    bool changer_fault) {
    if (index == active_tool)
        return ToolSwitcherCellState::Mounted;
    return changer_fault ? ToolSwitcherCellState::Fault : ToolSwitcherCellState::Idle;
}

/// The most columns the picker grid takes for @p heads: three up to six heads,
/// four from seven on, so eight heads take two rows and only a ninth starts a
/// third.
[[nodiscard]] inline int tool_picker_max_columns(int heads) {
    return heads <= 6 ? 3 : 4;
}

/// Columns for @p heads under a cap of @p max_cols: the fewest rows the cap
/// allows, then as few columns as fill those rows, so the rows balance (four
/// heads are 2x2, not 3+1).
[[nodiscard]] inline int tool_picker_balanced_columns(int heads, int max_cols) {
    if (heads < 1 || max_cols < 1)
        return 1;
    const int rows = (heads + max_cols - 1) / max_cols;
    return (heads + rows - 1) / rows;
}

/// The picker grid's columns for @p heads on a screen with room for the cap.
[[nodiscard]] inline int tool_picker_columns(int heads) {
    return tool_picker_balanced_columns(heads, tool_picker_max_columns(heads));
}

/// The picker's grid of square head cells.
struct ToolPickerGrid {
    int cols = 0;
    int rows = 0;
    int cell = 0;       ///< px, both axes
    int width = 0;      ///< px of the whole grid
    int height = 0;     ///< px of every row, which may exceed the visible height
    int max_height = 0; ///< px the grid may show before it scrolls
    bool scrolls = false;
};

/// Lay @p heads square cells of up to @p preferred px into @p avail_w x
/// @p avail_h. Starts at tool_picker_max_columns(), drops columns only while
/// even floor-sized cells would not fit the width, then balances the rows
/// under that cap (tool_picker_balanced_columns()); shrinks the cells toward
/// @p min_target until every row fits the height, and scrolls what still does
/// not. No cell is ever under @p min_target.
[[nodiscard]] inline ToolPickerGrid fit_tool_picker_grid(int heads, int avail_w, int avail_h,
                                                         int min_target, int gap, int preferred) {
    ToolPickerGrid g;
    if (heads < 1)
        return g;
    int cols = tool_picker_max_columns(heads);
    while (cols > 1 && cols * min_target + (cols - 1) * gap > avail_w)
        cols--;
    cols = tool_picker_balanced_columns(heads, cols);
    const int rows = (heads + cols - 1) / cols;
    int cell = std::min(preferred, (avail_w - (cols - 1) * gap) / cols);
    const int fit_h = (avail_h - (rows - 1) * gap) / rows;
    cell = std::min(cell, fit_h);
    cell = std::max(cell, min_target);
    g.cols = cols;
    g.rows = rows;
    g.cell = cell;
    g.width = cols * cell + (cols - 1) * gap;
    g.height = rows * cell + (rows - 1) * gap;
    g.scrolls = g.height > avail_h;
    // A scrolling grid shows whole rows where at least one fits.
    g.max_height = g.height;
    if (g.scrolls) {
        const int visible = std::max(1, (avail_h + gap) / (cell + gap));
        g.max_height = visible * cell + (visible - 1) * gap;
    }
    return g;
}

/// A detail row that is the empty-carriage line rather than a head.
inline constexpr int kToolSwitcherEmptyCarriageRow = -1;

/// The detail block's rows in display order, as tool indices: the mounted head
/// first, then the others by index. With the carriage empty (@p active_tool
/// -1) the first row is kToolSwitcherEmptyCarriageRow instead. An active index
/// the tool list does not hold names no head, so every head keeps its order.
[[nodiscard]] inline std::vector<int> tool_switcher_detail_order(int tool_count, int active_tool) {
    std::vector<int> order;
    if (tool_count < 1)
        return order;
    const bool mounted = active_tool >= 0 && active_tool < tool_count;
    if (mounted)
        order.push_back(active_tool);
    else if (active_tool == -1)
        order.push_back(kToolSwitcherEmptyCarriageRow);
    for (int i = 0; i < tool_count; i++) {
        if (i != active_tool)
            order.push_back(i);
    }
    return order;
}

/// How many of @p rows detail rows fit in @p avail_h px: the first is a body
/// line, each after it a small line. Rows that do not fit are dropped whole.
[[nodiscard]] inline int tool_switcher_detail_rows_that_fit(int rows, int avail_h, int body_px,
                                                            int small_px) {
    int used = 0;
    for (int i = 0; i < rows; i++) {
        used += i == 0 ? body_px : small_px;
        if (used > avail_h)
            return i;
    }
    return rows;
}

} // namespace helix
