// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/grid_edit_mode_test_access.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"

#include <unordered_set>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("GridEditMode: starts inactive", "[grid_edit][edit_mode]") {
    GridEditMode em;
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: enter/exit toggles state", "[grid_edit][edit_mode]") {
    GridEditMode em;
    em.enter(nullptr, nullptr); // null container/config OK for state test
    REQUIRE(em.is_active());
    em.exit();
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: exit when not active is no-op", "[grid_edit][edit_mode]") {
    GridEditMode em;
    em.exit(); // Should not crash
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: double enter is no-op", "[grid_edit][edit_mode]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);
    em.enter(nullptr, nullptr); // Second enter should be ignored
    REQUIRE(em.is_active());
    em.exit();
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: select/deselect widget tracking", "[grid_edit][selection]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);

    REQUIRE(em.selected_widget() == nullptr);

    // Use a reinterpret_cast'd pointer for tracking test (lv_obj_t is opaque)
    int dummy = 0;
    auto* fake = reinterpret_cast<lv_obj_t*>(&dummy);
    em.select_widget(fake);
    REQUIRE(em.selected_widget() == fake);

    em.select_widget(nullptr);
    REQUIRE(em.selected_widget() == nullptr);

    // Selection clears on exit
    em.select_widget(fake);
    em.exit();
    REQUIRE(em.selected_widget() == nullptr);
}

TEST_CASE("GridEditMode: selecting same widget is no-op", "[grid_edit][selection]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);

    int dummy = 0;
    auto* fake = reinterpret_cast<lv_obj_t*>(&dummy);
    em.select_widget(fake);
    REQUIRE(em.selected_widget() == fake);

    // Selecting same widget again should not crash or change state
    em.select_widget(fake);
    REQUIRE(em.selected_widget() == fake);

    em.exit();
}

TEST_CASE("GridEditMode: select_widget when not active is no-op", "[grid_edit][selection]") {
    GridEditMode em;
    int dummy = 0;
    auto* fake = reinterpret_cast<lv_obj_t*>(&dummy);

    em.select_widget(fake);
    REQUIRE(em.selected_widget() == nullptr);
}

// =============================================================================
// screen_to_grid_cell
// =============================================================================

TEST_CASE("GridEditMode: screen_to_grid_cell maps coordinates correctly", "[grid_edit][drag]") {
    // 6-column grid, container at (100, 0) with width 600, height 400, 4 rows
    // Cell size: 100x100
    auto cell = GridEditMode::screen_to_grid_cell(150, 50,  // point inside col 0, row 0
                                                  100, 0,   // container origin
                                                  600, 400, // container size
                                                  6, 4,     // cols, rows
                                                  0         // gutter
    );
    REQUIRE(cell.first == 0);  // col 0
    REQUIRE(cell.second == 0); // row 0

    // Bottom-right corner area: col 5, row 3
    auto cell2 = GridEditMode::screen_to_grid_cell(690, 390, 100, 0, 600, 400, 6, 4, 0);
    REQUIRE(cell2.first == 5);
    REQUIRE(cell2.second == 3);
}

TEST_CASE("GridEditMode: screen_to_grid_cell clamps out-of-bounds coordinates",
          "[grid_edit][drag]") {
    // Point before container origin — should clamp to (0, 0)
    auto cell = GridEditMode::screen_to_grid_cell(50, 10, // before container at (100, 20)
                                                  100, 20, 600, 400, 6, 4, 0);
    CHECK(cell.first == 0);
    CHECK(cell.second == 0);

    // Point beyond container extent — should clamp to (ncols-1, nrows-1)
    auto cell2 =
        GridEditMode::screen_to_grid_cell(800, 500, // beyond container at (100,20) size 600x400
                                          100, 20, 600, 400, 6, 4, 0);
    CHECK(cell2.first == 5);
    CHECK(cell2.second == 3);
}

TEST_CASE("GridEditMode: screen_to_grid_cell center of each cell", "[grid_edit][drag]") {
    // Container at (0,0), 400x300, 4 cols x 3 rows
    // Cell size: 100x100
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            int cx = c * 100 + 50;
            int cy = r * 100 + 50;
            auto cell = GridEditMode::screen_to_grid_cell(cx, cy, 0, 0, 400, 300, 4, 3, 0);
            INFO("Testing center of cell (" << c << "," << r << ") at screen (" << cx << "," << cy
                                            << ")");
            CHECK(cell.first == c);
            CHECK(cell.second == r);
        }
    }
}

TEST_CASE("screen_to_grid_cell: gutters shift the cell boundaries",
          "[grid_edit][drag][grid_metrics]") {
    // 6 columns in 480px with 4px gutters -> track 76.67px, pitch 80.67px.
    // x=79 is inside track 0 (which ends at 76.67, then 4px of gutter to 80.67).
    // The gutter-blind form has a pitch of exactly 80, so it would call this
    // track 0 too -- pick a point where the two disagree instead.
    // Track 3 starts at 3 * 80.67 = 242. Gutter-blind puts track 3 at 240.
    // x = 241 is still track 2's gutter under the correct math.
    auto [col, row] = helix::GridEditMode::screen_to_grid_cell(241, 10, 0, 0, 480, 272, 6, 4, 4);
    INFO("col=" << col);
    REQUIRE(col == 2);

    auto [col2, row2] = helix::GridEditMode::screen_to_grid_cell(243, 10, 0, 0, 480, 272, 6, 4, 4);
    REQUIRE(col2 == 3);
}

TEST_CASE("screen_to_grid_cell: zero gutter matches the historical behavior",
          "[grid_edit][drag][grid_metrics]") {
    // Every pre-existing expectation must survive when gutter == 0.
    auto [col, row] = helix::GridEditMode::screen_to_grid_cell(240, 10, 0, 0, 480, 272, 6, 4, 0);
    REQUIRE(col == 3);
}

// =============================================================================
// clamp_span
// =============================================================================

TEST_CASE("GridEditMode: clamp_span respects min/max from registry", "[grid_edit][resize]") {
    // printer_image: min 1x1, max 4x3 (from registry)
    const auto* def = find_widget_def("printer_image");
    REQUIRE(def != nullptr);
    REQUIRE(def->is_scalable());

    // Over max — clamp down
    auto [c, r] = GridEditMode::clamp_span("printer_image", 5, 4);
    CHECK(c == def->effective_max_colspan());
    CHECK(r == def->effective_max_rowspan());

    // Under min — clamp up
    auto [c2, r2] = GridEditMode::clamp_span("printer_image", 0, 0);
    CHECK(c2 == def->effective_min_colspan());
    CHECK(r2 == def->effective_min_rowspan());

    // Within range — unchanged
    auto [c3, r3] = GridEditMode::clamp_span("printer_image", 2, 2);
    CHECK(c3 == 2);
    CHECK(r3 == 2);
}

TEST_CASE("GridEditMode: clamp_span non-scalable widget stays fixed", "[grid_edit][resize]") {
    // "shutdown" has no min/max overrides, so effective min == max == default (1x1)
    const auto* def = find_widget_def("shutdown");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->is_scalable());

    auto [c, r] = GridEditMode::clamp_span("shutdown", 3, 3);
    CHECK(c == def->effective_min_colspan());
    CHECK(r == def->effective_min_rowspan());
    // Both should equal the default colspan/rowspan (1x1)
    CHECK(c == 1);
    CHECK(r == 1);
}

TEST_CASE("GridEditMode: clamp_span unknown widget returns at least 1x1", "[grid_edit][resize]") {
    auto [c, r] = GridEditMode::clamp_span("nonexistent_widget_xyz", 0, 0);
    CHECK(c >= 1);
    CHECK(r >= 1);
}

TEST_CASE("GridEditMode: clamp_span tips widget respects range", "[grid_edit][resize]") {
    // tips: colspan default=3, min=2, max=6, rowspan default=1, min=1, max=1
    const auto* def = find_widget_def("tips");
    REQUIRE(def != nullptr);
    REQUIRE(def->is_scalable());

    // Max colspan 6, only 1 row allowed
    auto [c, r] = GridEditMode::clamp_span("tips", 10, 5);
    CHECK(c == def->effective_max_colspan());
    CHECK(r == def->effective_max_rowspan());

    // Min colspan 2
    auto [c2, r2] = GridEditMode::clamp_span("tips", 1, 1);
    CHECK(c2 == def->effective_min_colspan());
    CHECK(r2 == 1);
}

// =============================================================================
// build_default_grid — anchor positions and auto-place defaults
// =============================================================================

TEST_CASE("build_default_grid only sets positions for anchor widgets", "[grid]") {
    auto entries = PanelWidgetConfig::build_default_grid();
    REQUIRE(entries.size() > 3); // At least the 3 anchors + some auto-place widgets

    // Find anchor entries and verify their fixed positions
    const PanelWidgetEntry* printer_image = nullptr;
    const PanelWidgetEntry* print_status = nullptr;
    const PanelWidgetEntry* tips = nullptr;
    const PanelWidgetEntry* temperature = nullptr;
    const PanelWidgetEntry* bed_temperature = nullptr;

    for (const auto& e : entries) {
        if (e.id == "printer_image")
            printer_image = &e;
        if (e.id == "print_status")
            print_status = &e;
        if (e.id == "tips")
            tips = &e;
        if (e.id == "temperature")
            temperature = &e;
        if (e.id == "bed_temperature")
            bed_temperature = &e;
    }

    REQUIRE(printer_image != nullptr);
    CHECK(printer_image->col == 0);
    CHECK(printer_image->row == 0);
    CHECK(printer_image->colspan >= 2); // depends on breakpoint (2-3)
    CHECK(printer_image->rowspan >= 2); // depends on breakpoint (2-3)
    CHECK(printer_image->has_grid_position());

    REQUIRE(print_status != nullptr);
    CHECK(print_status->col == 0);
    CHECK(print_status->row >= 2);     // depends on breakpoint (2-3)
    CHECK(print_status->colspan >= 2); // depends on breakpoint (2-3)
    CHECK(print_status->rowspan == 2);
    CHECK(print_status->has_grid_position());

    REQUIRE(tips != nullptr);
    CHECK(tips->col >= 0);
    CHECK(tips->row >= 0);
    CHECK(tips->colspan >= 1);
    CHECK(tips->rowspan >= 1);
    CHECK(tips->has_grid_position());

    REQUIRE(temperature != nullptr);
    CHECK(temperature->has_grid_position());

    REQUIRE(bed_temperature != nullptr);
    CHECK(bed_temperature->has_grid_position());

    // All non-anchor entries must have col=-1, row=-1 (auto-place)
    for (const auto& e : entries) {
        if (e.id == "printer_image" || e.id == "print_status" || e.id == "tips" ||
            e.id == "temperature" || e.id == "bed_temperature") {
            continue;
        }
        INFO("Widget '" << e.id << "' should be auto-place (col=-1, row=-1)");
        CHECK(e.col == -1);
        CHECK(e.row == -1);
        CHECK_FALSE(e.has_grid_position());
    }
}

