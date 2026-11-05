// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_page_set.cpp
 * @brief Where the home carousel lands after edit mode changes its page set: on
 * the page that was on screen, renumbered, with one slide to the change's focus
 * (prestonbrown/helixscreen#1638).
 */

#include "grid_edit_page_set.h"

#include "../catch_amalgamated.hpp"

using helix::page_set_focus;
using helix::page_set_landing;
using helix::PageSetChange;
using helix::PageSetLanding;

namespace {

/// A change of @p page_count pages that adds a page or not, removes
/// @p removed_page (-1 for none) and asks to end on @p focus_page.
PageSetChange page_set_change_of(int page_count, bool page_added, bool page_prepended,
                                 int removed_page, int focus_page) {
    PageSetChange change;
    change.page_count = page_count;
    change.page_added = page_added;
    change.page_prepended = page_prepended;
    change.removed_page = removed_page;
    change.focus_page = focus_page;
    return change;
}

/// A change of @p page_count pages that puts a page back at @p inserted_page.
PageSetChange inserted_page_change(int page_count, int inserted_page) {
    PageSetChange change;
    change.page_count = page_count;
    change.inserted_page = inserted_page;
    change.focus_page = inserted_page;
    return change;
}

struct LandingRow {
    const char* path;
    PageSetChange change;
    int shown;       ///< The carousel's page when the rebuild runs
    int comes_up_on; ///< PageSetLanding::shown
    int ends_on;     ///< PageSetLanding::focus
};

} // namespace

TEST_CASE("a page-set rebuild comes up on the page on screen and slides once to its focus",
          "[1638][home]") {
    const LandingRow rows[] = {
        {"a drop onto the next-page slot", page_set_change_of(2, true, false, -1, 2), 2, 2, 2},
        {"a drop onto the next-page slot that empties its origin page",
         page_set_change_of(2, true, false, 1, 2), 2, 1, 1},
        {"a drop past the last page's border that keeps its origin page",
         page_set_change_of(1, true, false, -1, 1), 0, 0, 1},
        {"a drop past the last page's border that empties its origin page",
         page_set_change_of(2, true, false, 1, 2), 1, 1, 1},
        {"a drop past the first page's left border that keeps its origin page",
         page_set_change_of(2, false, true, -1, -1), 0, 1, 0},
        {"a drop past the first page's left border that empties its origin page",
         page_set_change_of(1, false, true, 0, -1), 0, 0, 0},
        {"a drop past the first page's left border from a later origin page that empties it",
         page_set_change_of(3, false, true, 2, -1), 0, 1, 0},
        {"a move that empties a page before its landing page",
         page_set_change_of(4, false, false, 1, 2), 2, 1, 1},
        {"a move that empties a page after its landing page",
         page_set_change_of(2, false, false, 1, 0), 0, 0, 0},
        {"removing the last widget of the last of three pages",
         page_set_change_of(3, false, false, 2, 2), 2, 1, 1},
        {"removing the last widget of the middle of three pages",
         page_set_change_of(3, false, false, 1, 1), 1, 1, 1},
        {"the delete-page button on the last of two pages",
         page_set_change_of(2, false, false, 1, 1), 1, 0, 0},
        {"a page on screen past the removed page comes up renumbered and slides to the focus",
         page_set_change_of(4, false, false, 1, 1), 3, 2, 1},
        {"a removed page on screen gives way to a focus on another page",
         page_set_change_of(4, false, false, 1, 3), 1, 2, 2},
        {"the next-page slot on screen with no page added there",
         page_set_change_of(2, false, false, -1, 1), 2, 1, 1},
        {"an undo that puts a middle page back, from the page that took its place",
         inserted_page_change(2, 1), 1, 2, 1},
        {"an undo that puts a middle page back, from the page before it",
         inserted_page_change(2, 1), 0, 0, 1},
        {"an undo that puts the last page back, from the page before it",
         inserted_page_change(2, 2), 1, 1, 2},
    };
    for (const LandingRow& row : rows) {
        INFO(row.path);
        const PageSetLanding landing = page_set_landing(row.change, row.shown);
        CHECK(landing.shown == row.comes_up_on);
        CHECK(landing.focus == row.ends_on);
        // The edit session's page, which the commit sets before the rebuild runs.
        CHECK(page_set_focus(row.change) == row.ends_on);
    }
}