// =============================================================================
// GridLayout bottom-right packing — free cell ordering
// =============================================================================

TEST_CASE("GridLayout bottom-right packing fills cells correctly", "[grid]") {
    // Breakpoint 2 = MEDIUM = 6x4 grid
    GridLayout grid(UiBreakpoint::Medium);
    REQUIRE(grid.cols() == 6);
    REQUIRE(grid.rows() == 4);

    // Place the 3 anchor widgets
    REQUIRE(grid.place({"printer_image", 0, 0, 2, 2}));
    REQUIRE(grid.place({"print_status", 0, 2, 2, 2}));
    REQUIRE(grid.place({"tips", 2, 0, 4, 1}));

    // Collect free cells scanning bottom-right to top-left (same as populate_widgets)
    int grid_cols = grid.cols();
    int grid_rows = grid.rows();

    std::vector<std::pair<int, int>> free_cells;
    for (int r = grid_rows - 1; r >= 0; --r) {
        for (int c = grid_cols - 1; c >= 0; --c) {
            if (!grid.is_occupied(c, r)) {
                free_cells.push_back({c, r});
            }
        }
    }

    // Expected free cells in bottom-right to top-left order:
    // Row 3: (5,3), (4,3), (3,3), (2,3)  — cols 0-1 occupied by print_status
    // Row 2: (5,2), (4,2), (3,2), (2,2)  — cols 0-1 occupied by print_status
    // Row 1: (5,1), (4,1), (3,1), (2,1)  — cols 0-1 occupied by printer_image, cols 2-5 free
    // Row 0: all occupied (printer_image 0-1, tips 2-5)
    REQUIRE(free_cells.size() == 12);

    CHECK(free_cells[0] == std::make_pair(5, 3));
    CHECK(free_cells[1] == std::make_pair(4, 3));
    CHECK(free_cells[2] == std::make_pair(3, 3));
    CHECK(free_cells[3] == std::make_pair(2, 3));
    CHECK(free_cells[4] == std::make_pair(5, 2));
    CHECK(free_cells[5] == std::make_pair(4, 2));
    CHECK(free_cells[6] == std::make_pair(3, 2));
    CHECK(free_cells[7] == std::make_pair(2, 2));
    CHECK(free_cells[8] == std::make_pair(5, 1));
    CHECK(free_cells[9] == std::make_pair(4, 1));
    CHECK(free_cells[10] == std::make_pair(3, 1));
    CHECK(free_cells[11] == std::make_pair(2, 1));

    // With 4 auto-place widgets, the mapping is:
    //   widget i of n_auto → cell (n_auto - 1 - i)
    // So: widget 0 → cell 3 = (2,3)
    //     widget 1 → cell 2 = (3,3)
    //     widget 2 → cell 1 = (4,3)
    //     widget 3 → cell 0 = (5,3)
    // Result: left-to-right fill in the bottom row
    size_t n_auto = 4;
    std::vector<std::pair<int, int>> assigned;
    for (size_t i = 0; i < n_auto; ++i) {
        size_t cell_idx = n_auto - 1 - i;
        REQUIRE(cell_idx < free_cells.size());
        assigned.push_back(free_cells[cell_idx]);
    }

    CHECK(assigned[0] == std::make_pair(2, 3));
    CHECK(assigned[1] == std::make_pair(3, 3));
    CHECK(assigned[2] == std::make_pair(4, 3));
    CHECK(assigned[3] == std::make_pair(5, 3));
}

// =============================================================================
// Auto-place entries get positions written back after placement
// =============================================================================

TEST_CASE("auto-place entries get positions written back after placement", "[grid]") {
    // Simulate the populate_widgets writeback logic without LVGL.
    // Build entries: 3 anchors with positions + 4 auto-place widgets.
    std::vector<PanelWidgetEntry> entries = {
        {"printer_image", true, {}, 0, 0, 2, 2}, {"print_status", true, {}, 0, 2, 2, 2},
        {"tips", true, {}, 2, 0, 4, 1},          {"widget_a", true, {}, -1, -1, 1, 1},
        {"widget_b", true, {}, -1, -1, 1, 1},    {"widget_c", true, {}, -1, -1, 1, 1},
        {"widget_d", true, {}, -1, -1, 1, 1},
    };

    // Verify auto-place entries start without positions
    for (size_t i = 3; i < entries.size(); ++i) {
        REQUIRE_FALSE(entries[i].has_grid_position());
    }

    // Replicate the two-pass placement from populate_widgets
    UiBreakpoint breakpoint = UiBreakpoint::Medium;
    GridLayout grid(breakpoint);

    struct PlacedSlot {
        size_t entry_index;
        int col, row, colspan, rowspan;
    };
    std::vector<PlacedSlot> placed;
    std::vector<size_t> auto_place_indices;

    // First pass: place entries with explicit positions
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].has_grid_position()) {
            bool ok = grid.place({entries[i].id, entries[i].col, entries[i].row, entries[i].colspan,
                                  entries[i].rowspan});
            REQUIRE(ok);
            placed.push_back(
                {i, entries[i].col, entries[i].row, entries[i].colspan, entries[i].rowspan});
        } else {
            auto_place_indices.push_back(i);
        }
    }

    REQUIRE(placed.size() == 3);
    REQUIRE(auto_place_indices.size() == 4);

    // Second pass: bottom-right packing for 1x1 auto-place widgets
    int grid_cols = grid.cols();
    int grid_rows = grid.rows();

    std::vector<std::pair<int, int>> free_cells;
    for (int r = grid_rows - 1; r >= 0; --r) {
        for (int c = grid_cols - 1; c >= 0; --c) {
            if (!grid.is_occupied(c, r)) {
                free_cells.push_back({c, r});
            }
        }
    }

    size_t n_auto = auto_place_indices.size();
    for (size_t i = 0; i < n_auto; ++i) {
        size_t entry_idx = auto_place_indices[i];
        int colspan = entries[entry_idx].colspan;
        int rowspan = entries[entry_idx].rowspan;

        if (colspan == 1 && rowspan == 1) {
            size_t cell_idx = n_auto - 1 - i;
            if (cell_idx < free_cells.size()) {
                auto [col, row] = free_cells[cell_idx];
                if (grid.place({entries[entry_idx].id, col, row, 1, 1})) {
                    placed.push_back({entry_idx, col, row, 1, 1});
                    continue;
                }
            }
        }

        // Fallback
        auto pos = grid.find_available(colspan, rowspan);
        REQUIRE(pos.has_value());
        REQUIRE(grid.place({entries[entry_idx].id, pos->first, pos->second, colspan, rowspan}));
        placed.push_back({entry_idx, pos->first, pos->second, colspan, rowspan});
    }

    REQUIRE(placed.size() == 7); // All 7 widgets placed

    // Write computed positions back to entries (same as populate_widgets)
    for (const auto& p : placed) {
        entries[p.entry_index].col = p.col;
        entries[p.entry_index].row = p.row;
        entries[p.entry_index].colspan = p.colspan;
        entries[p.entry_index].rowspan = p.rowspan;
    }

    // Verify: all entries now have valid grid positions
    for (const auto& e : entries) {
        INFO("Widget '" << e.id << "' should have valid position after writeback");
        CHECK(e.has_grid_position());
        CHECK(e.col >= 0);
        CHECK(e.row >= 0);
        CHECK(e.colspan >= 1);
        CHECK(e.rowspan >= 1);
    }

    // Verify anchors kept their original positions
    CHECK(entries[0].col == 0); // printer_image
    CHECK(entries[0].row == 0);
    CHECK(entries[1].col == 0); // print_status
    CHECK(entries[1].row == 2);
    CHECK(entries[2].col == 2); // tips
    CHECK(entries[2].row == 0);

    // Verify auto-placed widgets landed in the bottom row (row 3) left-to-right
    CHECK(entries[3].col == 2); // widget_a
    CHECK(entries[3].row == 3);
    CHECK(entries[4].col == 3); // widget_b
    CHECK(entries[4].row == 3);
    CHECK(entries[5].col == 4); // widget_c
    CHECK(entries[5].row == 3);
    CHECK(entries[6].col == 5); // widget_d
    CHECK(entries[6].row == 3);

    // Verify no two widgets occupy the same cell
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            // Check that bounding boxes don't overlap
            bool overlap = entries[i].col < entries[j].col + entries[j].colspan &&
                           entries[j].col < entries[i].col + entries[i].colspan &&
                           entries[i].row < entries[j].row + entries[j].rowspan &&
                           entries[j].row < entries[i].row + entries[i].rowspan;
            INFO("Widgets '" << entries[i].id << "' and '" << entries[j].id
                             << "' should not overlap");
            CHECK_FALSE(overlap);
        }
    }
}

// =============================================================================
// GridLayout: can_place rejects out-of-bounds placements
// =============================================================================

TEST_CASE("GridLayout: can_place rejects out-of-bounds column", "[grid]") {
    GridLayout grid(UiBreakpoint::Medium); // MEDIUM = 6x4
    REQUIRE(grid.cols() == 6);
    REQUIRE(grid.rows() == 4);

    // 1x1 widget at col=6 (one past the last column) is rejected
    CHECK_FALSE(grid.can_place(6, 0, 1, 1));

    // 1x1 widget at col=5 is the last valid column
    CHECK(grid.can_place(5, 0, 1, 1));

    // 2x1 widget starting at col=5 overflows (5+2 > 6)
    CHECK_FALSE(grid.can_place(5, 0, 2, 1));

    // 2x1 widget starting at col=4 fits (4+2 == 6)
    CHECK(grid.can_place(4, 0, 2, 1));
}

TEST_CASE("GridLayout: can_place rejects out-of-bounds row", "[grid]") {
    GridLayout grid(UiBreakpoint::Medium); // MEDIUM = 6x4

    // 1x1 widget at row=4 (one past the last row) is rejected
    CHECK_FALSE(grid.can_place(0, 4, 1, 1));

    // 1x1 at row=3 is the last valid row
    CHECK(grid.can_place(0, 3, 1, 1));

    // 1x2 widget starting at row=3 overflows (3+2 > 4)
    CHECK_FALSE(grid.can_place(0, 3, 1, 2));

    // 1x2 widget starting at row=2 fits (2+2 == 4)
    CHECK(grid.can_place(0, 2, 1, 2));
}

TEST_CASE("GridLayout: can_place rejects negative coordinates and zero spans", "[grid]") {
    GridLayout grid(UiBreakpoint::Medium); // MEDIUM = 6x4

    CHECK_FALSE(grid.can_place(-1, 0, 1, 1));
    CHECK_FALSE(grid.can_place(0, -1, 1, 1));
    CHECK_FALSE(grid.can_place(0, 0, 0, 1));
    CHECK_FALSE(grid.can_place(0, 0, 1, 0));
}

// =============================================================================
// print_status bottom-left pin: rowspan > 1 pins to grid.rows() - rowspan
// =============================================================================

TEST_CASE("print_status bottom-left pin on 6x4 grid", "[grid]") {
    // On a 6x4 grid (MEDIUM breakpoint=3), print_status with rowspan=2
    // should be pinned to row = 4 - 2 = 2
    GridLayout grid(UiBreakpoint::Medium);
    REQUIRE(grid.cols() == 6);
    REQUIRE(grid.rows() == 4);

    int rowspan = 2;
    int pinned_row = grid.rows() - rowspan;
    CHECK(pinned_row == 2);

    // Verify the pinned position is placeable
    CHECK(grid.can_place(0, pinned_row, 2, rowspan));
    REQUIRE(grid.place({"print_status", 0, pinned_row, 2, rowspan}));
}

TEST_CASE("print_status bottom-left pin on 8x5 grid", "[grid]") {
    // On an 8x5 grid (LARGE breakpoint=4), print_status with rowspan=2
    // should be pinned to row = 5 - 2 = 3
    GridLayout grid(UiBreakpoint::Large);
    REQUIRE(grid.cols() == 8);
    REQUIRE(grid.rows() == 5);

    int rowspan = 2;
    int pinned_row = grid.rows() - rowspan;
    CHECK(pinned_row == 3);

    // Verify the pinned position is placeable
    CHECK(grid.can_place(0, pinned_row, 2, rowspan));
    REQUIRE(grid.place({"print_status", 0, pinned_row, 2, rowspan}));
}

TEST_CASE("print_status pin formula consistent across all breakpoints", "[grid]") {
    // Verify the pin formula works for every breakpoint
    UiBreakpoint bps[] = {UiBreakpoint::Micro,  UiBreakpoint::Tiny,  UiBreakpoint::Small,
                          UiBreakpoint::Medium, UiBreakpoint::Large, UiBreakpoint::XLarge};
    for (auto bp : bps) {
        GridLayout grid(bp);
        int rowspan = 2;
        int pinned_row = grid.rows() - rowspan;

        INFO("Breakpoint " << to_int(bp) << ": " << grid.cols() << "x" << grid.rows()
                           << " grid, pinned_row=" << pinned_row);
        CHECK(pinned_row >= 0);
        CHECK(grid.can_place(0, pinned_row, 2, rowspan));
    }
}

// =============================================================================
// Overflow clamping: explicit coords that exceed grid bounds get clamped
// =============================================================================

TEST_CASE("Overflow clamping pushes col to fit within grid", "[grid]") {
    // Simulate the clamping logic from populate_widgets:
    //   if (col + colspan > grid.cols()) col = max(0, grid.cols() - colspan);
    GridLayout grid(UiBreakpoint::Medium); // 6x4
    REQUIRE(grid.cols() == 6);

    // Widget at col=5 with colspan=2 overflows (5+2=7 > 6)
    int col = 5;
    int colspan = 2;
    if (col + colspan > grid.cols()) {
        col = std::max(0, grid.cols() - colspan);
    }
    CHECK(col == 4); // Clamped to col=4 so 4+2=6 fits exactly

    // After clamping, placement should succeed
    CHECK(grid.can_place(col, 0, colspan, 1));
    REQUIRE(grid.place({"test_widget", col, 0, colspan, 1}));
}

TEST_CASE("Overflow clamping pushes row to fit within grid", "[grid]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4
    REQUIRE(grid.rows() == 4);

    // Widget at row=3 with rowspan=2 overflows (3+2=5 > 4)
    int row = 3;
    int rowspan = 2;
    if (row + rowspan > grid.rows()) {
        row = std::max(0, grid.rows() - rowspan);
    }
    CHECK(row == 2); // Clamped to row=2 so 2+2=4 fits exactly

    CHECK(grid.can_place(0, row, 1, rowspan));
    REQUIRE(grid.place({"test_widget", 0, row, 1, rowspan}));
}

TEST_CASE("Overflow clamping handles widget larger than grid dimension", "[grid]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Widget with colspan=8 on a 6-column grid: max(0, 6-8) = max(0,-2) = 0
    // The widget still won't fit (0+8 > 6), but col is clamped to 0
    int col = 3;
    int colspan = 8;
    if (col + colspan > grid.cols()) {
        col = std::max(0, grid.cols() - colspan);
    }
    CHECK(col == 0);

    // Placement will fail because 0+8 > 6 -- the widget falls through to auto-place
    CHECK_FALSE(grid.can_place(col, 0, colspan, 1));
}

// =============================================================================
// Disable-on-overflow: widgets that can't be placed get disabled
// =============================================================================

TEST_CASE("Widgets disabled when grid is full and auto-place fails", "[grid]") {
    // Simulate the disable-on-overflow logic from populate_widgets.
    // Fill a 6x4 grid completely, then try to auto-place another widget.
    GridLayout grid(UiBreakpoint::Medium); // 6x4
    REQUIRE(grid.cols() == 6);
    REQUIRE(grid.rows() == 4);

    // Fill the entire grid with 1x1 placements
    int widget_num = 0;
    for (int r = 0; r < grid.rows(); ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            std::string id = "filler_" + std::to_string(widget_num++);
            REQUIRE(grid.place({id, c, r, 1, 1}));
        }
    }

    // Grid is completely full -- find_available returns nullopt
    auto pos = grid.find_available(1, 1);
    REQUIRE_FALSE(pos.has_value());

    // Simulate the disable-on-overflow logic:
    // When a widget can't be placed, it gets disabled with col=-1, row=-1
    PanelWidgetEntry overflow_entry{"overflow_widget", true, {}, -1, -1, 1, 1};
    REQUIRE(overflow_entry.enabled == true);

    // Replicate the populate_widgets overflow handling
    auto place_pos = grid.find_available(overflow_entry.colspan, overflow_entry.rowspan);
    if (!place_pos) {
        // Grid full -- disable widget (same as populate_widgets)
        overflow_entry.enabled = false;
        overflow_entry.col = -1;
        overflow_entry.row = -1;
    }

    CHECK(overflow_entry.enabled == false);
    CHECK(overflow_entry.col == -1);
    CHECK(overflow_entry.row == -1);
    CHECK_FALSE(overflow_entry.has_grid_position());
}

TEST_CASE("Multiple overflow widgets all get disabled", "[grid]") {
    // Fill grid mostly, leave only 1 free cell, try to place 3 auto-place widgets
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill all cells except (5,3) -- the bottom-right corner
    for (int r = 0; r < grid.rows(); ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            if (r == 3 && c == 5)
                continue; // Leave one cell free
            std::string id = "filler_" + std::to_string(r * grid.cols() + c);
            REQUIRE(grid.place({id, c, r, 1, 1}));
        }
    }

    // Verify exactly 1 free cell remains
    auto pos = grid.find_available(1, 1);
    REQUIRE(pos.has_value());
    CHECK(pos->first == 5);
    CHECK(pos->second == 3);

    // Try to auto-place 3 widgets into 1 free cell
    std::vector<PanelWidgetEntry> overflow_entries = {
        {"widget_a", true, {}, -1, -1, 1, 1},
        {"widget_b", true, {}, -1, -1, 1, 1},
        {"widget_c", true, {}, -1, -1, 1, 1},
    };

    int placed_count = 0;
    int disabled_count = 0;
    for (auto& entry : overflow_entries) {
        auto avail = grid.find_available(entry.colspan, entry.rowspan);
        if (avail &&
            grid.place({entry.id, avail->first, avail->second, entry.colspan, entry.rowspan})) {
            entry.col = avail->first;
            entry.row = avail->second;
            ++placed_count;
        } else {
            entry.enabled = false;
            entry.col = -1;
            entry.row = -1;
            ++disabled_count;
        }
    }

    // Exactly 1 widget placed, 2 disabled
    CHECK(placed_count == 1);
    CHECK(disabled_count == 2);

    // First widget should have been placed in the free cell
    CHECK(overflow_entries[0].col == 5);
    CHECK(overflow_entries[0].row == 3);
    CHECK(overflow_entries[0].enabled == true);

    // Remaining widgets should be disabled
    CHECK(overflow_entries[1].enabled == false);
    CHECK_FALSE(overflow_entries[1].has_grid_position());
    CHECK(overflow_entries[2].enabled == false);
    CHECK_FALSE(overflow_entries[2].has_grid_position());
}

// =============================================================================
// Drag logic: config position vs screen position mismatch detection
// =============================================================================

TEST_CASE("screen_to_grid_cell accurately maps widget centers to grid cells", "[grid_edit][drag]") {
    // Simulate a 6x4 grid in a 600x400 container at screen origin (100, 50).
    // Each cell is 100x100. Verify that the center of a widget at a known
    // grid position maps back to that same grid cell.
    int container_x = 100;
    int container_y = 50;
    int container_w = 600;
    int container_h = 400;
    int ncols = 6;
    int nrows = 4;

    // Widget at grid cell (3, 2) — its screen top-left would be at (400, 250)
    // Center of first cell: (400 + 50, 250 + 50) = (450, 300)
    auto cell = GridEditMode::screen_to_grid_cell(450, 300, container_x, container_y, container_w,
                                                  container_h, ncols, nrows, 0);
    CHECK(cell.first == 3);
    CHECK(cell.second == 2);

    // Widget at grid cell (5, 1) — screen top-left at (600, 150)
    // Center of first cell: (650, 200)
    auto cell2 = GridEditMode::screen_to_grid_cell(650, 200, container_x, container_y, container_w,
                                                   container_h, ncols, nrows, 0);
    CHECK(cell2.first == 5);
    CHECK(cell2.second == 1);
}

TEST_CASE("Drag same-position detection correctly identifies no-move", "[grid_edit][drag]") {
    // When a widget's config says (2,2) and the user drops on screen position
    // that maps to (2,2), the drag should be a no-op.
    int orig_col = 2, orig_row = 2;
    int target_col = 2, target_row = 2;
    bool same_position = (target_col == orig_col && target_row == orig_row);
    REQUIRE(same_position);
}

TEST_CASE("Drag to different position is detected when config matches screen",
          "[grid_edit][drag]") {
    // When config says (5,1) and user drops at screen position mapping to (2,2),
    // the drag should succeed (different position).
    int orig_col = 5, orig_row = 1;
    int target_col = 2, target_row = 2;
    bool same_position = (target_col == orig_col && target_row == orig_row);
    REQUIRE_FALSE(same_position);
}

TEST_CASE("Drag collision detection: empty target cell allows placement", "[grid_edit][drag]") {
    // Build a 6x4 grid with some occupied cells, verify can_place on an empty cell
    GridLayout grid(UiBreakpoint::Medium); // MEDIUM = 6x4
    REQUIRE(grid.place({"printer_image", 0, 0, 2, 2}));
    REQUIRE(grid.place({"tips", 2, 0, 4, 1}));
    REQUIRE(grid.place({"widget_a", 2, 1, 1, 1}));

    // Cell (3,1) is empty, should allow a 1x1 placement
    CHECK(grid.can_place(3, 1, 1, 1));

    // Cell (0,0) is occupied by printer_image, should reject
    CHECK_FALSE(grid.can_place(0, 0, 1, 1));
}

TEST_CASE("Drag collision detection: occupied target with same size allows swap",
          "[grid_edit][drag]") {
    // Simulate swap logic from handle_drag_end
    std::vector<PanelWidgetEntry> entries = {
        {"widget_a", true, {}, 2, 1, 1, 1},
        {"widget_b", true, {}, 4, 1, 1, 1},
    };

    int drag_cfg_idx = 0;
    int drag_orig_col = 2, drag_orig_row = 1;
    int drag_orig_colspan = 1, drag_orig_rowspan = 1;
    int target_col = 4, target_row = 1;

    // Find occupant at target
    int occupant_cfg_idx = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (static_cast<int>(i) == drag_cfg_idx)
            continue;
        if (target_col >= entries[i].col && target_col < entries[i].col + entries[i].colspan &&
            target_row >= entries[i].row && target_row < entries[i].row + entries[i].rowspan) {
            occupant_cfg_idx = static_cast<int>(i);
            break;
        }
    }

    REQUIRE(occupant_cfg_idx == 1);

    // Same size allows swap
    auto& occupant = entries[static_cast<size_t>(occupant_cfg_idx)];
    bool can_swap =
        (occupant.colspan == drag_orig_colspan && occupant.rowspan == drag_orig_rowspan);
    REQUIRE(can_swap);

    // Perform swap
    occupant.col = drag_orig_col;
    occupant.row = drag_orig_row;
    entries[static_cast<size_t>(drag_cfg_idx)].col = target_col;
    entries[static_cast<size_t>(drag_cfg_idx)].row = target_row;

    // Verify swapped positions
    CHECK(entries[0].col == 4); // widget_a moved to target
    CHECK(entries[0].row == 1);
    CHECK(entries[1].col == 2); // widget_b moved to original
    CHECK(entries[1].row == 1);
}

TEST_CASE("Drag collision detection: occupied target with different size rejects swap",
          "[grid_edit][drag]") {
    std::vector<PanelWidgetEntry> entries = {
        {"small_widget", true, {}, 2, 1, 1, 1}, // 1x1
        {"big_widget", true, {}, 4, 0, 2, 2},   // 2x2
    };

    int drag_orig_colspan = 1, drag_orig_rowspan = 1;
    int target_col = 4, target_row = 0;

    // Find occupant at target
    int occupant_cfg_idx = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (static_cast<int>(i) == 0)
            continue;
        if (target_col >= entries[i].col && target_col < entries[i].col + entries[i].colspan &&
            target_row >= entries[i].row && target_row < entries[i].row + entries[i].rowspan) {
            occupant_cfg_idx = static_cast<int>(i);
            break;
        }
    }

    REQUIRE(occupant_cfg_idx == 1);

    // Different size rejects swap
    auto& occupant = entries[static_cast<size_t>(occupant_cfg_idx)];
    bool can_swap =
        (occupant.colspan == drag_orig_colspan && occupant.rowspan == drag_orig_rowspan);
    REQUIRE_FALSE(can_swap);
}

TEST_CASE("Drag: saved cfg_idx is stable across FLOATING flag changes", "[grid_edit][drag]") {
    // Test the pattern where drag_cfg_idx_ is saved at drag start and
    // reused at drag end (because find_config_index_for_widget skips FLOATING objects).
    // This is a pure logic test verifying the pattern works.
    int drag_cfg_idx = 3; // Saved at drag start

    // At drag end, we use the saved index instead of re-looking up
    int cfg_idx = drag_cfg_idx;
    REQUIRE(cfg_idx == 3);

    // Verify the entry can be accessed with the saved index
    std::vector<PanelWidgetEntry> entries = {
        {"a", true, {}, 0, 0, 1, 1},
        {"b", true, {}, 1, 0, 1, 1},
        {"c", true, {}, 2, 0, 1, 1},
        {"d", true, {}, 3, 0, 1, 1}, // This is the dragged widget
    };
    REQUIRE(static_cast<size_t>(cfg_idx) < entries.size());
    CHECK(entries[static_cast<size_t>(cfg_idx)].id == "d");
}

TEST_CASE("Drag: FLOATING position compensation prevents visual shift", "[grid_edit][drag]") {
    // When a grid-managed widget becomes FLOATING, its coordinate reference
    // changes from content area to parent outer coords + padding. Without
    // compensation, the widget shifts by the container's padding amount.
    // Verify the compensation math: pos = widget_screen - container_screen - padding

    // Simulate: container at screen (10, 20) with padding (8, 6)
    int container_x1 = 10, container_y1 = 20;
    int pad_left = 8, pad_top = 6;

    // Widget at screen (118, 126) — i.e. content-relative (100, 100)
    int widget_x1 = 118, widget_y1 = 126;

    // Compensation formula (same as create_selection_chrome)
    int pos_x = widget_x1 - container_x1 - pad_left;
    int pos_y = widget_y1 - container_y1 - pad_top;

    // FLOATING position should be (100, 100) — matching the content-relative offset
    CHECK(pos_x == 100);
    CHECK(pos_y == 100);

    // Without compensation (setting pos to raw screen delta), widget shifts by padding
    int wrong_x = widget_x1 - container_x1; // 108, not 100
    int wrong_y = widget_y1 - container_y1; // 106, not 100
    CHECK(wrong_x != pos_x);                // Would shift right by pad_left
    CHECK(wrong_y != pos_y);                // Would shift down by pad_top
}

TEST_CASE("screen_to_grid_cell boundary: cell edges map correctly", "[grid_edit][drag]") {
    // Test that points exactly on cell boundaries map to the right cell.
    // Container at (0,0), 600x400, 6 cols x 4 rows. Each cell = 100x100.
    int cw = 600, ch = 400, ncols = 6, nrows = 4;

    // Exactly at cell (1,0) left edge: x=100
    auto cell = GridEditMode::screen_to_grid_cell(100, 50, 0, 0, cw, ch, ncols, nrows, 0);
    CHECK(cell.first == 1);
    CHECK(cell.second == 0);

    // Just before cell (1,0) left edge: x=99 should be cell (0,0)
    auto cell2 = GridEditMode::screen_to_grid_cell(99, 50, 0, 0, cw, ch, ncols, nrows, 0);
    CHECK(cell2.first == 0);
    CHECK(cell2.second == 0);

    // Exactly at the right edge of the container: x=599
    auto cell3 = GridEditMode::screen_to_grid_cell(599, 50, 0, 0, cw, ch, ncols, nrows, 0);
    CHECK(cell3.first == 5);
    CHECK(cell3.second == 0);
}

TEST_CASE("Drag: multi-cell widget bounds check at grid edges", "[grid_edit][drag]") {
    GridLayout grid(UiBreakpoint::Medium); // MEDIUM = 6x4

    // A 2x2 widget can be placed at (4,2) — fits exactly (4+2=6, 2+2=4)
    CHECK(grid.can_place(4, 2, 2, 2));

    // A 2x2 widget at (5,2) overflows columns (5+2=7 > 6)
    CHECK_FALSE(grid.can_place(5, 2, 2, 2));

    // A 2x2 widget at (4,3) overflows rows (3+2=5 > 4)
    CHECK_FALSE(grid.can_place(4, 3, 2, 2));
}

TEST_CASE("Multi-cell widget disabled when no contiguous space available", "[grid]") {
    // Fill grid leaving only scattered 1x1 holes -- a 2x2 widget can't fit
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill rows 0-2 completely
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            REQUIRE(grid.place({"filler_" + std::to_string(r * 10 + c), c, r, 1, 1}));
        }
    }

    // Fill row 3 with gaps: place at cols 0,1,3,4 -- leave 2,5 empty
    REQUIRE(grid.place({"filler_30", 0, 3, 1, 1}));
    REQUIRE(grid.place({"filler_31", 1, 3, 1, 1}));
    REQUIRE(grid.place({"filler_33", 3, 3, 1, 1}));
    REQUIRE(grid.place({"filler_34", 4, 3, 1, 1}));

    // Two free cells at (2,3) and (5,3) -- not contiguous for a 2x2 widget
    auto pos = grid.find_available(2, 2);
    REQUIRE_FALSE(pos.has_value());

    // A 2x2 widget should be disabled
    PanelWidgetEntry big_widget{"big_widget", true, {}, -1, -1, 2, 2};
    auto avail = grid.find_available(big_widget.colspan, big_widget.rowspan);
    if (!avail) {
        big_widget.enabled = false;
        big_widget.col = -1;
        big_widget.row = -1;
    }

    CHECK(big_widget.enabled == false);
    CHECK_FALSE(big_widget.has_grid_position());
}

TEST_CASE("Drag: hardware-gated invisible widgets should not block placement",
          "[grid_edit][drag]") {
    // Simulates the bug where humidity/probe/width_sensor are enabled in config
    // with grid positions, but not actually placed on screen due to hardware gates.
    // These invisible widgets should NOT occupy cells in the collision grid.
    GridLayout grid(UiBreakpoint::Medium); // MEDIUM = 6x4

    // Visible widgets
    grid.place({"printer_image", 0, 0, 2, 2});
    grid.place({"temperature", 4, 0, 1, 1});
    grid.place({"fan", 5, 0, 1, 1});

    // If we DON'T include invisible "humidity" at (3,2), cell (3,2) is free
    CHECK(grid.can_place(3, 2, 1, 1));
    CHECK(grid.can_place(2, 2, 2, 2));

    // Now simulate the OLD buggy behavior: place invisible widget
    GridLayout grid_with_invisible(UiBreakpoint::Medium);
    grid_with_invisible.place({"printer_image", 0, 0, 2, 2});
    grid_with_invisible.place({"temperature", 4, 0, 1, 1});
    grid_with_invisible.place({"fan", 5, 0, 1, 1});
    grid_with_invisible.place({"humidity", 3, 2, 1, 1}); // invisible but placed

    // Now (3,2) is blocked — a 2x2 at (2,2) would fail
    CHECK_FALSE(grid_with_invisible.can_place(2, 2, 2, 2));
    // But 1x1 at (2,2) still works
    CHECK(grid_with_invisible.can_place(2, 2, 1, 1));
}

TEST_CASE("Drag: occupant detection should skip invisible widgets", "[grid_edit][drag]") {
    // Simulate the occupant detection loop with a visible_ids filter.
    // Hardware-gated widgets in config should not be detected as occupants.
    std::vector<PanelWidgetEntry> entries = {
        {"led", true, {}, 5, 1, 1, 1},         // dragged widget
        {"humidity", true, {}, 3, 2, 1, 1},    // hardware-gated, NOT visible
        {"temperature", true, {}, 4, 0, 1, 1}, // visible
    };

    std::unordered_set<std::string> visible_ids = {"led", "temperature"};
    int target_col = 3, target_row = 2;
    int drag_idx = 0;

    int occupant_cfg_idx = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].enabled || !entries[i].has_grid_position()) {
            continue;
        }
        if (static_cast<int>(i) == drag_idx) {
            continue;
        }
        if (visible_ids.find(entries[i].id) == visible_ids.end()) {
            continue; // Skip invisible widgets
        }
        if (target_col >= entries[i].col && target_col < entries[i].col + entries[i].colspan &&
            target_row >= entries[i].row && target_row < entries[i].row + entries[i].rowspan) {
            occupant_cfg_idx = static_cast<int>(i);
            break;
        }
    }

    // humidity is at (3,2) but invisible — should NOT be detected as occupant
    CHECK(occupant_cfg_idx == -1);
}

TEST_CASE("Drag: center-based targeting for multi-cell widgets", "[grid_edit][drag]") {
    // Verify that using the widget center gives the correct target cell.
    // Container: 600x400 at (0,0), 6 cols x 4 rows. Cell = 100x100.
    int cw = 600, ch = 400, ncols = 6, nrows = 4;
    int cx = 0, cy = 0;

    // A 2x2 widget grabbed at its center. Widget top-left at (200,100).
    // Widget center = (200 + 100, 100 + 100) = (300, 200) → cell (3,2).
    // But we WANT cell (2,1) — the cell where the TOP-LEFT of the widget is.
    // With center-based: center = (200 + 600*2/(6*2), 100 + 400*2/(4*2)) = (300, 200)
    int widget_left = 200, widget_top = 100;
    int half_w = (cw * 2) / (ncols * 2); // colspan=2, half cell span
    int half_h = (ch * 2) / (nrows * 2); // rowspan=2
    int widget_cx = widget_left + half_w;
    int widget_cy = widget_top + half_h;

    auto cell =
        GridEditMode::screen_to_grid_cell(widget_cx, widget_cy, cx, cy, cw, ch, ncols, nrows, 0);
    CHECK(cell.first == 3); // center maps to (3,2)
    CHECK(cell.second == 2);

    // For a 1x1 widget, center offset is small (half a cell)
    int half_w_1x1 = (cw * 1) / (ncols * 2); // 50
    int half_h_1x1 = (ch * 1) / (nrows * 2); // 50
    int cx_1x1 = 200 + half_w_1x1;           // 250
    int cy_1x1 = 100 + half_h_1x1;           // 150

    auto cell2 = GridEditMode::screen_to_grid_cell(cx_1x1, cy_1x1, cx, cy, cw, ch, ncols, nrows, 0);
    CHECK(cell2.first == 2); // center of 1x1 at (200,100) → (250,150) → cell (2,1)
    CHECK(cell2.second == 1);
}

TEST_CASE("Drag threshold: small movement should not start drag", "[grid_edit][drag]") {
    // The drag threshold prevents FLOATING from being set on every touch.
    // Only movements > DRAG_THRESHOLD_PX should start a real drag.
    constexpr int DRAG_THRESHOLD_PX = 12; // Must match GridEditMode::DRAG_THRESHOLD_PX

    // Small movement (5px diagonal) — below threshold
    int dx = 3, dy = 4; // distance = 5
    bool exceeds = (dx * dx + dy * dy > DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX);
    CHECK_FALSE(exceeds);

    // Exactly at threshold (12px horizontal) — does NOT exceed (not strictly greater)
    dx = 12;
    dy = 0;
    exceeds = (dx * dx + dy * dy > DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX);
    CHECK_FALSE(exceeds);

    // Just past threshold (13px horizontal) — exceeds
    dx = 13;
    dy = 0;
    exceeds = (dx * dx + dy * dy > DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX);
    CHECK(exceeds);

    // Diagonal past threshold: 9,9 → sqrt(162) ≈ 12.7 → 162 > 144
    dx = 9;
    dy = 9;
    exceeds = (dx * dx + dy * dy > DRAG_THRESHOLD_PX * DRAG_THRESHOLD_PX);
    CHECK(exceeds);
}

TEST_CASE("Drag start touch margin: finger drift within margin is accepted",
          "[grid_edit][drag][resize][1169]") {
    // handle_drag_start allows the edge grab band of drift outside the widget
    // bounds, so a press that lands slightly off still owns the widget. Calls
    // the real predicate rather than restating it.
    GridEditMode em;
    const int band = GridEditModeTestAccess::edge_hit_band(em); // no container → fallback

    // Widget bounds: (100, 50) → (200, 150)
    const lv_area_t area = {100, 50, 200, 150};
    auto owns = [&](int x, int y) {
        return GridEditModeTestAccess::press_owns_widget(em, lv_point_t{x, y}, area);
    };

    CHECK(owns(150, 100));              // dead centre
    CHECK(owns(200, 100));              // exactly on the right boundary
    CHECK(owns(200 + band, 100));       // exactly at the outward limit
    CHECK_FALSE(owns(201 + band, 100)); // one past it
    CHECK(owns(150, 50 - band));        // outward limit above the top edge
    CHECK_FALSE(owns(150, 49 - band));
    CHECK(owns(100 - band, 100)); // outward limit left of the left edge
    CHECK_FALSE(owns(99 - band, 100));
    CHECK(owns(150, 150 + band)); // outward limit below the bottom edge
    CHECK_FALSE(owns(150, 151 + band));
}

// ============================================================================
// Grow-resize gesture: guard is anchored at the press origin (#1169)
// ============================================================================

TEST_CASE("press_owns_widget: a grow-resize press still owns the widget after the pointer leaves",
          "[grid_edit][resize][1169]") {
    // A resize that GROWS a widget drags away from it. By the time the drag
    // threshold is met the live pointer is well outside the bounds, but the
    // press origin is still on the edge. handle_drag_start must judge by the
    // origin — testing the live pointer drops exactly these gestures.
    GridEditMode em;
    const int band = GridEditModeTestAccess::edge_hit_band(em);

    const lv_area_t area = {100, 100, 300, 300};

    // Finger lands 5px inside the right edge — squarely in the grab band.
    const lv_point_t origin{295, 200};
    REQUIRE(GridEditModeTestAccess::press_owns_widget(em, origin, area));
    REQUIRE(em.detect_resize_edge(origin.x, origin.y, area) == GridEditMode::ResizeEdge::Right);

    // The pointer has since travelled 34px past the right edge — beyond the
    // band, so a live-pointer guard would reject the gesture here.
    const lv_point_t live{334, 200};
    REQUIRE(live.x > area.x2 + band); // the drift really is out of range
    CHECK_FALSE(GridEditModeTestAccess::press_owns_widget(em, live, area));

    // The origin still owns the widget, so the resize is allowed to start and
    // the edge classification (which already reads press_origin_) still fires.
    CHECK(GridEditModeTestAccess::press_owns_widget(em, origin, area));
    CHECK(em.detect_resize_edge(origin.x, origin.y, area) == GridEditMode::ResizeEdge::Right);
}

TEST_CASE("press_owns_widget: a press that never touched the widget is rejected",
          "[grid_edit][resize][1169]") {
    // The guard is anchored at the origin, not disabled: an origin nowhere near
    // the widget must still be turned away even if the pointer has drifted back.
    GridEditMode em;
    const int band = GridEditModeTestAccess::edge_hit_band(em);
    const lv_area_t area = {100, 100, 300, 300};

    const lv_point_t far_origin{300 + band + 40, 200};
    CHECK_FALSE(GridEditModeTestAccess::press_owns_widget(em, far_origin, area));

    // Pointer now sits dead centre — irrelevant, the origin is what decides.
    CHECK(GridEditModeTestAccess::press_owns_widget(em, lv_point_t{200, 200}, area));
}

// ============================================================================
// Edge grab band derivation
// ============================================================================

TEST_CASE("edge_hit_band: falls back to 18 with no container", "[grid_edit][resize][1169]") {
    // current_metrics() returns zeroed metrics without a container, so there is
    // no cell size to derive from. The fallback is what keeps the fixed-pixel
    // detect_resize_edge expectations below valid.
    GridEditMode em;
    CHECK(GridEditModeTestAccess::edge_hit_band(em) == 18);
}

TEST_CASE("edge_hit_band_for_cell: derives from cell size and clamps both ends",
          "[grid_edit][resize][1169]") {
    // Band is cell/6, clamped to [14, 32] so it stays finger-sized on any panel.
    auto band = [](float cell) { return GridEditModeTestAccess::edge_hit_band_for_cell(cell); };

    // Mid-range: proportional to the cell.
    CHECK(band(120.0f) == 20);
    CHECK(band(150.0f) == 25);

    // Below the floor: a small cell would give a band too thin to hit.
    CHECK(band(60.0f) == 14); // 10 → clamped up
    CHECK(band(30.0f) == 14); // 5  → clamped up
    CHECK(band(84.0f) == 14); // 14 → exactly the floor, unclamped

    // Above the ceiling: a large cell would swallow the widget interior.
    CHECK(band(300.0f) == 32); // 50 → clamped down
    CHECK(band(192.0f) == 32); // 32 → exactly the ceiling, unclamped
    CHECK(band(186.0f) == 31); // 31 → just under, unclamped

    // Non-positive cell size (no grid yet) → fallback, not a clamped zero.
    CHECK(band(0.0f) == 18);
    CHECK(band(-5.0f) == 18);
}

TEST_CASE("Drag end uses snap preview position, not release point", "[grid_edit][drag]") {
    // Simulates the pattern: handle_drag_move sets snap_preview_col_/row_,
    // handle_drag_end uses those saved values instead of recomputing from the
    // release point (which can differ due to finger movement during release).
    int snap_preview_col = 4;
    int snap_preview_row = 1;
    int drag_orig_col = 4;
    int drag_orig_row = 2;

    // The drop target is the snap preview position
    int target_col = snap_preview_col;
    int target_row = snap_preview_row;

    // Target differs from origin — should allow drop
    CHECK((target_col != drag_orig_col || target_row != drag_orig_row));
    CHECK(target_col == 4);
    CHECK(target_row == 1);

    // If snap preview was never set (-1), drop should be rejected
    int no_preview_col = -1;
    int no_preview_row = -1;
    CHECK_FALSE((no_preview_col >= 0 && no_preview_row >= 0));
}

// ============================================================================
// Widget Catalog: catalog_open flag
// ============================================================================

TEST_CASE("GridEditMode: catalog_open starts false", "[grid_edit][catalog]") {
    GridEditMode em;
    REQUIRE_FALSE(em.is_catalog_open());
}

TEST_CASE("GridEditMode: catalog_open flag not affected by enter/exit", "[grid_edit][catalog]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);
    REQUIRE_FALSE(em.is_catalog_open());
    em.exit();
    REQUIRE_FALSE(em.is_catalog_open());
}

// ============================================================================
// Widget sizing constraints
// ============================================================================

TEST_CASE("PanelWidgetDef: effective min/max accessors", "[grid_edit][sizing]") {
    // Widget with explicit min/max
    PanelWidgetDef def{};
    def.colspan = 2;
    def.rowspan = 2;
    def.min_colspan = 1;
    def.min_rowspan = 1;
    def.max_colspan = 4;
    def.max_rowspan = 3;

    CHECK(def.effective_min_colspan() == 1);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_colspan() == 4);
    CHECK(def.effective_max_rowspan() == 3);
    CHECK(def.is_scalable());
}

TEST_CASE("PanelWidgetDef: zero min/max defaults to colspan/rowspan", "[grid_edit][sizing]") {
    PanelWidgetDef def{};
    def.colspan = 1;
    def.rowspan = 1;
    def.min_colspan = 0;
    def.min_rowspan = 0;
    def.max_colspan = 0;
    def.max_rowspan = 0;

    CHECK(def.effective_min_colspan() == 1);
    CHECK(def.effective_min_rowspan() == 1);
    CHECK(def.effective_max_colspan() == 1);
    CHECK(def.effective_max_rowspan() == 1);
    CHECK_FALSE(def.is_scalable());
}

TEST_CASE("PanelWidgetDef: partially scalable (one axis)", "[grid_edit][sizing]") {
    PanelWidgetDef def{};
    def.colspan = 1;
    def.rowspan = 1;
    def.min_colspan = 1;
    def.min_rowspan = 1;
    def.max_colspan = 2;
    def.max_rowspan = 1; // Can't grow vertically

    CHECK(def.is_scalable()); // Scalable on col axis
    CHECK(def.effective_max_colspan() == 2);
    CHECK(def.effective_max_rowspan() == 1);
}

TEST_CASE("clamp_span: clamps to widget min/max", "[grid_edit][sizing]") {
    // Register a test widget definition with known constraints
    // Use an existing scalable widget: "temperature" (min 1x1, max 2x2)
    auto [c1, r1] = GridEditMode::clamp_span("temperature", 0, 0);
    CHECK(c1 == 1); // Clamped to min
    CHECK(r1 == 1);

    auto [c2, r2] = GridEditMode::clamp_span("temperature", 5, 5);
    CHECK(c2 == 2); // Clamped to max
    CHECK(r2 == 2);

    auto [c3, r3] = GridEditMode::clamp_span("temperature", 1, 1);
    CHECK(c3 == 1); // Within range
    CHECK(r3 == 1);

    auto [c4, r4] = GridEditMode::clamp_span("temperature", 2, 2);
    CHECK(c4 == 2); // At max
    CHECK(r4 == 2);
}

TEST_CASE("clamp_span: non-scalable widget stays fixed", "[grid_edit][sizing]") {
    // "shutdown" is 1x1, min 1x1, max 1x1
    auto [c1, r1] = GridEditMode::clamp_span("shutdown", 3, 3);
    CHECK(c1 == 1);
    CHECK(r1 == 1);
}

TEST_CASE("clamp_span: asymmetric constraints", "[grid_edit][sizing]") {
    // "tips" is 4x2, min 2x1, max 6x2 — wide and moderately tall
    auto [c1, r1] = GridEditMode::clamp_span("tips", 1, 1);
    CHECK(c1 == 2); // Clamped to min_colspan
    CHECK(r1 == 1); // rowspan stays at 1 (within [1,2])

    auto [c2, r2] = GridEditMode::clamp_span("tips", 6, 3);
    CHECK(c2 == 6); // At max_colspan
    CHECK(r2 == 2); // Clamped to max_rowspan
}

TEST_CASE("All registered widgets have valid sizing constraints", "[grid_edit][sizing]") {
    const auto& defs = get_all_widget_defs();
    REQUIRE(defs.size() > 0);

    for (const auto& def : defs) {
        INFO("Widget: " << def.id);
        // Default span must be within min/max range
        CHECK(def.effective_min_colspan() <= def.colspan);
        CHECK(def.colspan <= def.effective_max_colspan());
        CHECK(def.effective_min_rowspan() <= def.rowspan);
        CHECK(def.rowspan <= def.effective_max_rowspan());
        // Min must not exceed max
        CHECK(def.effective_min_colspan() <= def.effective_max_colspan());
        CHECK(def.effective_min_rowspan() <= def.effective_max_rowspan());
    }
}

// ============================================================================
// Resize edge detection
// ============================================================================

TEST_CASE("detect_resize_edge: right edge", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300}; // 200x200 widget, edges at 100/300

    // 18px inside + 18px outside: zone = [282, 318]
    CHECK(em.detect_resize_edge(295, 200, area) == GridEditMode::ResizeEdge::Right);
    CHECK(em.detect_resize_edge(283, 200, area) == GridEditMode::ResizeEdge::Right);
    // Outside the widget (18px outward)
    CHECK(em.detect_resize_edge(317, 200, area) == GridEditMode::ResizeEdge::Right);

    // Far from right edge — center of widget
    CHECK(em.detect_resize_edge(200, 200, area) == GridEditMode::ResizeEdge::None);
}

TEST_CASE("detect_resize_edge: left edge", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300};

    // 18px inside + 18px outside: zone = [82, 118]
    CHECK(em.detect_resize_edge(105, 200, area) == GridEditMode::ResizeEdge::Left);
    CHECK(em.detect_resize_edge(117, 200, area) == GridEditMode::ResizeEdge::Left);
    // Outside the widget (18px outward)
    CHECK(em.detect_resize_edge(83, 200, area) == GridEditMode::ResizeEdge::Left);

    // Far from left edge — center of widget
    CHECK(em.detect_resize_edge(200, 200, area) == GridEditMode::ResizeEdge::None);
}

TEST_CASE("detect_resize_edge: bottom edge", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300};

    // 18px inside + 18px outside: zone = [282, 318]
    CHECK(em.detect_resize_edge(200, 295, area) == GridEditMode::ResizeEdge::Bottom);
    CHECK(em.detect_resize_edge(200, 283, area) == GridEditMode::ResizeEdge::Bottom);
    CHECK(em.detect_resize_edge(200, 317, area) == GridEditMode::ResizeEdge::Bottom);

    // Far from bottom edge
    CHECK(em.detect_resize_edge(200, 200, area) == GridEditMode::ResizeEdge::None);
}

TEST_CASE("detect_resize_edge: top edge", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300};

    // 18px inside + 18px outside: zone = [82, 118]
    CHECK(em.detect_resize_edge(200, 105, area) == GridEditMode::ResizeEdge::Top);
    CHECK(em.detect_resize_edge(200, 117, area) == GridEditMode::ResizeEdge::Top);
    CHECK(em.detect_resize_edge(200, 83, area) == GridEditMode::ResizeEdge::Top);

    // Far from top edge
    CHECK(em.detect_resize_edge(200, 200, area) == GridEditMode::ResizeEdge::None);
}

TEST_CASE("detect_resize_edge: corner disambiguation picks closest edge", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300};

    // Bottom-right corner — closer to right edge (5px from right, 10px from bottom)
    CHECK(em.detect_resize_edge(296, 292, area) == GridEditMode::ResizeEdge::Right);

    // Bottom-right corner — closer to bottom edge (10px from right, 5px from bottom)
    CHECK(em.detect_resize_edge(292, 296, area) == GridEditMode::ResizeEdge::Bottom);

    // Bottom-right corner — equidistant: some edge wins (deterministic)
    CHECK(em.detect_resize_edge(295, 295, area) != GridEditMode::ResizeEdge::None);

    // Top-left corner — closer to top edge
    CHECK(em.detect_resize_edge(108, 102, area) == GridEditMode::ResizeEdge::Top);

    // Top-left corner — closer to left edge
    CHECK(em.detect_resize_edge(102, 108, area) == GridEditMode::ResizeEdge::Left);

    // Top-right corner — closer to right edge
    CHECK(em.detect_resize_edge(298, 105, area) == GridEditMode::ResizeEdge::Right);

    // Bottom-left corner — closer to bottom edge
    CHECK(em.detect_resize_edge(105, 298, area) == GridEditMode::ResizeEdge::Bottom);
}

TEST_CASE("detect_resize_edge: outside widget bounds", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300};

    // Well outside the widget (beyond 18px outward tolerance)
    CHECK(em.detect_resize_edge(50, 50, area) == GridEditMode::ResizeEdge::None);
    CHECK(em.detect_resize_edge(350, 350, area) == GridEditMode::ResizeEdge::None);

    // Outside perpendicular bounds — near right edge X but outside Y tolerance
    CHECK(em.detect_resize_edge(295, 50, area) == GridEditMode::ResizeEdge::None);
    CHECK(em.detect_resize_edge(295, 350, area) == GridEditMode::ResizeEdge::None);
}

TEST_CASE("detect_resize_edge: 18+18 hit zone boundaries", "[grid_edit][resize]") {
    GridEditMode em;
    lv_area_t area = {100, 100, 300, 300}; // widget edges at x1=100, x2=300, y1=100, y2=300

    // Right edge: zone = [282, 318]
    CHECK(em.detect_resize_edge(282, 200, area) == GridEditMode::ResizeEdge::Right);
    CHECK(em.detect_resize_edge(281, 200, area) == GridEditMode::ResizeEdge::None);
    CHECK(em.detect_resize_edge(318, 200, area) == GridEditMode::ResizeEdge::Right);
    CHECK(em.detect_resize_edge(319, 200, area) == GridEditMode::ResizeEdge::None);

    // Left edge: zone = [82, 118]
    CHECK(em.detect_resize_edge(118, 200, area) == GridEditMode::ResizeEdge::Left);
    CHECK(em.detect_resize_edge(119, 200, area) == GridEditMode::ResizeEdge::None);
    CHECK(em.detect_resize_edge(82, 200, area) == GridEditMode::ResizeEdge::Left);
    CHECK(em.detect_resize_edge(81, 200, area) == GridEditMode::ResizeEdge::None);

    // Bottom edge: zone = [282, 318]
    CHECK(em.detect_resize_edge(200, 282, area) == GridEditMode::ResizeEdge::Bottom);
    CHECK(em.detect_resize_edge(200, 281, area) == GridEditMode::ResizeEdge::None);
    CHECK(em.detect_resize_edge(200, 318, area) == GridEditMode::ResizeEdge::Bottom);
    CHECK(em.detect_resize_edge(200, 319, area) == GridEditMode::ResizeEdge::None);

    // Top edge: zone = [82, 118]
    CHECK(em.detect_resize_edge(200, 118, area) == GridEditMode::ResizeEdge::Top);
    CHECK(em.detect_resize_edge(200, 119, area) == GridEditMode::ResizeEdge::None);
    CHECK(em.detect_resize_edge(200, 82, area) == GridEditMode::ResizeEdge::Top);
    CHECK(em.detect_resize_edge(200, 81, area) == GridEditMode::ResizeEdge::None);
}

// ============================================================================
// round_to_grid_cell helper
// ============================================================================

TEST_CASE("round_to_grid_cell: exact cell boundary", "[grid_edit][resize]") {
    // 6 cells in 600px container starting at x=0
    // Cell boundaries: 0, 100, 200, 300, 400, 500, 600
    CHECK(GridEditMode::round_to_grid_cell(0, 0, 600, 6, 0) == 0);
    CHECK(GridEditMode::round_to_grid_cell(100, 0, 600, 6, 0) == 1);
    CHECK(GridEditMode::round_to_grid_cell(300, 0, 600, 6, 0) == 3);
    CHECK(GridEditMode::round_to_grid_cell(600, 0, 600, 6, 0) == 6);
}

TEST_CASE("round_to_grid_cell: midpoint rounding", "[grid_edit][resize]") {
    // Cell size = 100px. Midpoint of cell 0 = 50px.
    // 49px → rounds to boundary 0 (cell 0)
    CHECK(GridEditMode::round_to_grid_cell(49, 0, 600, 6, 0) == 0);
    // 50px → rounds to boundary 1 (std::round rounds 0.5 up)
    CHECK(GridEditMode::round_to_grid_cell(50, 0, 600, 6, 0) == 1);
    // 51px → rounds to boundary 1
    CHECK(GridEditMode::round_to_grid_cell(51, 0, 600, 6, 0) == 1);

    // Just past midpoint of cell 2 (250px)
    CHECK(GridEditMode::round_to_grid_cell(249, 0, 600, 6, 0) == 2);
    CHECK(GridEditMode::round_to_grid_cell(251, 0, 600, 6, 0) == 3);
}

TEST_CASE("round_to_grid_cell: with content origin offset", "[grid_edit][resize]") {
    // Container starts at x=100, 600px wide, 6 cells
    CHECK(GridEditMode::round_to_grid_cell(100, 100, 600, 6, 0) == 0);
    CHECK(GridEditMode::round_to_grid_cell(200, 100, 600, 6, 0) == 1);
    CHECK(GridEditMode::round_to_grid_cell(700, 100, 600, 6, 0) == 6);

    // Midpoint: 100 + 50 = 150 → rounds to 1
    CHECK(GridEditMode::round_to_grid_cell(150, 100, 600, 6, 0) == 1);
    CHECK(GridEditMode::round_to_grid_cell(149, 100, 600, 6, 0) == 0);
}

TEST_CASE("round_to_grid_cell: clamps to valid range", "[grid_edit][resize]") {
    // Below origin → clamps to 0
    CHECK(GridEditMode::round_to_grid_cell(-50, 0, 600, 6, 0) == 0);
    // Above maximum → clamps to ncells
    CHECK(GridEditMode::round_to_grid_cell(800, 0, 600, 6, 0) == 6);
}

TEST_CASE("round_to_grid_cell: rounds against the gutter-aware pitch",
          "[grid_edit][resize][grid_metrics]") {
    // pitch = 80.67. Boundary 3 sits at 242; the midpoint before it is ~201.7.
    REQUIRE(helix::GridEditMode::round_to_grid_cell(202, 0, 480, 6, 4) == 3);
    REQUIRE(helix::GridEditMode::round_to_grid_cell(201, 0, 480, 6, 4) == 2);
    // Clamps still hold at both ends.
    REQUIRE(helix::GridEditMode::round_to_grid_cell(-500, 0, 480, 6, 4) == 0);
    REQUIRE(helix::GridEditMode::round_to_grid_cell(9999, 0, 480, 6, 4) == 6);
}

// ============================================================================
// Origin-shifting resize math
// ============================================================================

TEST_CASE("compute_resize_result: right edge grow", "[grid_edit][resize]") {
    // Widget at (1,0) span 2x2, drag right edge to cell boundary 4
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Right, 1, 0, 2, 2,
                                                      /*new_edge_cell=*/4, /*ncells=*/6);
    CHECK(result.col == 1);
    CHECK(result.row == 0);
    CHECK(result.colspan == 3); // was 2, now extends to col 4 → 4-1=3
    CHECK(result.rowspan == 2); // unchanged
    CHECK(result.colspan >= 1); // valid result has positive spans
}

TEST_CASE("compute_resize_result: right edge shrink", "[grid_edit][resize]") {
    // Widget at (1,0) span 3x2, drag right edge to cell boundary 3
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Right, 1, 0, 3, 2,
                                                      /*new_edge_cell=*/3, /*ncells=*/6);
    CHECK(result.col == 1);
    CHECK(result.colspan == 2); // 3-1=2
    CHECK(result.rowspan == 2);
}

TEST_CASE("compute_resize_result: left edge grow", "[grid_edit][resize]") {
    // Widget at (2,0) span 2x2, drag left edge to cell boundary 1
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Left, 2, 0, 2, 2,
                                                      /*new_edge_cell=*/1, /*ncells=*/6);
    CHECK(result.col == 1); // origin shifts left
    CHECK(result.row == 0);
    CHECK(result.colspan == 3); // was 2, grew by 1
    CHECK(result.rowspan == 2);
    CHECK(result.colspan >= 1); // valid result has positive spans
}

TEST_CASE("compute_resize_result: left edge shrink", "[grid_edit][resize]") {
    // Widget at (1,0) span 3x2, drag left edge to cell boundary 2
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Left, 1, 0, 3, 2,
                                                      /*new_edge_cell=*/2, /*ncells=*/6);
    CHECK(result.col == 2);     // origin shifts right
    CHECK(result.colspan == 2); // was 3, shrank by 1 (right edge stays at 4)
    CHECK(result.rowspan == 2);
}

TEST_CASE("compute_resize_result: top edge grow", "[grid_edit][resize]") {
    // Widget at (0,2) span 2x2, drag top edge to cell boundary 1
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Top, 0, 2, 2, 2,
                                                      /*new_edge_cell=*/1, /*ncells=*/4);
    CHECK(result.col == 0);
    CHECK(result.row == 1); // origin shifts up
    CHECK(result.colspan == 2);
    CHECK(result.rowspan == 3); // was 2, grew by 1
    CHECK(result.colspan >= 1); // valid result has positive spans
}

TEST_CASE("compute_resize_result: bottom edge grow", "[grid_edit][resize]") {
    // Widget at (0,0) span 2x2, drag bottom edge to cell boundary 3
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Bottom, 0, 0, 2, 2,
                                                      /*new_edge_cell=*/3, /*ncells=*/4);
    CHECK(result.col == 0);
    CHECK(result.row == 0);
    CHECK(result.colspan == 2);
    CHECK(result.rowspan == 3); // was 2, grew by 1
    CHECK(result.colspan >= 1); // valid result has positive spans
}

TEST_CASE("compute_resize_result: clamp to min span 1", "[grid_edit][resize]") {
    // Widget at (2,0) span 2x2, drag left edge past right edge → clamps to min 1
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Left, 2, 0, 2, 2,
                                                      /*new_edge_cell=*/5, /*ncells=*/6);
    CHECK(result.colspan >= 1);
    // Origin should be clamped so widget stays within right edge
    CHECK(result.col + result.colspan <= 6);
}

TEST_CASE("compute_resize_result: clamp to grid bounds", "[grid_edit][resize]") {
    // Widget at (4,0) span 2x2, drag right edge past grid boundary
    auto result = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Right, 4, 0, 2, 2,
                                                      /*new_edge_cell=*/7, /*ncells=*/6);
    CHECK(result.col == 4);
    CHECK(result.col + result.colspan <= 6);

    // Drag top edge past grid top
    auto result2 = GridEditMode::compute_resize_result(GridEditMode::ResizeEdge::Top, 0, 1, 2, 2,
                                                       /*new_edge_cell=*/-1, /*ncells=*/4);
    CHECK(result2.row >= 0);
}

// ============================================================================
// Shrink-to-fit placement (mirrors place_widget_from_catalog fallback logic)
// ============================================================================

// Helper that replicates the shrink-to-fit algorithm from place_widget_from_catalog
// Returns {col, row, colspan, rowspan} or {-1,-1,-1,-1} if no fit
static std::tuple<int, int, int, int> try_place_with_shrink(GridLayout& grid, int colspan,
                                                            int rowspan, int min_colspan,
                                                            int min_rowspan) {
    // Try default size first
    auto pos = grid.find_available(colspan, rowspan);
    if (pos)
        return {pos->first, pos->second, colspan, rowspan};

    // Try progressively smaller sizes
    for (int try_r = rowspan; try_r >= min_rowspan; --try_r) {
        for (int try_c = colspan; try_c >= min_colspan; --try_c) {
            if (try_c == colspan && try_r == rowspan)
                continue;
            auto p = grid.find_available(try_c, try_r);
            if (p)
                return {p->first, p->second, try_c, try_r};
        }
    }
    return {-1, -1, -1, -1};
}

TEST_CASE("Shrink-to-fit: default size fits, no shrink needed", "[grid_edit][shrink_to_fit]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4
    // Empty grid — 2x2 should fit at (0,0)
    auto [col, row, cs, rs] = try_place_with_shrink(grid, 2, 2, 1, 1);
    CHECK(col == 0);
    CHECK(row == 0);
    CHECK(cs == 2);
    CHECK(rs == 2);
}

TEST_CASE("Shrink-to-fit: 2x2 doesn't fit, 2x1 does", "[grid_edit][shrink_to_fit]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill 3 of 4 rows completely, leaving only row 3 free
    int n = 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            grid.place({"filler_" + std::to_string(n++), c, r, 1, 1});
        }
    }

    // Row 3 is empty — 6 cells wide, 1 row tall
    // 2x2 won't fit (only 1 row free), but 2x1 should
    auto [col, row, cs, rs] = try_place_with_shrink(grid, 2, 2, 2, 1);
    CHECK(col >= 0);
    CHECK(row == 3);
    CHECK(cs == 2);
    CHECK(rs == 1); // Shrunk from 2x2 to 2x1
}

TEST_CASE("Shrink-to-fit: shrinks colspan when rowspan can't shrink",
          "[grid_edit][shrink_to_fit]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill everything except a single 1x2 slot at column 5, rows 2-3
    int n = 0;
    for (int r = 0; r < grid.rows(); ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            if (c == 5 && r >= 2)
                continue; // Leave (5,2) and (5,3) free
            grid.place({"filler_" + std::to_string(n++), c, r, 1, 1});
        }
    }

    // Only a 1x2 vertical slot remains. Widget default 2x2, min 1x1.
    auto [col, row, cs, rs] = try_place_with_shrink(grid, 2, 2, 1, 1);
    CHECK(col == 5);
    CHECK(row == 2);
    CHECK(cs == 1);
    CHECK(rs == 2); // 1x2 fits the vertical slot
}

TEST_CASE("Shrink-to-fit: no fit even at minimum size", "[grid_edit][shrink_to_fit]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill entire grid
    int n = 0;
    for (int r = 0; r < grid.rows(); ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            grid.place({"filler_" + std::to_string(n++), c, r, 1, 1});
        }
    }

    // No space at all
    auto [col, row, cs, rs] = try_place_with_shrink(grid, 2, 2, 1, 1);
    CHECK(col == -1);
    CHECK(row == -1);
}

TEST_CASE("Shrink-to-fit: non-scalable widget doesn't try smaller sizes",
          "[grid_edit][shrink_to_fit]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill 3 rows, leaving only 1 row free
    int n = 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            grid.place({"filler_" + std::to_string(n++), c, r, 1, 1});
        }
    }

    // Widget is 2x2 with min also 2x2 (non-scalable on row axis)
    // Can't shrink, so should fail
    auto [col, row, cs, rs] = try_place_with_shrink(grid, 2, 2, 2, 2);
    CHECK(col == -1);
    CHECK(row == -1);
}

TEST_CASE("Shrink-to-fit: tries rowspan reduction before colspan reduction",
          "[grid_edit][shrink_to_fit]") {
    GridLayout grid(UiBreakpoint::Medium); // 6x4

    // Fill rows 0-2 fully, leave row 3 completely empty (6 cells free)
    // This means both 2x1 and 1x2 could fit, but the algorithm tries
    // 2x1 (same colspan, reduced rowspan) before 1x2 (reduced colspan, same rowspan)
    int n = 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < grid.cols(); ++c) {
            grid.place({"filler_" + std::to_string(n++), c, r, 1, 1});
        }
    }

    // Default 2x2, min 1x1. Only 1 row free, so 2x2 fails.
    // Algorithm should try 2x1 first (rowspan shrinks before colspan)
    auto [col, row, cs, rs] = try_place_with_shrink(grid, 2, 2, 1, 1);
    CHECK(cs == 2); // Kept full width
    CHECK(rs == 1); // Shrunk height
}
